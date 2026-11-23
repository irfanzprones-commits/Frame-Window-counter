#include "Jobs.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>

#include "core/FileIO.hpp"
#include "ui/Overlays.hpp"

using namespace geode::prelude;

namespace gdfwr::game {
namespace {
double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}
// How long a verification run may last without reaching the end of the level (240 ticks = 1 second of level time).
constexpr int kTailTicks = 240 * 120;          // normal limit: 2 minutes after the last click or after the recorded end
constexpr int kHardCapTicks = 240 * 60 * 30;   // never more than 30 minutes of level time
constexpr int kEarlyDeathTicks = 60;           // a death this early is a start-up problem of the replay, not a bad click: say how the game was stepped
}  // namespace

// ---------------------------------------------------------------------------------------------------
// One run of the game from tick `rq.startTick` (restored from a saved state), or, when `fromZero`, the same run started at the level start with
// every earlier input replayed. Both end the same way: died, reached the end / the horizon alive, or (v0.3.0) converged with the verified run.
SimResult GameSimulator::runOnce(SimRequest const& rq, bool fromZero) {
    SimResult out;
    auto& S = State::get();
    int t0 = rq.startTick;
    std::vector<gdfwr::SimInput> merged;
    std::vector<gdfwr::SimInput> const* inputs = &rq.inputs;
    if (fromZero) {
        m_zeroRuns++;
        t0 = 0;
        merged.reserve(m_base.size() + rq.inputs.size());
        for (auto const& in : m_base) if (in.tick < rq.startTick) merged.push_back(in);   // the inputs of the verified run before the saved state
        merged.insert(merged.end(), rq.inputs.begin(), rq.inputs.end());
        inputs = &merged;
        m_pl->removeAllCheckpoints();
        restartForSim(m_pl);                                       // level from the start; S.sim and the "level ended" flags are cleared
        syncHold(m_pl, Hold{0, 0});
    } else {
        auto it = m_cps.find(rq.startTick);
        if (it == m_cps.end()) { out.loaded = false; return out; }
        S.sim.clear();
        m_pl->loadFromCheckpoint(it->second.data());                       // [BINDING] restore the saved state
        m_pl->m_gameState.m_levelTime = double(rq.startTick) / 240.0;       // [BINDING] make sure the tick is exact
        syncHold(m_pl, rq.holdAtStart);
    }
    size_t next = 0;
    int equalRun = 0;      // consecutive ticks whose state equals the verified run (v0.3.0 convergence)
    constexpr int kConvergeTicks = 4;
    for (int t = t0; t <= rq.horizonTick; t++) {
        bool have = m_baseline && size_t(t) < m_baseline->hashes.size();
        bool conv = rq.convergeAfter >= 0 && t >= rq.convergeAfter && !rq.compareBaseline;
        if (have && (rq.compareBaseline || conv)) {
            uint64_t h = stateHash(m_pl);
            bool equal = h == m_baseline->hashes[size_t(t)];
            if (rq.compareBaseline && !equal) {                           // 4.10: must match the recorded run
                out.desync = true;
                out.desyncTick = t;
                return out;
            }
            // v0.3.0: from `convergeAfter` on (every input that differs from the verified run has been applied by then) a state that equals
            // the verified run for a few ticks in a row IS the verified run from here on: the rest cannot go differently, so the run passes.
            if (conv) {
                equalRun = equal ? equalRun + 1 : 0;
                if (equalRun >= kConvergeTicks) {
                    out.converged = true;
                    out.convergedTick = t;
                    m_converged++;
                    if (rq.driftTick > t && size_t(rq.driftTick) < m_baseline->samples.size()) {   // the verified run's state at the next click, drift 0
                        auto const& s = m_baseline->samples[size_t(rq.driftTick)];
                        out.driftValid = true;
                        out.y = rq.driftPlayer == 0 ? s.y1 : s.y2;
                        out.vy = rq.driftPlayer == 0 ? s.vy1 : s.vy2;
                    }
                    return out;
                }
            }
        }
        if (t == rq.driftTick) {                                          // 4.12: state at the start of the next real click
            auto s = sampleOf(m_pl);
            out.driftValid = true;
            out.y = rq.driftPlayer == 0 ? s.y1 : s.y2;
            out.vy = rq.driftPlayer == 0 ? s.vy1 : s.vy2;
        }
        while (next < inputs->size() && (*inputs)[next].tick <= t) {
            auto const& in = (*inputs)[next++];
            m_pl->handleButton(in.down, gdfwr::kJump, in.player == 0);
        }
        stepTick(m_pl);
        if (S.sim.dead) { out.died = true; out.deathTick = t; out.diedMask = S.sim.diedMask; return out; }
        if (S.sim.done || levelEndReason(m_pl, m_startX)) break;          // reached the end of the level alive
    }
    return out;
}

// v0.3.0: a saved state is TRUSTED once the offset-0 run from it (the one that must equal the verified run, tick for tick) has proved it.
// When that run does not match, the state is not trusted any more and everything that would start from it starts at the level start
// instead (what the verified run did, so it is exact; only slower). The old rule marked the click UNRELIABLE and measured nothing.
SimResult GameSimulator::run(SimRequest const& rq) {
    bool have = m_cps.count(rq.startTick) != 0;
    if (have && !m_untrusted.count(rq.startTick)) {
        SimResult r = runOnce(rq, false);
        if (!rq.compareBaseline) return r;
        if (r.loaded && !r.died && !r.desync) return r;                    // reproduces the verified run: trusted
        if (m_untrusted.insert(rq.startTick).second) m_untrustedTotal++;
        log::info("GDFWR: the saved state at tick {} does not reproduce the verified run ({}); clicks that start there are replayed from the level start",
                  rq.startTick, r.desync ? "state differs at tick " + std::to_string(r.desyncTick) : (r.died ? "died at tick " + std::to_string(r.deathTick) : "not loaded"));
    }
    if (!m_haveBase) { SimResult r; r.loaded = false; return r; }
    return runOnce(rq, true);
}

