// v0.3.2 (Phase 2): WHERE does the silent replay (VerifyJob / GameSimulator) differ from the real game? MEASUREMENT ONLY: nothing in this file
// moves a click or changes how anything is stepped.
//
// Pure C++, no Geode. The game side (Hooks.cpp / Jobs.cpp) only feeds facts in; every decision and every line of text is made here, so it can be
// tested with plain numbers (tests/test_core.cpp) and against the fake game (tests/verify_sim_test.cpp).
//
// TICK CONVENTION, the same in both runs: the sample of tick t is the state at the START of tick t, BEFORE the inputs of tick t are applied.
//  - silent replay: VerifyJob::stepOne pushes Baseline::samples[t] first, then applies the inputs with editedTick <= t, then steps.
//  - real playback: the processCommands hook (Hooks.cpp) takes the sample at the top of the call, before feedReplay applies the inputs.
// "Shift k" means silent[t] is compared with real[t + k]: k > 0 = the silent replay is AHEAD of the real game (its tick t already shows
// what the real game shows at tick t + k), k < 0 = it is BEHIND.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "Model.hpp"

namespace gdfwr {

inline constexpr float kTraceEpsilon = 1e-3f;               // two values "differ" when |a - b| is larger than this (x, y, yVel of both players)
inline constexpr int kTraceMaxShift = 3;                    // shift test: k = -3 .. +3
inline constexpr size_t kTraceMaxCalls = 240 * 60 * 12;     // 12 minutes of level time; a longer playback keeps its first part and says so
inline constexpr int kTraceMaxTick = 240 * 3600;            // a call that claims a tick beyond one hour of level time is not used
inline constexpr int kTraceNearClicks = 5;                  // clicks nearest the death that get their own facts in the log
inline constexpr int kTraceEdgeBefore = 3, kTraceEdgeAfter = 12;   // window around a click edge: edge - 3 .. edge + 12
inline constexpr int kTraceTailMin = 4;                     // ticks after the first difference needed before a shift can be judged
inline constexpr int kTraceBlipMax = 3;                   // at most this many differing ticks (and equal ticks after them) = a one-off difference, not a divergence
inline constexpr double kTraceShiftWholeFrac = 0.98;        // a shift explains the whole run when this share of the ticks is equal
inline constexpr double kTraceShiftTailFrac = 0.90;         // ... or the part after the first difference

// ---- the real playback, one entry per processCommands call (Hooks.cpp -> Jobs.cpp replayBegin / replayEnd) ----------------------------------
namespace TraceFlag { inline constexpr uint8_t HalfTick = 1, LastTick = 2; }   // the isHalfTick / isLastTick arguments the game passed
struct TraceCall {
    int32_t tick = 0;          // tickOf(): llround(m_levelTime * 240) at the START of the call
    double clock = 0;          // m_levelTime * 240 at the start, not rounded (a whole number = a whole tick)
    double clockAfter = 0;     // the same after the game's own processCommands returned: did IT move the level clock?
    float dt = 0;              // the dt argument of the call
    float timeWarp = 1;        // m_gameState.m_timeWarp
    uint32_t frame = 0;        // display frames seen since the trace began (RecHud::update): calls with the same number ran in one frame
    int32_t fedFirst = -1;     // index in the replay's event list of the first input applied at the start of this call (-1 = none)
    uint16_t fedCount = 0;     // how many inputs were applied at the start of this call
    uint8_t flags = 0;         // TraceFlag
    Sample s;                  // x / y / yVel of both players at the start of the call, BEFORE the inputs of this tick
};
enum class TraceEnd : uint8_t { Running = 0, Completed, Died, Left, Analyze };
const char* traceEndText(TraceEnd e);   // "the level was completed" ...

struct RealTrace {
    bool recording = false;            // calls are being added (a replay is running and the setting is on)
    bool truncated = false;            // kTraceMaxCalls reached: the rest of the playback is not in the trace
    TraceEnd end = TraceEnd::Running;
    uint64_t macroHash = 0;            // computeMacroHash of the macro that was played
    int64_t levelId = 0;
    std::string levelName;
    size_t events = 0;                 // inputs in the replay's event list
    uint32_t frame = 0;                // display frames since the trace began
    std::vector<float> frameDt;        // seconds of each display frame
    std::vector<TraceCall> calls;

