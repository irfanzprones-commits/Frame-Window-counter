// Verification pass, analysis driver, real-game simulator and replay controller (Part 5, Part 9).
// Jobs are time-sliced: pump(ms) is called once per frame so the game never freezes (Part 5, step 4).
#pragma once
#include <map>
#include <memory>
#include <set>

#include <filesystem>

#include "Bridge.hpp"
#include "core/Analysis.hpp"
#include "core/Repair.hpp"
#include "core/Trace.hpp"

namespace gdfwr::game {

// Re-simulation on the real game: load a saved checkpoint, feed the inputs, step ticks, report death.
class GameSimulator final : public gdfwr::ISimulator {
public:
    explicit GameSimulator(PlayLayer* pl) : m_pl(pl) {}
    void keep(int tick, CheckpointObject* cp) { m_cps[tick] = cp; }
    void clear() { m_cps.clear(); m_untrusted.clear(); }   // a new verification run starts: the old saved states (and what was learned about them) do not belong to it
    size_t count() const { return m_cps.size(); }
    void setStartX(float x) { m_startX = x; }       // where player 1 starts (for the end-portal rule, see levelEndReason)
    void setBaseline(gdfwr::Baseline const* b) { m_baseline = b; }
    // v0.3.0: every input of the macro, in order. With it a run that cannot start from a saved state (the state is missing, or restoring it
    // does not reproduce the verified run) is replayed from the level start instead: slower, but exact. Without it that run is refused.
    void setBaseInputs(std::vector<gdfwr::SimInput> v) { m_base = std::move(v); m_haveBase = true; }
    // Statistics for the report: saved states that could not be trusted, and runs that had to start from tick 0 because of that.
    int untrustedStates() const { return int(m_untrusted.size()); }
    int untrustedTotal() const { return m_untrustedTotal; }   // over the whole analysis (clear() does not reset it)
    std::set<int> const& untrustedTicks() const { return m_untrusted; }
    int zeroRuns() const { return m_zeroRuns; }
    int convergedRuns() const { return m_converged; }
    gdfwr::SimResult run(gdfwr::SimRequest const& rq) override;
private:
    gdfwr::SimResult runOnce(gdfwr::SimRequest const& rq, bool fromZero);
    PlayLayer* m_pl;
    std::map<int, geode::Ref<CheckpointObject>> m_cps;
    gdfwr::Baseline const* m_baseline = nullptr;
    float m_startX = 0.f;
    std::vector<gdfwr::SimInput> m_base;
    bool m_haveBase = false;
    std::set<int> m_untrusted;     // ticks of saved states whose restore did not reproduce the verified run
    int m_zeroRuns = 0, m_converged = 0, m_untrustedTotal = 0;
};

// Takes the game over for a job (one lock per verify/analyze flow): stops its own update loop, marks our hooks as "simulation", restores everything at the end.
class GameLock {
public:
    explicit GameLock(PlayLayer* pl);
    ~GameLock() { release(false); }
    void release(bool resetLevel);
private:
    PlayLayer* m_pl;
    bool m_wasPaused = false, m_prevPractice = false, m_held = false, m_wasGamePaused = false;
};

// Why a verification failed. Only a Death can be repaired by moving a click.
enum class FailKind { None, Death, NoEnd, Other };

// Part 5, step 1: replay the final run from tick 0 to the end of the level. Saves a checkpoint at (edge - 12) for
// every action edge, the state hash and the position of every tick.
// v0.1.3 auto-repair: when the player DIES, the click that was active at that moment is moved +1 .. +maxShift ticks (later), then
// -1 .. -maxShift (earlier); every try is a full verification run of the changed macro. The first one that reaches the end of
// the level is kept (the file becomes EDITED). If none does, the file is left exactly as it was and the original failure is reported.
class VerifyJob {
public:
    VerifyJob(PlayLayer* pl, gdfwr::GdfwrFile file, GameSimulator& sim, bool autoRepair = true, int maxShift = gdfwr::kRepairDefaultShift);
    bool pump(double budgetMs);  // true when finished
    float progress() const;
    int tick() const { return m_tick; }
    void cancel() { if (!m_done) { m_done = true; m_cancelled = true; m_message = "Cancelled."; } }
    bool done() const { return m_done; }
    bool ok() const { return m_ok; }
    bool cancelled() const { return m_cancelled; }
    // v2: the run needs more saved states than "Max saved states" allows. They were NOT kept; the analysis then runs in chunks
    // (StreamedAnalyzeJob), each with its own replay, instead of failing.
    bool needsStreaming() const { return m_streamed; }
    int failTick() const { return m_failTick; }
    FailKind failKind() const { return m_kind; }
    std::string const& message() const { return m_message; }
    std::string statusText() const;                              // text under the progress bar (includes the repair progress)
    std::vector<std::string> const& repairs() const { return m_repairLog; }   // clicks that were moved to make the run pass
    gdfwr::GdfwrFile& file() { return m_file; }
    gdfwr::GdfwrFile const& fileRef() const { return m_file; }
    gdfwr::Baseline const& baseline() const { return m_base; }  // partial after a failure (for "Continue")
    // v0.3.2 (Phase 2, measurement only): facts of the run that ended, for the comparison with the real playback (makeTraceReport).
    bool hasDeathSample() const { return m_hasDeathAfter; }      // the state right AFTER the step in which the player died
    gdfwr::Sample const& deathSample() const { return m_deathAfter; }
    std::string const& killer() const { return m_killer; }       // "Killed by: ..." or empty
    StepStats const& stepStats() const { return m_stepStats; }   // how the game was stepped during THAT run
    SimCallStats const& simCalls() const { return m_simCalls; }
private:
    void beginRun(gdfwr::GdfwrFile file);                         // (re)start from tick 0 with this file
    void snapshotStats();                                         // v0.3.2: copy State::stepStats / simCalls when the run ends
    void stepOne();
    void fail(FailKind kind, int tick, std::string why);
    void onDeath(int t);
    void nextCandidate();
    void giveUpRepair();
    bool timedOut(int t) const;
    std::string noEndMessage(int t) const;
    PlayLayer* m_pl;
    gdfwr::GdfwrFile m_file;
    GameSimulator& m_sim;
    std::vector<gdfwr::Event> m_events;
    std::set<int> m_cpTicks;
    gdfwr::Baseline m_base;
    size_t m_next = 0;
    int m_tick = 0, m_lastEvent = 0, m_failTick = -1;
    int m_limit = 0;       // last tick the run may last before "did not reach the end" (generous: long levels are fine)
    float m_startX = 0.f;  // x of player 1 at tick 0 (for the end-portal rule and for the failure report)
    float m_lastX = 0.f, m_portalX = 0.f;
    bool m_moving = true;  // x grew during the last second of level time
    bool m_done = false, m_ok = false, m_cancelled = false, m_streamed = false;
    FailKind m_kind = FailKind::None;
    std::string m_message;
    // ---- auto-repair ----
    bool m_auto = true;
    int m_maxShift = gdfwr::kRepairDefaultShift;
    gdfwr::GdfwrFile m_orig;                       // the file as the user gave it (put back when the repair does not work)
    gdfwr::GdfwrFile m_accepted;                   // original + the shifts that were already shown to help
    std::vector<gdfwr::RepairClick> m_culprits;    // the clicks to try for the current trouble spot (as they are in m_accepted)
    size_t m_culpritIdx = 0;
    std::vector<int> m_shifts;                     // shifts to try for each of them, in order
    size_t m_shiftIdx = 0;
    int m_spotTick = -1;                           // tick of the death that started the current trouble spot
    int m_curShift = 0;
    bool m_candidate = false;                      // the run in progress is a repair try (m_file = m_accepted + one shifted click)
    int m_spots = 0, m_tries = 0, m_spotTries = 0;
    std::string m_note;                            // "click 3: trying +2 ticks" / the summary of a failed repair
    std::vector<std::string> m_repairLog;
    // ---- v0.3.2: measurement for the report ----
    gdfwr::Sample m_deathAfter;
    bool m_hasDeathAfter = false;
    std::string m_killer;
    StepStats m_stepStats;
    SimCallStats m_simCalls;
};

// Part 5, steps 4-6: run the analysis engine against the real game.
class AnalyzeJob {
public:
    AnalyzeJob(PlayLayer* pl, gdfwr::GdfwrFile file, GameSimulator& sim, gdfwr::Baseline const& baseline,
               int analyseUntilTick, std::vector<int> selectedClicks);
    bool pump(double budgetMs);  // true when finished
    void cancel();
    gdfwr::AnalysisEngine& engine() { return *m_engine; }
    gdfwr::GdfwrFile& file() { return m_file; }
private:
    void store();
    PlayLayer* m_pl;
    GameSimulator* m_sim = nullptr;
    gdfwr::GdfwrFile m_file;
    std::unique_ptr<gdfwr::AnalysisEngine> m_engine;
    bool m_stored = false;
};

// v2, bounded memory: the same analysis as AnalyzeJob, but only a chunk of the click edges has saved states at any time. For every
// chunk the level is replayed again from tick 0 (uninterrupted, every tick compared with the baseline of the verification), the
// states of that chunk's clicks are kept, its clicks are measured, and the states are freed. The replay is never interrupted to
// restore a state, so an inexact restore cannot damage the baseline. Used automatically when VerifyJob::needsStreaming().
class StreamedAnalyzeJob {
public:
    StreamedAnalyzeJob(PlayLayer* pl, gdfwr::GdfwrFile file, GameSimulator& sim, gdfwr::Baseline const& baseline, int analyseUntilTick);
    bool pump(double budgetMs);  // true when finished
    void cancel();
    gdfwr::GdfwrFile& file() { return m_file; }
    float progress() const;
    std::string statusText() const;
    bool failed() const { return m_failed; }
    std::string const& message() const { return m_message; }
private:
    enum class Phase { Start, Replay, Measure, Done };
    void beginChunk();
    bool replayStep();          // one tick of the replay; true when all states of the chunk are saved (or the replay failed)
    void endChunk();
    void store();
    PlayLayer* m_pl;
    gdfwr::GdfwrFile m_file;
    GameSimulator& m_sim;
    gdfwr::Baseline const* m_baseline;
    gdfwr::AnalysisInput m_in;
    gdfwr::AnalysisParams m_par;
    int m_until = INT_MAX;
    std::vector<gdfwr::Chunk> m_chunks;
    size_t m_ci = 0;
    Phase m_phase = Phase::Start;
    std::vector<gdfwr::Event> m_events;
    size_t m_next = 0;
    int m_tick = 0, m_stopTick = 0;
    std::set<int> m_ticks;
    std::unique_ptr<gdfwr::AnalysisEngine> m_engine;
    gdfwr::AnalysisData m_total;
    bool m_stored = false, m_cancelled = false, m_failed = false;
    std::string m_message;
};

// ---- Replay (Part 9) ----
bool startReplay(PlayLayer* pl, std::string& err);
void feedReplay(PlayLayer* pl, int tick);       // called from the processCommands hook, before the original
// why = nullptr: silent. `end` is only for the trace of the playback (v0.3.2): how the playback ended.
void stopReplay(char const* why, gdfwr::TraceEnd end = gdfwr::TraceEnd::Died);

// ---- v0.3.2 (Phase 2): WHERE does the silent replay differ from the real playback? MEASUREMENT ONLY ----
// While Replay plays the macro in the level, every processCommands call of the real game is noted (State::trace). The hook does:
//     idx = replayPre(...)        first thing in the call: replayBegin (the sample of this tick, taken BEFORE the inputs of the tick are applied),
//                                 then feedReplay (the inputs of this tick, as before)
//     <the game's own processCommands>
//     replayEnd(idx)              did the game's own call move the level clock?
// replayPre / replayBegin return -1 when no trace is being recorded. Nothing here applies an input that was not applied before, moves a click or
// changes how anything is stepped. The fake-game tests call replayPre / replayEnd, so the order "sample, then inputs" that they check is the order of the hook.
int replayPre(PlayLayer* pl, int tick, float dt, bool isHalfTick, bool isLastTick);
int replayBegin(PlayLayer* pl, int tick, float dt, bool isHalfTick, bool isLastTick);
void replayEnd(PlayLayer* pl, int callIdx);
// The comparison of a FINISHED silent replay with the last real playback. The text is in core/Trace.cpp; this only collects the facts.
gdfwr::TraceReport makeTraceReport(VerifyJob const& v);
// Writes the two .csv files of a report that compared something into `dir`. Returns the files written; `error` says what failed.
std::vector<std::filesystem::path> writeTraceFiles(gdfwr::TraceReport const& r, std::string const& levelName, std::filesystem::path const& dir,
                                                   std::string& error);
// One Geode log line per line, each starting with "GDFWR-TRACE ".
void logTraceLines(std::vector<std::string> const& lines);

}  // namespace gdfwr::game