// ---------------------------------------------------------------------------------------------------
GameLock::GameLock(PlayLayer* pl) : m_pl(pl) {
    auto* sch = CCDirector::get()->getScheduler();
    // The pause menu (the panel is opened from it) must be closed: a game that is paused is not a game that runs. It comes back
    // when the job is over (release).
    m_wasGamePaused = pl->m_isPaused;
    if (m_wasGamePaused) closePauseMenu();
    m_wasPaused = sch->isTargetPaused(pl);
    if (!m_wasPaused) sch->pauseTarget(pl);     // the game's own update loop must not run while we step
    m_prevPractice = pl->m_isPracticeMode;
    State::get().sim.begin();
    m_held = true;
}
void GameLock::release(bool resetLevel) {
    if (!m_held) return;
    m_held = false;
    auto& S = State::get();
    if (resetLevel && PlayLayer::get() == m_pl) {
        m_pl->removeAllCheckpoints();
        m_pl->resetLevelFromStart();
        if (m_pl->m_isPracticeMode != m_prevPractice) m_pl->togglePracticeMode(m_prevPractice);
    }
    S.sim.end();
    if (!m_wasPaused && PlayLayer::get() == m_pl) CCDirector::get()->getScheduler()->resumeTarget(m_pl);
    if (resetLevel && m_wasGamePaused && PlayLayer::get() == m_pl) m_pl->pauseGame(false);   // the pause menu the user came from
}

// ---------------------------------------------------------------------------------------------------
VerifyJob::VerifyJob(PlayLayer* pl, gdfwr::GdfwrFile file, GameSimulator& sim, bool autoRepair, int maxShift)
    : m_pl(pl), m_file(file), m_sim(sim), m_auto(autoRepair),
      // v0.3.0: a click of a recording made at 14 FPS (17 ticks per frame) can be off by that much. The range is never smaller than the
      // frame the recording could resolve, plus a little, whatever the setting says.
      m_maxShift(std::clamp(std::max(maxShift, int(file.timing.clickResolution) > 1 ? int(file.timing.clickResolution) + 2 : 1), 1, 60)) {
    m_orig = file;
    m_accepted = file;
    // Is the silent simulation running at all? If the player does not move in any way of stepping, nothing below can work and
    // the honest answer is to say so at once (instead of two minutes of nothing, ending in "did not reach the end").
    auto probe = calibrateStepper(pl);
    beginRun(std::move(file));
    if (!probe.moved) {
        fail(FailKind::Other, 0,
             "The silent replay does not run: the player did not move in either way of stepping the game.\n" + simDiagnostics(pl, m_startX));
    }
}

// (Re)starts the verification from tick 0 with `file`: fresh saved states, fresh baseline, level restarted.
void VerifyJob::beginRun(gdfwr::GdfwrFile file) {
    m_file = std::move(file);
    m_events = gdfwr::finalRunEvents(m_file.macro);
    auto actions = gdfwr::makeActions(m_events);
    m_cpTicks.clear();
    // v2: with the long search (setting "Search beyond 12 ticks") a second, farther state is saved for every edge
    int reach = int(Mod::get()->getSettingValue<int64_t>("search-reach"));
    for (int t : gdfwr::requiredCheckpointTicks(actions, gdfwr::kOffsetCap, reach)) m_cpTicks.insert(t);
    m_lastEvent = m_events.empty() ? 0 : m_events.back().editedTick;
    m_base = gdfwr::Baseline{};
    m_base.macroHash = m_file.macro.macroHash;
    m_next = 0;
    m_tick = 0;
    m_moving = true;
    m_streamed = false;
    m_hasDeathAfter = false;                         // v0.3.2: the facts of the report belong to THIS run only
    m_killer.clear();
    State::get().stepStats = StepStats{};
    State::get().simCalls = SimCallStats{};
    m_sim.clear();
    m_pl->removeAllCheckpoints();
    restartForSim(m_pl);   // level from the start, a stale "level ended" flag cleared (it would end the run at tick 0)
    m_startX = playerX(m_pl);
    m_lastX = m_startX;
    m_portalX = endPortalX(m_pl);
    m_sim.setStartX(m_startX);
    m_limit = std::max(m_lastEvent, m_file.level.endTick) + kTailTicks;
}

float VerifyJob::progress() const {
    if (m_done) return 1.f;
    float p;
    if (m_portalX > m_startX + 120.f) p = (playerX(m_pl) - m_startX) / (m_portalX - m_startX);  // how far the player is on the way
    else p = float(m_tick) / float(std::max(1, std::max(m_lastEvent, m_file.level.endTick) + 240));
    return std::clamp(p, 0.f, 0.99f);
}

std::string VerifyJob::statusText() const {
    std::string t = "Checking that the macro survives from start to end.\nTick " + std::to_string(m_tick);
    if (!m_note.empty()) t += "\n" + m_note;
    if (!m_repairLog.empty()) t += "\nFixed so far: " + std::to_string(m_repairLog.size()) + " click(s)";
    return t;
}

void VerifyJob::snapshotStats() {
    m_stepStats = State::get().stepStats;
    m_simCalls = State::get().simCalls;
}

