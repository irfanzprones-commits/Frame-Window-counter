// Recording logic of Part 1 (segments, bookmarks, final run, validation, click edits).
// Game-independent: the Geode layer only feeds it facts ("button changed at tick t").
#pragma once
#include "Model.hpp"

namespace gdfwr {

// ---- final run (Part 1, 2.2) --------------------------------------------------
// endSegment: segment that ended in COMPLETION, or (partial) the segment active at stop.
// Builds m.finalRun, appends synthetic junction events to m.events and updates m.macroHash.
// Returns false when no chain reaching tick 0 exists (then finalRun.exists = false).
bool computeFinalRun(Macro& m, int endSegment, bool partial, std::vector<Warning>& warnings);

// Events of the final run sorted by (editedTick, junction events first, seq). Uses edited ticks.
std::vector<Event> finalRunEvents(const Macro& m);
// Does one physics step last 1/240 s of level time (the TPS is unchanged)? A time-warp trigger scales the level time per step, so
// the step is also accepted when it equals step * timeWarp or step / timeWarp. The tolerance is relative (0.2 %), not 1e-6 s:
// a recording must not be marked "TPS changed" by float noise, a lag spike or a time warp.
bool tickStepLooksNormal(double delta, double timeWarp, double step = 1.0 / 240.0);
uint64_t computeMacroHash(const Macro& m);
int countActions(const Macro& m);  // press + release events of the final run

struct Validation {
    bool ok = true;
    std::string message;
    int badTick = -1;
};
// Analysis input rule (Part 3): per player strictly alternating press/release starting with press,
// non-negative ticks. A trailing press is allowed ("run ends while holding").
Validation validateEvents(const std::vector<Event>& finalEvents);

// ---- click editing (Part 1, 3.5) ---------------------------------------------
struct EditOutcome {
    bool ok = false;
    std::string error;
};
EditOutcome moveEvent(Macro& m, uint32_t seq, int newTick);  // moves the same-tick group together
EditOutcome restoreEvent(Macro& m, uint32_t seq);
void restoreAll(Macro& m);
// After ANY edit: NOT_VERIFIED, baseline deleted, saved analysis STALE, edit status updated (3.5).
void applyEditSideEffects(GdfwrFile& f);

Event* findEvent(Macro& m, uint32_t seq);
const Event* findEvent(const Macro& m, uint32_t seq);

// ---- live session ---------------------------------------------------------------
struct SessionConfig {
    LevelInfo level;
    TimingInfo timing;
    std::string modVersion;
    int64_t createdUtc = 0;
    bool randomUncaptured = false;  // level has random triggers and nothing could be captured (2.6)
};

class SessionModel {
public:
    enum class State { Idle, Active, Suspended, Armed, Completed, Partial };
    using Hold = std::array<uint8_t, 2>;

    State state() const { return state_; }
    bool unfinished() const { return state_ == State::Active || state_ == State::Suspended || state_ == State::Armed; }
    bool capturing() const { return state_ == State::Active; }

    void begin(const SessionConfig& cfg, int tick, Hold holdNow);

    // Effective game-level button change (Part 1, 2.3). Returns false if it was not captured.
    bool onButton(int player, bool press, int tick, std::string_view source);
    // Compare the game's own hold state with the state implied by the events (Part 1, section 8).
    void checkHold(Hold gameHold);

    void onCheckpointCreated(uint64_t gameId, int tick, Hold hold, float x, float y);
    void onCheckpointRemoved(bool first);
    void onAllCheckpointsRemoved();
    void onDeath(int tick);
    // The game restored its state after a restart / death respawn / checkpoint load (2.2).
    // tickBefore: last tick seen before the reset. tickAfter: tick read after the restore.
    void onReset(int tickBefore, int tickAfter, bool tickKnown, Hold holdAfter, size_t gameCheckpointCount);
    void onGamePause(int tick);
    void onGameResume(int tick, Hold gameHold);
    void onTpsChange(int tick, float from, float to);
    void onRandomBranch(int tick, int trigger, int branch);
    void noteFps(float avg, float min);
    void flagSegment(uint8_t segFlag) { if (hasSeg()) macro_.segments[size_t(cur_)].flags |= segFlag; }

    void suspend(int tick);   // Pause Recording (3.2)
    void arm();               // Resume Recording (3.2)
    void complete(int tick);  // level completed
    void stop(int tick, EndReason why = EndReason::Stopped);
    // Forget everything. Every index must be reset together with the data it points into (crash 2026-11-12).
    void abandon() {
        state_ = State::Idle;
        macro_ = {};
        stack_.clear();
        cur_ = -1;
        implied_ = {0, 0};
        openPress_ = {-1, -1};
        pendingDeath_ = false;
        lastTick_ = 0;
    }

    int liveActionCount() const;  // HUD "Recording: N actions" (Part 1, 6)
    Hold impliedHold() const { return implied_; }
    int currentSegment() const { return hasSeg() ? cur_ : -1; }
    const Macro& macro() const { return macro_; }

    // Final document. Valid after complete()/stop().
    GdfwrFile finish(std::string gameVersion);

private:
    bool hasSeg() const { return cur_ >= 0 && size_t(cur_) < macro_.segments.size(); }  // never index segments without this
    void newSegment(int tick, StartKind kind, int parent, int parentBookmark, Hold hold, uint8_t flags);
    void endSegment(int tick, EndReason why);
    void marker(MarkerKind k, int tick);
    Segment& seg() { return macro_.segments[size_t(cur_)]; }

    SessionConfig cfg_;
    State state_ = State::Idle;
    Macro macro_;
    std::vector<uint32_t> stack_;  // active bookmark ids, oldest first (mirrors the game's checkpoint array)
    int cur_ = -1;
    Hold implied_{0, 0};
    std::array<int32_t, 2> openPress_{-1, -1};
    bool pendingDeath_ = false;
    int lastTick_ = 0;
};

}  // namespace gdfwr