    void begin(uint64_t hash, int64_t level, std::string name, size_t eventCount);   // clears everything and starts recording
    void finish(TraceEnd why) { if (recording) { recording = false; end = why; } }
    bool any() const { return !calls.empty(); }
    // Adds a call; returns its index, or -1 (not recording / full). The sample must be taken BEFORE the inputs of the tick are applied.
    int beginCall(int tick, double clock, float dt, float timeWarp, uint8_t flags, Sample const& s);
    void noteFed(int idx, int first, int count);
    void endCall(int idx, double clockAfter);
    void noteFrame(float dt);          // once per display frame (RecHud::update)
};

// ---- the real trace as ONE sample per tick (the first call that saw the tick = the state at the start of that tick) -------------------------
struct TickSeries {
    std::vector<Sample> s;
    std::vector<uint8_t> have;         // 0 = no call saw this tick
    std::vector<int32_t> call;         // index into RealTrace::calls of the first call of the tick, -1 = none
    std::vector<uint16_t> count;       // how many calls saw this tick (2 or more: a half tick, or a clock that did not move)
    int size() const { return int(s.size()); }
    bool has(int t) const { return t >= 0 && t < size() && have[size_t(t)] != 0; }
    int ticksWithData() const;
};
TickSeries tickSeries(RealTrace const& tr);

// Facts about HOW the real game called processCommands (the "frame structure").
struct TraceStats {
    size_t calls = 0, halfCalls = 0, lastCalls = 0, fractional = 0;   // fractional: the clock was not a whole number of ticks at the start
    int firstTick = 0, lastTick = 0, ticksSeen = 0, skippedTicks = 0, repeatedCalls = 0, backwards = 0, wild = 0;
    size_t frames = 0;                 // display frames that contain calls (and have a dt)
    double fpsAvg = 0, fpsMin = 0;     // display frames per second while calls were running
    std::vector<std::pair<int, int>> stepsPerFrame;   // (calls in one frame, number of such frames), the partial first and last frame left out
    size_t clockInside1 = 0, clockInsideHalf = 0, clockInsideNone = 0, clockInsideOther = 0;   // did the game's own call move the clock by 1 tick, 0.5, 0, other
    double stepMedian = 0, stepMin = 0, stepMax = 0;  // clock distance between two consecutive calls, in ticks (1 = 240 TPS)
    size_t stepPairs = 0, stepNormal = 0;             // pairs, and those within 0.2 % of 1 tick
    double tps = 0;                    // 240 / stepMedian
    float dtMin = 0, dtMax = 0, twMin = 1, twMax = 1;
};
TraceStats traceStats(RealTrace const& tr);

// ---- the tick-by-tick comparison -----------------------------------------------------------------------------------------------------------------
struct ShiftStat {
    int k = 0;
    int compared = 0, matched = 0;     // ticks of the silent replay that have a real tick at t + k, and those that are equal within epsilon
    int firstMismatch = -1;            // first silent tick that is not equal at this shift
    int runStart = -1, runLen = 0;     // the longest run of consecutive equal ticks (silent ticks)
    int tailCompared = 0, tailMatched = 0;   // the same, only from the first difference at k = 0 on
};
enum class TraceVerdict : uint8_t { NoData, Identical, ShiftedWhole, ShiftedFromFirstDiff, TooShortToTell, NotAShift };

struct TraceCompare {
    bool ok = false;                   // a comparison was possible
    std::string why;                   // when !ok
    float eps = kTraceEpsilon;
    int simTicks = 0;                  // samples of the silent replay (ticks 0 .. simTicks-1)
    int realTicks = 0;                 // size of the real series (ticks 0 .. realTicks-1; some may have no data)
    int compared = 0;                  // ticks compared at k = 0
    int missingReal = 0;               // silent ticks inside the real range that the real run never saw
    int simBeyondReal = 0;             // silent ticks after the end of the real trace
    int firstAny = -1;                 // first tick where a value is not exactly equal (any size)
    int firstOver = -1;                // first tick where a value differs by MORE than epsilon
    Sample simAt, realAt;              // the values at firstOver
    int overCount = 0;                 // ticks (of those compared) that differ by more than epsilon
    std::vector<ShiftStat> shifts;     // k = -3 .. +3, in that order
    TraceVerdict verdict = TraceVerdict::NoData;
    int verdictK = 0;                  // the shift of ShiftedWhole / ShiftedFromFirstDiff
    int bestK = 0;                     // the shift with the most equal ticks (ties: the smaller |k|, then the positive one)
    // The same shift test on the VERTICAL movement only (y and yVel of both players; x is ignored). A click that reaches the player k ticks earlier in one
    // run than in the other leaves x in step and shifts the jump: the whole-state test above cannot see that, this one can.
    std::vector<ShiftStat> vshifts;    // k = -3 .. +3
    int vFirstOver = -1;               // first tick where y / yVel differ by more than epsilon (k = 0)
    int xFirstOver = -1;               // first tick where x of either player differs by more than epsilon (k = 0)
    int vbestK = 0;
    TraceVerdict vverdict = TraceVerdict::NoData;   // Identical = y / yVel never differ at k = 0
    int vverdictK = 0;
    // the silent replay died during tick deathTick: its state right after that step against the real state at the start of the next tick
    bool deathKnown = false;
    int deathTick = -1;
    Sample deathSim, deathReal;
    bool deathRealHave = false, deathEqual = false;
    bool realContinued = false;        // the real run has a tick after deathTick (it survived that step)
};
// sim[t] = state at the start of tick t of the silent replay. deathTick < 0: the silent replay did not die.
TraceCompare compareSamples(std::vector<Sample> const& sim, TickSeries const& real, float eps = kTraceEpsilon, int deathTick = -1,
                            Sample const* deathAfter = nullptr);
bool sampleDiffers(Sample const& a, Sample const& b, float eps);
bool verticalDiffers(Sample const& a, Sample const& b, float eps);   // y and yVel of both players only (x is ignored)

// ---- facts around the clicks nearest a tick (the death) ---------------------------------------------------------------------------------------
struct LocalCompare {                  // the silent replay against the real trace inside a window of ticks
    int lo = 0, hi = -1;
    int compared = 0;
    int firstOver = -1;                // k = 0
    Sample simAt, realAt;
    int bestK = 0, bestMatched = 0;
    std::vector<int> matchedPerShift;  // k = -3 .. +3
    int vBestK = 0, vBestMatched = 0;  // the same for the vertical movement only (y, yVel; x ignored)
    std::vector<int> vMatchedPerShift; // k = -3 .. +3
};
struct EdgeFacts {
    bool press = true;
    int tick = 0;                      // the macro tick; the silent replay applies the input at this tick (stepOne: editedTick <= t)
    bool realApplied = false;          // the real playback applied it
    int realTick = -1;                 // tick of the real call that applied it
    uint8_t realFlags = 0;
    int realCall = -1;
    LocalCompare local;
    std::string callsAround;           // the real calls at tick - 1 .. tick + 1 in short form: "2394(L f612) 2395(L f613 +1)"
};
struct ClickFacts {
    int player = 0, number = 0;        // number: 1-based, inside the player's stream
    int pressTick = 0, releaseTick = -1;
    int distance = 0;                  // to the reference tick: 0 = the player was holding it
    EdgeFacts press, release;
    bool hasRelease = false;
};
std::vector<ClickFacts> nearestClickFacts(Macro const& macro, std::vector<Sample> const& sim, TickSeries const& real, RealTrace const& tr,
                                          int refTick, int n = kTraceNearClicks, float eps = kTraceEpsilon);

// ---- the report --------------------------------------------------------------------------------------------------------------------------------------
struct SimRun {                        // what the silent replay did
    std::vector<Sample> samples;       // Baseline::samples (index = tick)
    uint64_t macroHash = 0;
    bool died = false;
    int deathTick = -1;
    bool hasDeathSample = false;
    Sample deathSample;                // its state right after the dying step
    std::string killer;                // "Killed by: ..."
    int endTick = -1;                  // reached the end of the level at this tick (-1 = did not)
    std::string stepMode;              // "Direct" / "Update"
    bool stepKick = false;
    std::string stepProbe;             // "direct +60.0, update +60.0"
    uint64_t steps = 0, clockForced = 0;           // stepTick: steps, and steps where the MOD had to move the level clock itself
    double clockAdvMin = 0, clockAdvMax = 0;       // how far (ticks) the game's own call moved the clock inside one step
    uint64_t calls = 0, halfCalls = 0, lastCalls = 0;   // processCommands calls the hook saw while the silent replay ran
    float dtMin = 0, dtMax = 0;        // the dt those calls got
};
struct FileFacts {
    std::string levelName;
    int64_t levelId = 0;
    bool twoPlayer = false;
    int clickResolution = 1;           // what the file says (ticks); 1 = exact
    bool cbf = false;                  // the file was recorded with click-between-steps
    int tps = 240;
    float fpsAvg = 0, fpsMin = 0;      // while recording
    int inputs = 0;
};
struct TraceInput {
    RealTrace const* real = nullptr;   // nullptr: no trace exists
    std::string unavailable;           // not empty: why no comparison is made (setting off, other macro ...)
    SimRun sim;
    Macro const* macro = nullptr;
    FileFacts file;
    bool clickBetweenSteps = false;    // the mod setting "Clicks are exact to 1 tick"
    std::string modVersion;
    float eps = kTraceEpsilon;
};
struct TraceReport {
    bool compared = false;
    TraceCompare cmp;
    TraceStats stats;
    std::vector<std::string> popup;    // short, for the "Silent replay failed" popup
    std::vector<std::string> result;   // for the result screen
    std::vector<std::string> log;      // everything, one Geode log line each
    std::string callsCsv, compareCsv;  // the numbers (empty unless compared)
    std::string headline;              // one line
};
TraceReport buildTraceReport(TraceInput const& in);

// Short, one-line summary of the real trace, for the log when a playback ends: "GDFWR-TRACE real playback: ..."
std::vector<std::string> describeRealTrace(RealTrace const& tr);

// Text helpers
std::vector<std::string> wrapLines(std::string const& text, size_t width, std::string const& indent = "   ");   // keeps '\n', wraps at spaces
std::string sampleText(Sample const& s, int player, bool withNames = true);   // "x 3141.5900  y 105.0000  yVel -3.2000"
std::string stepsPerFrameText(std::vector<std::pair<int, int>> const& v, size_t maxItems = 6);

}  // namespace gdfwr