void VerifyJob::fail(FailKind kind, int tick, std::string why) {
    log::warn("GDFWR: verification failed at tick {}: {}", tick, why);
    snapshotStats();
    m_done = true;
    m_ok = false;
    m_kind = kind;
    m_failTick = tick;
    m_message = std::move(why);
    m_file.header.verify = gdfwr::VerifyStatus::Failed;
    m_file.header.verifyFailTick = tick;
}

// "click 12 (player 1): press 4318, release 4350" (the ticks the silent replay uses)
static std::string describeClickTicks(gdfwr::RepairClick const& c) {
    std::string s = "click " + std::to_string(c.index + 1) + " (player " + std::to_string(c.player + 1) + "): press " + std::to_string(c.pressTick);
    s += c.hasRelease ? ", release " + std::to_string(c.releaseTick) : ", no release";
    return s;
}

// The player died at tick t. With the Fit on, move the click that is to blame and run again (every try is a whole new run).
// A "trouble spot" is one death and the clicks around it: the click holding at that moment, and the click that comes next (it may have
// been too late). Every shift of every one of them is tried; the spot counts as repaired when the run survives it and dies
// (or ends) somewhere clearly later.
void VerifyJob::onDeath(int t) {
    // v0.2.1: what killed the player, and (for a death in the first second) how the game was stepped. The same lines go to the Geode log.
    std::string facts = killerText();
    if (t <= kEarlyDeathTicks) facts += (facts.empty() ? "" : "\n") + simDiagnostics(m_pl, m_startX);
    if (!m_candidate && !facts.empty()) log::info("GDFWR: death at tick {}: {}", t, facts);
    // v0.3.1: the text of a check that FAILED. It is the SILENT REPLAY that died; the user's macro may well pass in normal playback (that is
    // the case reported on a 120-frame-perfect jump), so the text no longer reads like a verdict on the recording. A failed check never
    // keeps a moved click (giveUpRepair puts the original back), so "not changed" is always true here.
    auto report = [&] {
        std::string s = "The silent replay died at tick " + std::to_string(t) + ". Your macro was NOT changed.\n"
                        "If it passes when you play it back normally, the fault is in the analysis (the silent replay is out of step with the game), not in your macro.";
        for (auto const& c : gdfwr::culpritClicks(m_file.macro, t))
            s += std::string("\n") + (c.pressTick <= t ? "Last click before the death: " : "Next click after the death: ") + describeClickTicks(c);
        if (!facts.empty()) s += "\n" + facts;
        return s;
    };
    if (!m_auto) return fail(FailKind::Death, t, report() + (m_note.empty() ? "" : "\n" + m_note));
    if (m_candidate && t <= m_spotTick + gdfwr::repairSpotMargin(m_maxShift)) return nextCandidate();   // still the same trouble spot
    if (m_candidate) {   // the moved click got the run past the spot: keep the shift and go on with whatever kills the run now
        m_accepted = m_file;
        m_repairLog.push_back(gdfwr::describeShift(m_culprits[m_culpritIdx], m_curShift));
        log::info("GDFWR: repair kept: {}", m_repairLog.back());
    }
    m_culprits = gdfwr::culpritClicks(m_file.macro, t);
    if (m_culprits.empty()) {
        if (m_tries == 0) return fail(FailKind::Death, t, report() + "\nFit: the macro has no click near this tick to move.");
        return giveUpRepair();
    }
    if (m_spots >= gdfwr::kRepairMaxClicks) return giveUpRepair();
    m_spots++;
    m_spotTick = t;
    m_spotTries = 0;
    m_culpritIdx = 0;
    m_shifts = gdfwr::repairShiftOrder(m_maxShift);
    m_shiftIdx = 0;
    m_candidate = false;
    log::info("GDFWR: the run died at tick {}; {} click(s) to try", t, m_culprits.size());
    nextCandidate();
}

void VerifyJob::nextCandidate() {
    while (m_culpritIdx < m_culprits.size()) {
        auto const& click = m_culprits[m_culpritIdx];
        while (m_shiftIdx < m_shifts.size()) {
            int shift = m_shifts[m_shiftIdx++];
            gdfwr::GdfwrFile cand = m_accepted;
            if (!gdfwr::shiftClick(cand.macro, click, shift).ok) continue;   // that tick is taken by a neighbouring click: skip it
            gdfwr::applyEditSideEffects(cand);                               // EDITED, NOT_VERIFIED, analysis STALE (rule 3.5)
            m_curShift = shift;
            m_candidate = true;
            m_tries++;
            m_spotTries++;
            char b[200];
            std::snprintf(b, sizeof b, "The run died at tick %d. Trying click %d of player %d moved %+d tick(s) (try %d)", m_spotTick, click.index + 1,
                          click.player + 1, shift, m_spotTries);
            m_note = b;
            beginRun(std::move(cand));
            return;
        }
        m_culpritIdx++;      // nothing helped with this click: the next click around the spot
        m_shiftIdx = 0;
    }
    giveUpRepair();
}

// Nothing helped: run the ORIGINAL file again up to its death, so that the saved states and the partial baseline are the ones of the
// original macro (the file is left exactly as the user gave it).
void VerifyJob::giveUpRepair() {
    std::string note;
    if (m_tries > 0 && !m_culprits.empty()) {
        std::string which;
        for (size_t i = 0; i < m_culprits.size(); i++) {
            if (i) which += " and ";
            which += "click " + std::to_string(m_culprits[i].index + 1) + " (player " + std::to_string(m_culprits[i].player + 1) + ")";
        }
        char b[80];
        std::snprintf(b, sizeof b, "+1..+%d and -1..-%d ticks (%d tries)", m_maxShift, m_maxShift, m_tries);
        note = "Fit moved " + which + " by " + b + " but the run never got past this spot.";
        if (m_spots > 1) note += " (" + std::to_string(m_spots - 1) + " earlier spot(s) were fitted, but those moves were dropped.)";
        note += " The macro was not changed.";
    }
    m_auto = false;
    m_candidate = false;
    m_repairLog.clear();
    m_note = note;
    beginRun(m_orig);   // dies at the same tick again; onDeath then reports it with the note
}

