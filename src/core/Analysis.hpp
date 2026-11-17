// Analysis engine (Part 4 + Part 5, steps 4 to 6). Pure logic over an abstract simulator, so the
// rules can be tested without the game. The engine is resumable: step() performs ONE shifted run.
#pragma once
#include <array>
#include <climits>
#include <deque>
#include <functional>

#include "Model.hpp"

namespace gdfwr {

struct Action {
    int tick = 0;
    uint8_t player = 0;
    bool press = true;
    uint32_t seq = 0;
    bool synthetic = false;  // added by the mod (not by the player)
};
std::vector<Action> makeActions(const std::vector<Event>& finalEvents);
// Ticks at which the verification pass must save a checkpoint: max(0, edge - cap) for every action edge (4.15).
// With a long search (reach > cap) the far start max(0, edge - reach) is saved too: offsets above cap start from it,
// so the common runs (offsets up to cap) stay short.
std::vector<int> requiredCheckpointTicks(const std::vector<Action>& actions, int cap = kOffsetCap, int reach = 0);

// ---- streamed analysis (v2) -----------------------------------------------------------------------------------
// Memory bound: instead of keeping one saved state for EVERY click edge at once, the clicks are cut into chunks. For each chunk the
// level is replayed again from tick 0 (uninterrupted, compared with the baseline), only that chunk's states are kept, its clicks are
// measured, and the states are freed. The main replay is never interrupted to restore a state, so the baseline cannot be damaged by
// an imperfect restore (the reason this is not an inline ring buffer).
struct ClickEdges { int player = 0, index = 0, press = 0, release = -1; };   // release -1: the run ends while holding
// Every click in the order of AnalysisData::clicks (all clicks of player 1, then all of player 2).
std::vector<ClickEdges> listClicks(const std::vector<Action>& actions);
struct Chunk {
    int clickFrom = 0, clickTo = 0;   // click ids [from, to)
    std::vector<int> ticks;           // the states to keep: edge - cap (and edge - reach) of every edge of these clicks, sorted, unique
    int lastTick = 0;                 // the replay can stop once it has saved the state at this tick
    int fullScanTop = 0;              // share of the full scans (pass 2) for this chunk
};
// A chunk is closed at the first click boundary after it holds `maxStates` states; two clicks of one cluster (actions <= cap apart
// on one player) are never separated, so a chunk can exceed the limit by the states of one cluster.
std::vector<Chunk> planChunks(const std::vector<Action>& actions, int cap, int reach, size_t maxStates, int fullScanTop = 20);
void computeSummary(AnalysisData& d);   // tightest windows, averages (Analysis 9)
// Copies the results of clicks [from, to) (and the clusters that start in them) of `part` into `total`, then recounts everything.
void mergeAnalysis(AnalysisData& total, const AnalysisData& part, int clickFrom, int clickTo);

struct SimInput {
    int tick = 0;
    uint8_t player = 0;
    bool down = false;
    uint32_t order = 0;
    double sub = 0;  // 0 <= sub < 1: the input lands this far (in ticks) after the start of `tick`. Only used when the
                     // simulator reports supportsSubTick(); otherwise it is always 0 (CBF-style step splitting, v2).
};
struct SimRequest {
    int startTick = 0;                      // checkpoint to load (the state at the START of this tick)
    std::array<uint8_t, 2> holdAtStart{0, 0};
    std::vector<SimInput> inputs;           // every input with tick >= startTick, in application order
    int horizonTick = 0;                    // last tick to simulate (inclusive)
    int driftTick = -1;                     // sample y / vy at the START of this tick (>= 0)
    int driftPlayer = 0;
    bool compareBaseline = false;           // offset 0 verification (4.10): state hash must equal the baseline each tick
    int convergeAfter = -1;                 // from this tick on the simulator MAY stop on equal state (4.5); -1 = never
};
struct SimResult {
    bool loaded = true;                     // false: the checkpoint was missing
    bool died = false;
    int deathTick = -1;
    uint8_t diedMask = 0;                   // bit0 = player 1 / icon 1, bit1 = player 2 / icon 2
    bool desync = false;
    int desyncTick = -1;
    bool converged = false;
    int convergedTick = -1;
    bool driftValid = false;
    double y = 0, vy = 0;
};
class ISimulator {
public:
    virtual ~ISimulator() = default;
    virtual SimResult run(const SimRequest& req) = 0;
    // true when run() honours SimInput::sub (an input between two physics steps). The real-game simulator does not yet.
    virtual bool supportsSubTick() const { return false; }
};

struct AnalysisInput {
    std::vector<Action> actions;            // final run, edited ticks, all players, in order
    int levelEndTick = 0;
    bool twoPlayer = false, dual = false;
    std::vector<Sample> baselineSamples;    // y / vy per tick for drift (4.12); may be empty
    int analyseUntilTick = INT_MAX;         // Part 5, step 3 "Continue": horizons at or after this are NOT_ANALYSED
    uint32_t globalFlags = 0;               // e.g. ClickFlag::Edited
    // Extra flags for the region [fromTick, toTick] of a player (UNRELIABLE from the recording, RANDOM ...)
    std::function<uint32_t(int fromTick, int toTick, int player)> regionFlags;
};
struct AnalysisParams {
    int cap = kOffsetCap;                   // offsets 1..cap are tested one by one (and drawn in the pass map); 1..12
    int reach = kOffsetCap;                 // > cap: a side that passes all of 1..cap is searched on up to this many ticks
                                            // with steps of 4, then bisected to one tick (assumes pass/fail is monotone there)
    bool wholeClick = false;                // v1 behaviour: the press window moves press AND release together (fixed Hold).
                                            // Default (v2): the press window moves only the press, the release window only the release.
    bool subTick = false;                   // bisect the first failing tick below one tick (needs ISimulator::supportsSubTick())
    int subTickSteps = 4;                   // 4 halvings = 1/16 tick
    int fullScanTop = 20;
    std::vector<int> selectedClicks;        // result click ids that must be full-scanned too (4.9)
    int clickFrom = 0, clickTo = INT_MAX;   // only the clicks with these ids are measured (streamed analysis); the rest stay untouched
    bool clusters = true;
    bool detectConvergence = false;
    double epsilon = 0;
};
struct AnalysisProgress {
    size_t done = 0, total = 0;
    int stage = 0;  // 1 = pass 1 (fast), 2 = pass 2 (full scan), 3 = clusters
};
struct LiveView {  // the overlay of Part 5, step 5
    int action = 0;  // 2c+1 = input window of click c, 2c+2 = output window (1-based as shown to the user)
    std::string mapEarly = std::string(12, '?'), mapLate = std::string(12, '?');
    Counters counters;
};

class AnalysisEngine {
public:
    AnalysisEngine(AnalysisInput in, AnalysisParams params, ISimulator& sim);
    bool step();                 // one shifted run. Returns false when everything is finished.
    void runAll() { while (step()) {} }
    void cancel();               // keeps partial results, marks them incomplete (Part 5, step 4)
    bool finished() const { return finished_; }
    AnalysisProgress progress() const { return prog_; }
    LiveView live() const;
    const AnalysisData& result() const { return data_; }
    AnalysisData takeResult() { return std::move(data_); }

private:
    enum class Win { Input, Output, Cluster };  // Input = press window (v2) / whole-click window (wholeClick), Output = release window
    enum class TaskKind { Verify, Walk };
    enum class Phase { Scan, Coarse, Bisect, Sub };
    struct Task { TaskKind kind; Win win; int target; int side; bool full; };
    struct Walk {
        Task task{};
        int d = 1;
        Phase phase = Phase::Scan;
        int coarseX = 0;                       // next offset of the coarse search (steps of 4 after the map)
        int lo = 0, hi = 0;                    // integer bisection: lo passed, hi failed (or is the first clamped offset)
        bool hiClamp = false;
        int subIter = 0;
        double slo = 0, shi = 0;               // sub-tick bisection bounds
        bool extended = false, sub = false;
        bool failed = false;
        std::string cells = std::string(12, '?');  // index d-1
        StopReason stop = StopReason::None;
        int passes = 0;
        double maxDy = 0, maxDvy = 0;
        bool drift = false;
        uint8_t limiting = 0;
        int convTests = 0, convHits = 0;
    };
    struct ClickRef { int player; int idx; int press; int release; };  // action indices into actions_
    struct Cluster { int player; int first, last; std::vector<int> acts; };  // acts: action indices

