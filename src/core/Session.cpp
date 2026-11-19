#include "Session.hpp"

#include <algorithm>
#include <cmath>
#include <climits>
#include <map>

#include "Util.hpp"

namespace gdfwr {

// ---------------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------------
Event* findEvent(Macro& m, uint32_t seq) {
    if (seq < m.events.size() && m.events[seq].seq == seq) return &m.events[seq];
    for (auto& e : m.events) if (e.seq == seq) return &e;
    return nullptr;
}
const Event* findEvent(const Macro& m, uint32_t seq) { return findEvent(const_cast<Macro&>(m), seq); }

static bool inFinalRun(const Macro& m, const Event& e) {
    if (!m.finalRun.exists) return false;
    if (e.flags & EventFlag::Junction) {
        return std::find(m.finalRun.syntheticSeqs.begin(), m.finalRun.syntheticSeqs.end(), e.seq) !=
               m.finalRun.syntheticSeqs.end();
    }
    const auto& sp = m.finalRun.spans;
    for (size_t i = 0; i < sp.size(); i++) {
        if (sp[i].segment != e.segment) continue;
        bool last = (i + 1 == sp.size());
        if (e.tick >= sp[i].fromTick && (last ? e.tick <= sp[i].toTick : e.tick < sp[i].toTick)) return true;
    }
    return false;
}

std::vector<Event> finalRunEvents(const Macro& m) {
    std::vector<Event> out;
    for (const auto& e : m.events) if (inFinalRun(m, e)) out.push_back(e);
    std::stable_sort(out.begin(), out.end(), [](const Event& a, const Event& b) {
        if (a.editedTick != b.editedTick) return a.editedTick < b.editedTick;
        bool ja = a.flags & EventFlag::Junction, jb = b.flags & EventFlag::Junction;
        if (ja != jb) return ja;  // junction (synthetic) events come first inside a tick
        return a.seq < b.seq;
    });
    return out;
}

bool tickStepLooksNormal(double delta, double timeWarp, double step) {
    // (not called "near": that is a macro in <windows.h>)
    auto closeTo = [&](double want) { return std::fabs(delta - want) <= want * 2e-3 + 1e-9; };
    if (closeTo(step)) return true;
    if (timeWarp > 0.01 && timeWarp < 100.0) return closeTo(step * timeWarp) || closeTo(step / timeWarp);
    return false;
}

uint64_t computeMacroHash(const Macro& m) {
    uint64_t h = 1469598103934665603ull;
    for (const auto& e : finalRunEvents(m)) {
        h = hashPod<int32_t>(h, e.editedTick);
        h = hashPod<uint8_t>(h, e.player);
        h = hashPod<uint8_t>(h, uint8_t(e.type));
        h = hashPod<uint8_t>(h, e.button);
        if (e.sub) h = hashPod<uint16_t>(h, e.sub);   // only when set, so every hash of a whole-tick macro stays as it was
    }
    return h;
}

int countActions(const Macro& m) { return int(finalRunEvents(m).size()); }

Validation validateEvents(const std::vector<Event>& ev) {
    Validation v;
    std::array<int, 2> last{-1, -1};  // 1 = last was press, 0 = release, -1 = none
    for (const auto& e : ev) {
        if (e.player > 1) { v.ok = false; v.message = "unknown player"; v.badTick = e.editedTick; return v; }
        if (e.editedTick < 0) { v.ok = false; v.message = "tick below 0"; v.badTick = e.editedTick; return v; }
        int want = (e.type == EventType::Press) ? 1 : 0;
        int prev = last[e.player];
        bool okSeq = (prev == -1) ? (want == 1) : (prev != want);
        if (!okSeq) {
            v.ok = false;
            v.message = "press/release do not alternate";
            v.badTick = e.editedTick;
            return v;
        }
        last[e.player] = want;
    }
    return v;
}

// ---------------------------------------------------------------------------------
// final run (Part 1, 2.2)
// ---------------------------------------------------------------------------------
bool computeFinalRun(Macro& m, int endSegment, bool partial, std::vector<Warning>& warnings) {
    // clear previous result and previous synthetic junction events
    m.finalRun = FinalRun{};
    m.events.erase(std::remove_if(m.events.begin(), m.events.end(),
                                  [](const Event& e) { return e.flags & EventFlag::Junction; }),
                   m.events.end());
    for (size_t i = 0; i < m.events.size(); i++) m.events[i].seq = uint32_t(i);

    if (endSegment < 0 || size_t(endSegment) >= m.segments.size()) { m.macroHash = computeMacroHash(m); return false; }

    std::vector<int> chain;
    for (int s = endSegment; s >= 0; s = m.segments[size_t(s)].parent) {
        chain.push_back(s);
        if (chain.size() > m.segments.size()) return false;  // cycle guard
    }
    std::reverse(chain.begin(), chain.end());
    const Segment& root = m.segments[size_t(chain.front())];
    m.finalRun.partial = partial;
    if (root.startKind != StartKind::LevelStart) { m.macroHash = computeMacroHash(m); return false; }

    bool synthetic = false;
    auto addSynthetic = [&](const Segment& child, int tick, int player, bool press) {
        Event e;
        e.seq = uint32_t(m.events.size());
        e.segment = child.id;
        e.tick = e.editedTick = tick;
        e.player = uint8_t(player);
        e.type = press ? EventType::Press : EventType::Release;
        e.source = "synthetic";
        e.flags = EventFlag::Synthetic | EventFlag::Junction;
        m.finalRun.syntheticSeqs.push_back(e.seq);
        m.events.push_back(e);
        synthetic = true;
    };

    // a level start that begins with a held button needs a synthetic press at tick 0 [DECISION]
    for (int p = 0; p < 2; p++) if (root.startHold[size_t(p)]) addSynthetic(root, root.startTick, p, true);

    for (size_t i = 0; i < chain.size(); i++) {
        const Segment& s = m.segments[size_t(chain[i])];
        bool last = (i + 1 == chain.size());
        int from = s.startTick;
        int to = s.endTick;
        if (!last) {
            const Segment& child = m.segments[size_t(chain[i + 1])];
            int cut = child.startTick;
            if (child.parentBookmark >= 0 && size_t(child.parentBookmark) < m.bookmarks.size()) {
                const Bookmark& b = m.bookmarks[size_t(child.parentBookmark)];
                cut = b.tick;
                // junction hold rule
                for (int p = 0; p < 2; p++) {
                    bool before = b.hold[size_t(p)], after = child.startHold[size_t(p)];
                    if (before && !after) addSynthetic(child, cut, p, false);
                    if (!before && after) addSynthetic(child, cut, p, true);
                }
            }
            to = cut;
        }
        m.finalRun.spans.push_back({s.id, from, to});
    }
    m.finalRun.exists = true;

    // partners across the whole final run
    auto ev = finalRunEvents(m);
    std::array<int64_t, 2> open{-1, -1};
    for (auto& e : ev) {
        Event* real = findEvent(m, e.seq);
        if (!real) continue;
        if (e.type == EventType::Press) open[e.player] = e.seq;
        else if (open[e.player] >= 0) {
            real->partner = int32_t(open[e.player]);
            if (Event* pr = findEvent(m, uint32_t(open[e.player]))) pr->partner = int32_t(e.seq);
            open[e.player] = -1;
        }
    }
    for (const auto& e : ev) if (e.flags & EventFlag::Synthetic) synthetic = true;
    if (synthetic) {
        bool have = std::any_of(warnings.begin(), warnings.end(), [](const Warning& w) { return w.code == code::SyntheticEvents; });
        if (!have) warnings.push_back({std::string(code::SyntheticEvents), "events were added to keep press/release pairs correct"});
    }
    m.macroHash = computeMacroHash(m);
    return true;
}

// ---------------------------------------------------------------------------------
// click editing (Part 1, 3.5)
// ---------------------------------------------------------------------------------
static EditRecord* findEdit(Macro& m, uint32_t seq) {
    for (auto& r : m.edits) if (r.seq == seq) return &r;
    return nullptr;
}
static void setEdited(Macro& m, Event& e, int newTick) {
    e.editedTick = newTick;
    if (newTick == e.tick) {
        e.flags &= uint8_t(~EventFlag::Edited);
        m.edits.erase(std::remove_if(m.edits.begin(), m.edits.end(), [&](const EditRecord& r) { return r.seq == e.seq; }), m.edits.end());
    } else {
        e.flags |= EventFlag::Edited;
        if (auto* r = findEdit(m, e.seq)) r->newTick = newTick;
        else m.edits.push_back({e.seq, e.tick, newTick});
    }
}

EditOutcome moveEvent(Macro& m, uint32_t seq, int newTick) {
    auto fin = finalRunEvents(m);
    auto it = std::find_if(fin.begin(), fin.end(), [&](const Event& e) { return e.seq == seq; });
    if (it == fin.end()) return {false, "Only events of the final run can be edited (dead branches cannot)."};
    if (newTick < 0) return {false, "A tick cannot be below 0."};
    size_t idx = size_t(it - fin.begin());
    uint8_t pl = it->player;
    // same-tick group (same player, contiguous in that player's stream, same edited tick)
    std::vector<size_t> mine;  // indices of this player's events
    for (size_t i = 0; i < fin.size(); i++) if (fin[i].player == pl) mine.push_back(i);
    size_t pos = size_t(std::find(mine.begin(), mine.end(), idx) - mine.begin());
    size_t g0 = pos, g1 = pos;
    while (g0 > 0 && fin[mine[g0 - 1]].editedTick == fin[idx].editedTick) g0--;
    while (g1 + 1 < mine.size() && fin[mine[g1 + 1]].editedTick == fin[idx].editedTick) g1++;
    int lo = 0, hi = INT_MAX;
    if (g0 > 0) lo = fin[mine[g0 - 1]].editedTick + 1;                  // >= 1 tick from the previous neighbour
    if (g1 + 1 < mine.size()) hi = fin[mine[g1 + 1]].editedTick - 1;    // >= 1 tick from the next neighbour
    if (newTick < lo || newTick > hi) {
        return {false, "Allowed range for this event is " + std::to_string(lo) + " to " +
                           (hi == INT_MAX ? std::string("(no limit)") : std::to_string(hi)) + "."};
    }
    for (size_t k = g0; k <= g1; k++) if (Event* e = findEvent(m, fin[mine[k]].seq)) setEdited(m, *e, newTick);
    m.macroHash = computeMacroHash(m);
    return {true, ""};
}

EditOutcome restoreEvent(Macro& m, uint32_t seq) {
    Event* e = findEvent(m, seq);
    if (!e) return {false, "Unknown event."};
    int prev = e->editedTick;
    setEdited(m, *e, e->tick);
    if (!validateEvents(finalRunEvents(m)).ok) {
        setEdited(m, *e, prev);
        return {false, "Restoring this event first would break the order; restore its neighbour first."};
    }
    m.macroHash = computeMacroHash(m);
    return {true, ""};
}

void restoreAll(Macro& m) {
    for (auto& e : m.events) { e.editedTick = e.tick; e.flags &= uint8_t(~EventFlag::Edited); }
    m.edits.clear();
    m.macroHash = computeMacroHash(m);
}

void applyEditSideEffects(GdfwrFile& f) {
    f.header.edit = f.macro.edits.empty() ? EditStatus::Original : EditStatus::Edited;
    f.header.verify = VerifyStatus::NotVerified;  // 3.5: always, after any edit
    f.header.verifyFailTick = -1;
    f.baseline.reset();                            // baseline is deleted
    if (f.analysis) f.analysis->stale = true;      // saved results become STALE
    f.macro.macroHash = computeMacroHash(f.macro);
}

// ---------------------------------------------------------------------------------
// live session
// ---------------------------------------------------------------------------------
void SessionModel::marker(MarkerKind k, int tick) {
    macro_.markers.push_back({k, uint32_t(std::max(cur_, 0)), tick, uint32_t(macro_.events.size())});
}

void SessionModel::newSegment(int tick, StartKind kind, int parent, int parentBookmark, Hold hold, uint8_t flags) {
    Segment s;
    s.id = uint32_t(macro_.segments.size());
    s.parent = parent;
    s.parentBookmark = parentBookmark;
    s.startTick = s.endTick = tick;
    s.startKind = kind;
    s.startHold = hold;
    s.tps = uint16_t(cfg_.timing.tps);
    s.flags = flags;
    macro_.segments.push_back(s);
    cur_ = int(s.id);
    implied_ = hold;
    openPress_ = {-1, -1};
    pendingDeath_ = false;
    lastTick_ = tick;
}

void SessionModel::endSegment(int tick, EndReason why) {
    if (!hasSeg()) return;
    seg().endTick = std::max(tick, seg().startTick);
    seg().endReason = why;
}

void SessionModel::begin(const SessionConfig& cfg, int tick, Hold holdNow) {
    cfg_ = cfg;
    macro_ = {};
    stack_.clear();
    cur_ = -1;
    state_ = State::Active;
    newSegment(tick, StartKind::LevelStart, -1, -1, holdNow, 0);
}

bool SessionModel::onButton(int player, bool press, int tick, std::string_view source) {
    if (state_ != State::Active || !hasSeg() || player < 0 || player > 1) return false;
    if ((implied_[size_t(player)] != 0) == press) return false;  // not an effective change
    if (tick < lastTick_) seg().flags |= SegFlag::UnreliableTick;  // counter must never go backwards (2.4)
    lastTick_ = std::max(lastTick_, tick);
    Event e;
    e.seq = uint32_t(macro_.events.size());
    e.segment = uint32_t(cur_);
    e.tick = e.editedTick = tick;
    e.player = uint8_t(player);
    e.type = press ? EventType::Press : EventType::Release;
    e.source = std::string(source);
    if (press) openPress_[size_t(player)] = int32_t(e.seq);
    else if (openPress_[size_t(player)] >= 0) {
        e.partner = openPress_[size_t(player)];
        macro_.events[size_t(e.partner)].partner = int32_t(e.seq);
        openPress_[size_t(player)] = -1;
    }
    macro_.events.push_back(e);
    implied_[size_t(player)] = press ? 1 : 0;
    return true;
}

void SessionModel::checkHold(Hold g) {
    if (state_ != State::Active || !hasSeg()) return;
    if (g != implied_) seg().flags |= SegFlag::UnreliableInput;  // a missed event (Part 1, section 8)
}

void SessionModel::onCheckpointCreated(uint64_t gameId, int tick, Hold hold, float x, float y) {
    if (state_ == State::Idle || !hasSeg()) return;
    Bookmark b;
    b.id = uint32_t(macro_.bookmarks.size());
    b.gameId = gameId;
    b.segment = uint32_t(cur_);
    b.tick = tick;
    // a checkpoint made while nothing is captured hides inputs: a segment that loads it cannot be trusted (3.2)
    b.seq = state_ == State::Active ? uint32_t(macro_.events.size()) : UINT32_MAX;
    b.hold = hold;
    b.x = x;
    b.y = y;
    macro_.bookmarks.push_back(b);
    stack_.push_back(b.id);
    marker(MarkerKind::CheckpointCreate, tick);
}

void SessionModel::onCheckpointRemoved(bool first) {
    if (stack_.empty()) return;
    uint32_t id = first ? stack_.front() : stack_.back();
    if (first) stack_.erase(stack_.begin()); else stack_.pop_back();
    macro_.bookmarks[id].active = false;
    marker(MarkerKind::CheckpointDelete, lastTick_);
}

void SessionModel::onAllCheckpointsRemoved() {
    for (auto id : stack_) macro_.bookmarks[id].active = false;
    if (!stack_.empty()) marker(MarkerKind::CheckpointDelete, lastTick_);
    stack_.clear();
}

void SessionModel::onDeath(int tick) {
    if (state_ != State::Active) return;
    marker(MarkerKind::Death, tick);
    pendingDeath_ = true;
}

void SessionModel::onReset(int tickBefore, int tickAfter, bool tickKnown, Hold holdAfter, size_t gameCheckpointCount) {
    if (state_ != State::Active && state_ != State::Armed) return;  // Suspended: nothing is captured (3.2)
    bool wasArmed = (state_ == State::Armed);
    if (!wasArmed) {
        EndReason why = pendingDeath_ ? EndReason::Death : (tickAfter == 0 ? EndReason::Restart : EndReason::CheckpointLoad);
        endSegment(std::max(tickBefore, seg().startTick), why);
    }
    uint8_t flags = tickKnown ? 0 : SegFlag::UnreliableTick;
    // the stack must mirror the game's checkpoint array, otherwise we may not know which checkpoint was loaded (2.2)
    if (gameCheckpointCount != stack_.size()) flags |= SegFlag::UnreliableTick;

    if (tickAfter != 0 && !stack_.empty()) {
        const Bookmark& b = macro_.bookmarks[stack_.back()];
        if (b.tick != tickAfter) flags |= SegFlag::UnreliableTick;
        if (b.seq == UINT32_MAX) flags |= SegFlag::UnreliableTick | SegFlag::UnreliableInput;
        marker(MarkerKind::CheckpointLoad, tickAfter);
        newSegment(tickAfter, StartKind::CheckpointLoad, int(b.segment), int(b.id), holdAfter, flags);
    } else {
        if (tickAfter != 0) flags |= SegFlag::UnreliableTick;  // no checkpoint known but not at the start
        marker(MarkerKind::Restart, tickAfter);
        newSegment(tickAfter, StartKind::LevelStart, -1, -1, holdAfter, flags);
    }
    state_ = State::Active;
}

void SessionModel::onGamePause(int tick) { if (state_ == State::Active) marker(MarkerKind::Pause, tick); }

void SessionModel::onGameResume(int tick, Hold g) {
    if (state_ != State::Active) return;
    marker(MarkerKind::Resume, tick);
    for (int p = 0; p < 2; p++) {
        if (implied_[size_t(p)] && !g[size_t(p)]) {  // the game released the button during the pause (2.5)
            Event e;
            e.seq = uint32_t(macro_.events.size());
            e.segment = uint32_t(cur_);
            e.tick = e.editedTick = tick;
            e.player = uint8_t(p);
            e.type = EventType::Release;
            e.source = "synthetic";
            e.flags = EventFlag::Synthetic;
            if (openPress_[size_t(p)] >= 0) {
                e.partner = openPress_[size_t(p)];
                macro_.events[size_t(e.partner)].partner = int32_t(e.seq);
                openPress_[size_t(p)] = -1;
            }
            macro_.events.push_back(e);
            implied_[size_t(p)] = 0;
        }
    }
}

void SessionModel::onTpsChange(int tick, float from, float to) {
    if (!hasSeg()) return;
    seg().flags |= SegFlag::TpsChanged;
    cfg_.timing.tpsChanges.push_back({uint32_t(cur_), tick, from, to});
    marker(MarkerKind::TpsChange, tick);
}

void SessionModel::onRandomBranch(int tick, int trigger, int branch) {
    if (!hasSeg()) return;
    macro_.randomLog.push_back({tick, trigger, branch, uint32_t(cur_)});
}

void SessionModel::noteFps(float avg, float min) {
    if (!hasSeg()) return;
    seg().fpsAvg = avg;
    seg().fpsMin = min;
}

void SessionModel::suspend(int tick) {
    if (state_ != State::Active) return;
    marker(MarkerKind::Suspend, tick);
    endSegment(tick, EndReason::Suspended);
    state_ = State::Suspended;
}

void SessionModel::arm() {
    if (state_ != State::Suspended) return;
    marker(MarkerKind::Arm, lastTick_);
    state_ = State::Armed;
}

void SessionModel::complete(int tick) {
    if (state_ == State::Active) {
        marker(MarkerKind::Completion, tick);
        endSegment(tick, EndReason::Completion);
        state_ = State::Completed;
    } else if (unfinished()) {
        state_ = State::Partial;  // completed while the recorder was paused: not captured (3.2)
    }
}

void SessionModel::stop(int tick, EndReason why) {
    if (state_ == State::Active) endSegment(tick, why);
    if (unfinished()) state_ = State::Partial;
}

int SessionModel::liveActionCount() const {
    if (!hasSeg()) return 0;
    int n = 0;
    int s = cur_;
    int cutTick = INT_MAX;
    for (int guard = 0; s >= 0 && guard < 100000; guard++) {
        for (const auto& e : macro_.events)
            if (int(e.segment) == s && e.tick < cutTick && !(e.flags & EventFlag::Synthetic)) n++;
        const Segment& sg = macro_.segments[size_t(s)];
        if (sg.parent < 0) break;
        cutTick = sg.parentBookmark >= 0 ? macro_.bookmarks[size_t(sg.parentBookmark)].tick : sg.startTick;
        s = sg.parent;
    }
    return n;
}

GdfwrFile SessionModel::finish(std::string gameVersion) {
    GdfwrFile f;
    f.macro = macro_;
    f.level = cfg_.level;
    f.timing = cfg_.timing;
    f.header.modVersion = cfg_.modVersion;
    f.header.gameVersion = std::move(gameVersion);
    f.header.createdUtc = cfg_.createdUtc;
    f.header.origin = Origin::Recorded;
    bool completed = (state_ == State::Completed);
    f.header.rec = completed ? RecStatus::Completed : RecStatus::Partial;
    int endSeg = -1;
    if (completed) {
        for (const auto& s : f.macro.segments) if (s.endReason == EndReason::Completion) endSeg = int(s.id);
    } else if (!f.macro.segments.empty()) {
        endSeg = int(f.macro.segments.size()) - 1;
        // the last segment that actually captured something
        for (int i = int(f.macro.segments.size()) - 1; i >= 0; i--)
            if (f.macro.segments[size_t(i)].endReason != EndReason::Suspended) { endSeg = i; break; }
    }
    computeFinalRun(f.macro, endSeg, !completed, f.header.warnings);

    if (f.timing.clickResolution > 1)
        f.header.warnings.push_back({std::string(code::CoarseInput), "click resolution is " + std::to_string(f.timing.clickResolution) + " ticks"});
    if (cfg_.randomUncaptured)
        f.header.warnings.push_back({std::string(code::RandomUncaptured), "level has random triggers that were not captured"});
    for (const auto& s : f.macro.segments) {
        std::string tag = "segment " + std::to_string(s.id);
        if (s.flags & SegFlag::UnreliableInput) f.header.warnings.push_back({std::string(code::UnreliableInput), tag});
        if (s.flags & SegFlag::UnreliableTick) f.header.warnings.push_back({std::string(code::UnreliableTick), tag});
        if (s.flags & SegFlag::TpsChanged) f.header.warnings.push_back({std::string(code::TpsChanged), tag});
        f.timing.fps.push_back({s.id, s.fpsAvg, s.fpsMin});
    }
    if (f.macro.finalRun.exists) {
        int32_t end = 0;
        for (const auto& e : finalRunEvents(f.macro)) end = std::max(end, e.editedTick);
        f.level.endTick = std::max(f.macro.segments[size_t(endSeg)].endTick, end);
    }
    return f;
}

}  // namespace gdfwr