void VerifyJob::stepOne() {
    auto& S = State::get();
    int t = m_tick;
    if (m_cpTicks.count(t) && !m_streamed) {
        if (int(m_sim.count()) >= Mod::get()->getSettingValue<int64_t>("max-checkpoints")) {
            // v2: no longer a failure. Free what was kept; the analysis will replay the level once per chunk of clicks instead.
            m_streamed = true;
            m_sim.clear();
            log::info("GDFWR: more than {} saved states needed: the analysis will run in chunks", Mod::get()->getSettingValue<int64_t>("max-checkpoints"));
        } else {
            m_sim.keep(t, m_pl->createCheckpoint());   // [BINDING] state at the START of tick t (before this tick's inputs)
        }
    }
    m_base.hashes.push_back(stateHash(m_pl));
    m_base.samples.push_back(sampleOf(m_pl));
    while (m_next < m_events.size() && m_events[m_next].editedTick <= t) {
        auto const& e = m_events[m_next++];
        m_pl->handleButton(e.type == gdfwr::EventType::Press, gdfwr::kJump, e.player == 0);
    }
    stepTick(m_pl);
    if (S.sim.dead) {
        // v0.3.2 (measurement only): the state right after the step in which the player died, and who killed it. The report compares it with
        // the real playback's state at the start of the next tick: equal = the movement of that step is the same and only the death differs.
        m_deathAfter = sampleOf(m_pl);
        m_hasDeathAfter = true;
        m_killer = killerText();
        return onDeath(t);
    }
    // The level ended: this is the only way to become VERIFIED. The hooks of playEndAnimationToPos / levelComplete set
    // sim.done; the game's end flag and the end portal position are the safety nets (see Bridge.hpp). Last resort, for a level whose
    // end portal cannot be found at all: a RECORDED run that is still alive at the tick where the live run ended.
    char const* how = S.sim.done ? "level end" : levelEndReason(m_pl, m_startX);
    if (!how && !(m_portalX > m_startX + 120.f) && m_file.header.origin == gdfwr::Origin::Recorded && m_file.level.endTick > 0 &&
        t >= m_file.level.endTick && playerX(m_pl) > m_startX + 120.f) {
        how = "recorded end tick";
    }
    if (how) {
        log::info("GDFWR: verification reached the end of the level at tick {} ({}; player x {:.0f}, end portal x {:.0f}, start x {:.0f})", t,
                  how, playerX(m_pl), m_portalX, m_startX);
        if (m_candidate) {
            m_repairLog.push_back(gdfwr::describeShift(m_culprits[m_culpritIdx], m_curShift));
            log::info("GDFWR: repair kept: {}", m_repairLog.back());
        }
        snapshotStats();
        m_done = true;
        m_ok = true;
        m_kind = FailKind::None;
        m_file.baseline = m_base;
        m_file.header.verify = gdfwr::VerifyStatus::Verified;
        m_file.header.verifyFailTick = -1;
        m_file.header.rec = gdfwr::RecStatus::Completed;  // an import becomes COMPLETED only here (D2)
        m_file.level.endTick = t;
        m_file.macro.macroHash = gdfwr::computeMacroHash(m_file.macro);
        m_file.baseline->macroHash = m_file.macro.macroHash;
        return;
    }
    if (t % 240 == 0) {  // is the player still travelling? (needed by timedOut)
        float x = playerX(m_pl);
        m_moving = x > m_lastX + 1.f;
        m_lastX = x;
    }
    if (timedOut(t)) return fail(FailKind::NoEnd, t, noEndMessage(t));
    m_tick++;
}

// Past the normal limit a very long level is still fine as long as the player keeps travelling towards a known end portal;
// the hard cap stops everything else.
bool VerifyJob::timedOut(int t) const {
    if (t >= kHardCapTicks) return true;
    if (t <= m_limit) return false;
    bool travelling = m_moving && m_portalX > m_startX + 120.f && playerX(m_pl) < m_portalX;
    return !travelling;
}

// The numbers make a report like this one useful: they show whether the player moved at all and where the portal is.
std::string VerifyJob::noEndMessage(int t) const {
    char b[96];
    std::snprintf(b, sizeof b, "The macro played without dying, but the end of the level was not detected.\n(tick %d)\n", t);
    return std::string(b) + simDiagnostics(m_pl, m_startX);
}

bool VerifyJob::pump(double budgetMs) {
    if (PlayLayer::get() != m_pl) { cancel(); return true; }   // the level was left
    double start = nowMs();
    while (!m_done && nowMs() - start < budgetMs) stepOne();
    return m_done;
}