    // setup
    void buildStreams();
    bool nextStage();
    void enqueueClick(int c);
    void enqueueWalks(int c, bool full, bool verify);
    void enqueueClusters();
    bool popTask();
    // work
    bool advance();  // true when a simulation ran
    bool clampAt(const Walk& w, int d) const;
    SimRequest buildRequest(const Walk& w, double off, int* startOut) const;
    bool runOffset(Walk& w, double off, SimResult& r);   // one shifted run; true = pass
    void endScan();                                      // offsets 1..cap are done
    void afterEdge();                                    // the integer edge is known: sub-tick bisection or finish
    SimRequest verifyRequest(int c, Win win, int* startOut) const;
    void finishWalk();
    void finalize();
    void recountCounters();
    int horizonFor(int c) const;
    int prevRelease(int c) const;  // tick or -1
    int nextPress(int c) const;    // tick or -1
    std::array<uint8_t, 2> holdAt(int tick) const;
    std::vector<SimInput> plan(int startTick, const std::vector<int>& remove, const std::vector<SimInput>& add) const;
    WindowResult& win(int c, Win w) { return w == Win::Input ? data_.clicks[size_t(c)].in : data_.clicks[size_t(c)].out; }

    AnalysisInput in_;
    AnalysisParams par_;
    ISimulator& sim_;
    AnalysisData data_;
    std::vector<SimInput> base_;                 // all recorded actions as inputs
    std::vector<ClickRef> refs_;                 // same order as data_.clicks
    std::vector<std::vector<int>> stream_;       // per player: action indices in order
    std::vector<Cluster> clusters_;
    std::deque<Task> queue_;
    Walk cur_;
    bool haveCur_ = false;
    bool finished_ = false, cancelled_ = false;
    int stage_ = 0;
    AnalysisProgress prog_;
    std::vector<bool> unreliable_;               // per click: offset 0 failed
    std::vector<int> convT_, convH_;             // per click: output-walk convergence statistics (RELEASE-INSENSITIVE)
};

// Run-time "window rules" helpers shared with the UI / replay
Counters countWindows(const AnalysisData& a);

}  // namespace gdfwr