// ---------------------------------------------------------------------------------------------------
namespace {
// The input of the engine: the final run, the baseline samples (drift) and the UNRELIABLE / RANDOM flags of every click's horizon.
gdfwr::AnalysisInput makeAnalysisInput(PlayLayer* pl, gdfwr::GdfwrFile const& file, gdfwr::Baseline const& baseline, int analyseUntilTick) {
    gdfwr::AnalysisInput in;
    in.actions = gdfwr::makeActions(gdfwr::finalRunEvents(file.macro));
    in.levelEndTick = file.level.endTick;
    in.twoPlayer = file.level.twoPlayer;
    in.baselineSamples = baseline.samples;
    in.analyseUntilTick = analyseUntilTick;
    in.globalFlags = file.header.edit == gdfwr::EditStatus::Edited ? gdfwr::ClickFlag::Edited : 0;
    // v0.3.0: the windows are measured by REPLAYING the level, so a flagged tick counter or input of the RECORDING no longer makes a click
    // UNRELIABLE: the check of the macro (and the fit of its clicks) has already proved that the macro, as it is now, passes. Before, one such
    // flag anywhere near a click made it "not counted", and a recording made at a low frame rate came out with no window at all.
    // The old rule stays available (setting "Count clicks of a recording with a flagged tick counter" off).
    std::vector<std::pair<int, int>> bad;
    if (!Mod::get()->getSettingValue<bool>("count-untrusted-recording")) {
        for (auto const& sp : file.macro.finalRun.spans) {
            auto const& sg = file.macro.segments[sp.segment];
            if (sg.flags & (gdfwr::SegFlag::UnreliableInput | gdfwr::SegFlag::UnreliableTick)) bad.push_back({sp.fromTick, sp.toTick});
        }
    }
    // RANDOM flags for the horizon of every click (Part 3 / 4.13)
    auto xs = randomTriggerXs(pl);
    in.regionFlags = [bad, xs, smp = baseline.samples](int from, int to, int player) -> uint32_t {
        uint32_t f = 0;
        for (auto const& b : bad) if (from <= b.second && to >= b.first) f |= gdfwr::ClickFlag::Unreliable;
        if (!xs.empty() && !smp.empty()) {
            auto at = [&](int t) { return smp[size_t(std::clamp(t, 0, int(smp.size()) - 1))]; };
            float x0 = player == 0 ? at(from).x1 : at(from).x2, x1 = player == 0 ? at(to).x1 : at(to).x2;
            for (float x : xs) if (x >= x0 && x <= x1) { f |= gdfwr::ClickFlag::Random; break; }
        }
        return f;
    };
    return in;
}
// v0.3.0: tell the user when saved states could not be trusted (the clicks that start there were measured by replaying from the level start).
void addSimWarnings(gdfwr::AnalysisData& d, GameSimulator const& sim) {
    if (sim.untrustedTotal() <= 0) return;
    d.warnings.push_back({"RESTORE_REPLAYED", std::to_string(sim.untrustedTotal()) + " saved state(s) did not restore exactly; " +
                                                std::to_string(sim.zeroRuns()) + " run(s) started from tick 0"});
}
// Every input of the macro as the simulator replays it (same order and tie-break as the engine's own list).
std::vector<gdfwr::SimInput> baseInputsOf(gdfwr::AnalysisInput const& in) {
    std::vector<gdfwr::SimInput> v;
    v.reserve(in.actions.size());
    for (auto const& a : in.actions) v.push_back({a.tick, a.player, a.press, a.seq});
    return v;
}
// The settings that decide how the clicks are measured. `reach` must equal the one the saved states were planned for.
gdfwr::AnalysisParams makeAnalysisParams(std::vector<int> selectedClicks) {
    gdfwr::AnalysisParams p;
    p.fullScanTop = int(Mod::get()->getSettingValue<int64_t>("full-scan-top"));   // v0.3.0: default 0 (was 20): the second, tick-by-tick pass is optional
    p.selectedClicks = std::move(selectedClicks);
    p.reach = std::max(gdfwr::kOffsetCap, int(Mod::get()->getSettingValue<int64_t>("search-reach")));
    p.wholeClick = Mod::get()->getSettingValue<bool>("whole-click-window");
    p.subTick = false;  // sub-tick shifts need a simulator that can start an input between two steps (GameSimulator cannot yet)
    p.detectConvergence = Mod::get()->getSettingValue<bool>("fast-converge");   // v0.3.0: a run that is back on the verified run stops there
    return p;
}
}  // namespace

AnalyzeJob::AnalyzeJob(PlayLayer* pl, gdfwr::GdfwrFile file, GameSimulator& sim, gdfwr::Baseline const& baseline,
                       int analyseUntilTick, std::vector<int> selectedClicks)
    : m_pl(pl), m_sim(&sim), m_file(std::move(file)) {
    sim.setBaseline(&baseline);
    auto input = makeAnalysisInput(pl, m_file, baseline, analyseUntilTick);
    sim.setBaseInputs(baseInputsOf(input));
    m_engine = std::make_unique<gdfwr::AnalysisEngine>(std::move(input), makeAnalysisParams(std::move(selectedClicks)), sim);
    State::get().sim.clear();
}

void AnalyzeJob::store() {
    if (m_stored) return;
    m_stored = true;
    gdfwr::AnalysisData d = m_engine->takeResult();
    d.modVersion = GDFWR_VERSION;
    d.macroHash = m_file.macro.macroHash;
    d.stale = false;
    for (auto const& w : m_file.header.warnings) d.warnings.push_back(w);   // COARSE_INPUT, RANDOM_UNCAPTURED ...
    if (m_sim) addSimWarnings(d, *m_sim);
    m_file.analysis = std::move(d);
}

bool AnalyzeJob::pump(double budgetMs) {
    if (PlayLayer::get() != m_pl) { cancel(); return true; }
    double start = nowMs();
    while (nowMs() - start < budgetMs) {
        if (!m_engine->step()) { store(); return true; }
    }
    return false;
}

void AnalyzeJob::cancel() {
    m_engine->cancel();  // partial results are kept and marked incomplete (Part 5, step 4)
    store();
}

// ---------------------------------------------------------------------------------------------------
// Streamed analysis (v2): one chunk of clicks at a time, bounded memory.
StreamedAnalyzeJob::StreamedAnalyzeJob(PlayLayer* pl, gdfwr::GdfwrFile file, GameSimulator& sim, gdfwr::Baseline const& baseline, int analyseUntilTick)
    : m_pl(pl), m_file(std::move(file)), m_sim(sim), m_baseline(&baseline), m_until(analyseUntilTick) {
    sim.setBaseline(&baseline);
    m_in = makeAnalysisInput(pl, m_file, baseline, analyseUntilTick);
    sim.setBaseInputs(baseInputsOf(m_in));
    m_par = makeAnalysisParams({});
    m_events = gdfwr::finalRunEvents(m_file.macro);
    m_chunks = gdfwr::planChunks(m_in.actions, m_par.cap, m_par.reach, size_t(std::max<int64_t>(8, Mod::get()->getSettingValue<int64_t>("max-checkpoints"))),
                                 m_par.fullScanTop);
    log::info("GDFWR: streamed analysis in {} chunk(s)", m_chunks.size());
    State::get().sim.clear();
    if (m_chunks.empty()) m_phase = Phase::Done;
}

float StreamedAnalyzeJob::progress() const {
    if (m_phase == Phase::Done || m_chunks.empty()) return 1.f;
    float inChunk = 0.f;
    if (m_phase == Phase::Replay) inChunk = 0.5f * float(m_tick) / float(std::max(1, m_stopTick));
    else if (m_phase == Phase::Measure && m_engine) {
        auto p = m_engine->progress();
        inChunk = 0.5f + (p.total ? 0.5f * float(p.done) / float(p.total) : 0.5f);
    }
    return std::clamp((float(m_ci) + inChunk) / float(m_chunks.size()), 0.f, 0.99f);
}

std::string StreamedAnalyzeJob::statusText() const {
    char b[256];
    if (m_phase == Phase::Replay) {
        std::snprintf(b, sizeof b, "Part %zu of %zu: replaying to tick %d to save the states of its clicks\n(the macro is long, so the clicks are measured in parts)",
                      m_ci + 1, m_chunks.size(), m_stopTick);
        return b;
    }
    if (m_phase == Phase::Measure && m_engine) {
        auto lv = m_engine->live();
        auto const& k = lv.counters;
        std::snprintf(b, sizeof b, "Part %zu of %zu: measuring\nearly: %s   late: %s\n1:%d 2:%d 3:%d 4:%d 5-6:%d  N+:%d CLAMP:%d UNRELIABLE:%d RANDOM:%d",
                      m_ci + 1, m_chunks.size(), lv.mapEarly.c_str(), lv.mapLate.c_str(), k.b1, k.b2, k.b3, k.b4, k.b5_6, k.nPlus, k.clamp, k.unreliable, k.random);
        return b;
    }
    return "Starting.";
}

// A fresh uninterrupted replay from tick 0 that keeps the states of this chunk's clicks only.
void StreamedAnalyzeJob::beginChunk() {
    auto const& ch = m_chunks[m_ci];
    m_ticks.clear();
    for (int t : ch.ticks) if (t < m_until) m_ticks.insert(t);     // states at or after the failure tick are never needed (those clicks are NOT_ANALYSED)
    m_stopTick = m_ticks.empty() ? 0 : *m_ticks.rbegin();
    m_next = 0;
    m_tick = 0;
    m_sim.clear();
    if (m_ticks.empty()) { m_phase = Phase::Measure; return; }   // nothing before the failure tick: the engine marks these clicks NOT_ANALYSED
    m_pl->removeAllCheckpoints();
    restartForSim(m_pl);                                          // level from the start, stale "level ended" flag cleared
    syncHold(m_pl, Hold{0, 0});                                   // the measuring runs of the previous chunk may have left a button held
    m_sim.setStartX(playerX(m_pl));
    m_phase = Phase::Replay;
}

bool StreamedAnalyzeJob::replayStep() {
    auto& S = State::get();
    int t = m_tick;
    if (m_ticks.count(t)) m_sim.keep(t, m_pl->createCheckpoint());      // [BINDING] state at the START of tick t, as in VerifyJob
    if (t >= m_stopTick) return true;                                    // the last state of this chunk is saved
    if (size_t(t) < m_baseline->hashes.size() && stateHash(m_pl) != m_baseline->hashes[size_t(t)]) {
        m_failed = true;
        m_message = "The second replay of the macro did not match the first one at tick " + std::to_string(t) +
                    ". The game does not reproduce the run exactly, so the rest of the analysis cannot be trusted.";
        return true;
    }
    while (m_next < m_events.size() && m_events[m_next].editedTick <= t) {
        auto const& e = m_events[m_next++];
        m_pl->handleButton(e.type == gdfwr::EventType::Press, gdfwr::kJump, e.player == 0);
    }
    stepTick(m_pl);
    if (S.sim.dead) {
        m_failed = true;
        m_message = "The player died at tick " + std::to_string(t) + " in the second replay of the macro (it survived the first one).";
        return true;
    }
    m_tick++;
    return false;
}

void StreamedAnalyzeJob::endChunk() {
    auto const& ch = m_chunks[m_ci];
    gdfwr::mergeAnalysis(m_total, m_engine->result(), ch.clickFrom, ch.clickTo);
    m_engine.reset();
    m_sim.clear();                 // free the states of this chunk
    m_ci++;
    m_phase = m_ci < m_chunks.size() ? Phase::Start : Phase::Done;
}

bool StreamedAnalyzeJob::pump(double budgetMs) {
    if (PlayLayer::get() != m_pl) { cancel(); return true; }
    double start = nowMs();
    while (nowMs() - start < budgetMs) {
        switch (m_phase) {
        case Phase::Start:
            beginChunk();
            break;
        case Phase::Replay:
            if (replayStep()) {
                if (m_failed) { m_total.incomplete = true; store(); m_phase = Phase::Done; return true; }
                auto const& ch = m_chunks[m_ci];
                gdfwr::AnalysisParams p = m_par;
                p.clickFrom = ch.clickFrom;
                p.clickTo = ch.clickTo;
                p.fullScanTop = ch.fullScanTop;
                m_engine = std::make_unique<gdfwr::AnalysisEngine>(m_in, p, m_sim);
                m_phase = Phase::Measure;
            }
            break;
        case Phase::Measure:
            if (!m_engine) {   // a chunk with no state before the failure tick: nothing can be measured, only recorded as NOT_ANALYSED
                gdfwr::AnalysisParams p = m_par;
                auto const& ch = m_chunks[m_ci];
                p.clickFrom = ch.clickFrom;
                p.clickTo = ch.clickTo;
                p.clusters = false;
                m_engine = std::make_unique<gdfwr::AnalysisEngine>(m_in, p, m_sim);
            }
            if (!m_engine->step()) endChunk();
            break;
        case Phase::Done:
            store();
            return true;
        }
    }
    return false;
}

void StreamedAnalyzeJob::cancel() {
    if (m_cancelled) return;
    m_cancelled = true;
    if (m_engine) {   // the chunk that is being measured keeps its partial results (Part 5, step 4)
        m_engine->cancel();
        auto const& ch = m_chunks[m_ci];
        gdfwr::mergeAnalysis(m_total, m_engine->result(), ch.clickFrom, ch.clickTo);
        m_engine.reset();
    }
    m_total.incomplete = true;
    store();
}

void StreamedAnalyzeJob::store() {
    if (m_stored) return;
    m_stored = true;
    gdfwr::AnalysisData d = std::move(m_total);
    d.modVersion = GDFWR_VERSION;
    d.macroHash = m_file.macro.macroHash;
    d.stale = false;
    d.warnings.clear();
    for (auto const& w : m_file.header.warnings) d.warnings.push_back(w);   // COARSE_INPUT, RANDOM_UNCAPTURED ...
    addSimWarnings(d, m_sim);
    if (d.clicks.empty()) { d.specVersion = std::string(gdfwr::kAnalysisSpec); d.cap = m_par.cap; d.reach = m_par.reach; d.wholeClick = m_par.wholeClick; }
    m_file.analysis = std::move(d);
}

// ---------------------------------------------------------------------------------------------------
// Replay (Part 9)
bool startReplay(PlayLayer* pl, std::string& err) {
    auto& S = State::get();
    if (!S.file) { err = "No recording is open."; return false; }
    auto plan = gdfwr::buildReplayPlan(*S.file);
    if (!plan.ok) { err = plan.reason; return false; }
    S.replay = ReplayState{};
    S.replay.plan = std::move(plan);
    S.replay.events = gdfwr::finalRunEvents(S.file->macro);
    closePauseMenu();
    if (pl->m_isPracticeMode) pl->togglePracticeMode(false);
    pl->removeAllCheckpoints();
    pl->resetLevelFromStart();
    S.replay.active = true;  // set AFTER the reset: the reset hook stops a running replay
    // v0.3.2 (measurement only): from here on every processCommands call of the real game is noted (setting "trace-playback"). The trace of an
    // earlier playback goes away either way, so Analyze can never compare with a playback of another macro by accident.
    if (Mod::get()->getSettingValue<bool>("trace-playback"))
        S.trace.begin(gdfwr::computeMacroHash(S.file->macro), S.file->level.levelId, S.file->level.name, S.replay.events.size());
    else
        S.trace = gdfwr::RealTrace{};
    auto* hud = ui::ReplayHud::create(S.file->level.endTick);
    pl->m_uiLayer->addChild(hud, 99);
    S.replay.hud = hud;
    return true;
}

void feedReplay(PlayLayer* pl, int tick) {
    auto& S = State::get();
    auto& R = S.replay;
    size_t firstFed = R.next;
    while (R.next < R.events.size() && R.events[R.next].editedTick <= tick) {
        auto const& e = R.events[R.next];
        pl->handleButton(e.type == gdfwr::EventType::Press, gdfwr::kJump, e.player == 0);
        if (auto* hud = static_cast<ui::ReplayHud*>(R.hud.data())) {
            PlayerObject* p = e.player == 0 ? pl->m_player1 : pl->m_player2;
            if (p && R.next < R.plan.rings.size()) hud->onAction(pl, R.plan.rings[R.next], p->getPosition());
        }
        R.next++;
    }
    if (R.next > firstFed && R.traceIdx >= 0) S.trace.noteFed(R.traceIdx, int(firstFed), int(R.next - firstFed));   // v0.3.2: which call applied which inputs
}

void stopReplay(char const* why, gdfwr::TraceEnd end) {
    auto& S = State::get();
    auto& R = S.replay;
    if (!R.active) return;
    R.active = false;
    R.traceIdx = -1;
    if (S.trace.recording) {   // v0.3.2: the trace of this playback ends here; one summary goes to the Geode log
        S.trace.finish(end);
        logTraceLines(gdfwr::describeRealTrace(S.trace));
    }
    if (R.hud) R.hud->removeFromParent();
    R.hud = nullptr;
    if (why) notify(why);
}

// ---------------------------------------------------------------------------------------------------
// v0.3.2 (Phase 2): the trace of the real playback and its comparison with the silent replay. MEASUREMENT ONLY: nothing below applies an input,
// moves a click, or changes how the silent replay steps the game.
int replayBegin(PlayLayer* pl, int tick, float dt, bool isHalfTick, bool isLastTick) {
    auto& S = State::get();
    S.replay.traceIdx = -1;
    if (!pl || !S.trace.recording) return -1;
    uint8_t flags = uint8_t((isHalfTick ? gdfwr::TraceFlag::HalfTick : 0) | (isLastTick ? gdfwr::TraceFlag::LastTick : 0));
    // The sample is taken HERE, before feedReplay applies the inputs of this tick: the same moment at which VerifyJob::stepOne pushes
    // Baseline::samples (before the inputs of the tick, before the step). Both sides use the same convention, so tick t means the same.
    S.replay.traceIdx = S.trace.beginCall(tick, pl->m_gameState.m_levelTime * double(gdfwr::kTps), dt, pl->m_gameState.m_timeWarp, flags, sampleOf(pl));
    return S.replay.traceIdx;
}

int replayPre(PlayLayer* pl, int tick, float dt, bool isHalfTick, bool isLastTick) {
    int idx = replayBegin(pl, tick, dt, isHalfTick, isLastTick);   // 1. the state at the START of the tick, before anything of this tick is applied
    feedReplay(pl, tick);                                          // 2. the inputs of this tick (unchanged since v0.3.1)
    return idx;
}

void replayEnd(PlayLayer* pl, int callIdx) {
    if (!pl || callIdx < 0) return;
    State::get().trace.endCall(callIdx, pl->m_gameState.m_levelTime * double(gdfwr::kTps));   // did the game's own call move the level clock, and by how much?
}

gdfwr::TraceReport makeTraceReport(VerifyJob const& v) {
    auto& S = State::get();
    auto const& file = v.fileRef();
    gdfwr::TraceInput in;
    in.modVersion = GDFWR_VERSION;
    in.clickBetweenSteps = Mod::get()->getSettingValue<bool>("click-between-steps");
    in.macro = &file.macro;
    // what the file says about itself
    auto& ff = in.file;
    ff.levelName = file.level.name;
    ff.levelId = file.level.levelId;
    ff.twoPlayer = file.level.twoPlayer || file.level.dual;
    ff.clickResolution = int(file.timing.clickResolution);
    ff.cbf = file.timing.cbf;
    ff.tps = int(file.timing.tps);
    ff.inputs = int(gdfwr::finalRunEvents(file.macro).size());
    {
        float sum = 0.f, lowest = 0.f;
        int n = 0;
        for (auto const& f : file.timing.fps) {
            if (!(f.avg > 0.f)) continue;
            sum += f.avg;
            lowest = n == 0 ? f.min : std::min(lowest, f.min);
            n++;
        }
        if (n > 0) { ff.fpsAvg = sum / float(n); ff.fpsMin = lowest; }
    }
    // the silent replay: Baseline::samples[t] is the state at the START of tick t, before its inputs (VerifyJob::stepOne)
    auto& sr = in.sim;
    bool died = !v.ok() && v.failKind() == FailKind::Death;
    sr.samples = v.baseline().samples;
    sr.macroHash = gdfwr::computeMacroHash(file.macro);
    sr.died = died;
    sr.deathTick = died ? v.failTick() : -1;
    sr.hasDeathSample = died && v.hasDeathSample();
    sr.deathSample = v.deathSample();
    sr.killer = v.killer();
    sr.endTick = v.ok() ? file.level.endTick : -1;
    sr.stepMode = S.stepMode == StepMode::Update ? "Update" : "Direct";
    sr.stepKick = S.stepKick;
    sr.stepProbe = S.stepText;
    auto const& st = v.stepStats();
    sr.steps = st.steps;
    sr.clockForced = st.forced;
    sr.clockAdvMin = st.advMin;
    sr.clockAdvMax = st.advMax;
    auto const& sc = v.simCalls();
    sr.calls = sc.calls;
    sr.halfCalls = sc.half;
    sr.lastCalls = sc.last;
    sr.dtMin = sc.dtMin;
    sr.dtMax = sc.dtMax;
    // the real playback: only when it is a playback of THIS macro on THIS level
    if (!Mod::get()->getSettingValue<bool>("trace-playback")) in.unavailable = "The setting \"Record the real playback and compare it\" is off.";
    else if (!S.trace.any()) in.unavailable = "No real playback was recorded since the game was started.";
    else if (S.trace.macroHash != sr.macroHash)
        in.unavailable = "The macro that was played back is not the macro that was checked (it was edited after the playback, or the Fit moved a click).";
    else if (S.trace.levelId != file.level.levelId) in.unavailable = "The last playback was of another level.";
    else in.real = &S.trace;
    return gdfwr::buildTraceReport(in);
}

std::vector<std::filesystem::path> writeTraceFiles(gdfwr::TraceReport const& r, std::string const& levelName, std::filesystem::path const& dir,
                                                   std::string& error) {
    std::vector<std::filesystem::path> written;
    if (!r.compared) return written;
    std::string base = gdfwr::defaultFileName(levelName, localTime(), true, "");   // "Level_2026-11-23_0457"
    struct Item { char const* suffix; std::string const* text; };
    for (Item const& it : {Item{"_trace-real-playback.csv", &r.callsCsv}, Item{"_trace-compare.csv", &r.compareCsv}}) {
        if (it.text->empty()) continue;
        auto path = dir / (base + it.suffix);
        std::vector<uint8_t> bytes(it.text->begin(), it.text->end());
        std::string err;
        if (!gdfwr::writeAtomicBytes(path, bytes, false, &err)) {
            error += (error.empty() ? "" : "; ") + path.filename().string() + ": " + err;
            continue;
        }
        written.push_back(path);
    }
    return written;
}

void logTraceLines(std::vector<std::string> const& lines) {
    for (auto const& l : lines) log::info("GDFWR-TRACE {}", l);
}

}  // namespace gdfwr::game
