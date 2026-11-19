// Runs the REAL VerifyJob / calibrateStepper / repair state machine (src/game/Jobs.cpp, Bridge.cpp) against a tiny fake game.
// It proves our logic (which click is blamed, which shifts are tried and in which order, what is kept, what is put back, how the
// way of stepping is chosen). It does NOT prove anything about the real Geometry Dash: the fake game is ours.
// Build + run: sh tests/verify_sim.sh
#include <Geode/Geode.hpp>

#include <climits>
#include <cstdio>

#include "core/Session.hpp"
#include "game/Jobs.hpp"
#include "ui/Overlays.hpp"

// ---- link stubs for the UI pieces Jobs.cpp refers to (never used here) --------------------------------------------------------
namespace gdfwr::ui {
ReplayHud* ReplayHud::create(int) { return nullptr; }
void ReplayHud::onAction(PlayLayer*, gdfwr::ReplayRing const&, cocos2d::CCPoint) {}
}  // namespace gdfwr::ui

using namespace gdfwr;
namespace G = gdfwr::game;

static int g_fail = 0, g_checks = 0;
#define CHECK_EQ(a, b) do { g_checks++; if (!((a) == (b))) { g_fail++; std::printf("  FAIL line %d: %s == %s\n", __LINE__, #a, #b); } } while (0)
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; std::printf("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)

// ---- the fake game ---------------------------------------------------------------------------------------------------------------
// The player runs right 1 unit per tick. An obstacle kills the player at `tick` unless a press was applied in [lo, hi].
struct Obstacle { int tick, lo, hi; };
struct FakeGame : PlayLayer {
    std::vector<Obstacle> obstacles;
    bool needsKick = false;     // nothing moves until startGame() was called (or m_started set)
    bool updateOnly = false;    // processCommands does not move the player, update() does
    bool broken = false;        // nothing ever moves
    bool updateMoves = false;   // update() moves the player too (like the real game) ...
    int updateKillTick = -1;    // ... but the player dies at this tick in update() (a start-up problem of that way of stepping, not the macro)
    int directKillTick = -1;    // the same for processCommands
    float portalX = 1000.f;
    float x = 0;
    int tick = 0;
    std::vector<int> presses;
    EndPortalObject portal;
    PlayerObject p1;
    FakeGame() {
        m_anticheatSpike = &acSpike; acSpike.m_objectID = 8;   // same id as an ordinary spike: only the pointer identifies it
        m_player1 = &p1;
        m_endPortal = &portal;
        portal.setPositionX(portalX);
        PlayLayer::current() = this;
    }
    ~FakeGame() { PlayLayer::current() = nullptr; }
    void resetLevelFromStart() override {
        x = 0; tick = 0; presses.clear();
        m_gameState.m_levelTime = 0;
        m_started = !needsKick;
        m_levelEndAnimationStarted = false;
        p1.setPositionX(0);
    }
    void startGame() override { m_started = true; }
    void handleButton(bool down, int, bool) override { if (down) presses.push_back(tick); }
    void step() {
        if (broken || !m_started) return;
        for (auto const& o : obstacles) {
            if (o.tick != tick) continue;
            bool ok = false;
            for (int p : presses) if (p >= o.lo && p <= o.hi) ok = true;
            if (!ok) return die();
        }
        x += 1; tick++;
        m_gameState.m_levelTime = tick / 240.0;
        p1.setPositionX(x);
    }
    // a real checkpoint: the whole state of this fake game (position, tick, the presses so far)
    struct Cp : CheckpointObject { float x = 0; int tick = 0; std::vector<int> presses; };
    CheckpointObject* createCheckpoint() override { auto* c = new Cp; c->x = x; c->tick = tick; c->presses = presses; return c; }
    void loadFromCheckpoint(CheckpointObject* o) override {
        auto* c = static_cast<Cp*>(o);
        x = c->x; tick = c->tick; presses = c->presses;
        m_gameState.m_levelTime = tick / 240.0;
        p1.setPositionX(x);
    }
    GameObject killer, acSpike;
    int antiCheatTick = -1;     // update()/processCommands() make the game's own anti-cheat call (destroyPlayer with its spike) at this tick
    void die() { killer.m_objectID = 8; killer.setPosition({x + 5.f, 105.f}); G::simDestroyPlayer(this, &p1, &killer); }
    void antiCheatCall() { if (antiCheatTick >= 0 && tick == antiCheatTick) G::simDestroyPlayer(this, &p1, &acSpike); }
    void processCommands(float, bool, bool) override {
        if (updateOnly) return;
        antiCheatCall();
        if (directKillTick >= 0 && tick == directKillTick) return die();
        step();
    }
    void update(float) override {
        if (updateOnly) return step();
        if (!updateMoves) return;
        antiCheatCall();
        if (updateKillTick >= 0 && tick == updateKillTick) return die();
        step();
    }
};

static GdfwrFile makeFile(std::vector<std::array<int, 3>> acts, int endTick = 1000) {  // {tick, player, press}
    GdfwrFile f;
    f.header.rec = RecStatus::Completed;
    f.header.verify = VerifyStatus::NotVerified;
    f.level.levelId = 1; f.level.name = "Fake"; f.level.endTick = endTick;
    Segment s; s.id = 0; s.endTick = endTick; s.endReason = EndReason::Completion;
    f.macro.segments.push_back(s);
    for (auto a : acts) {
        Event e; e.seq = uint32_t(f.macro.events.size()); e.tick = e.editedTick = a[0]; e.player = uint8_t(a[1]);
        e.type = a[2] ? EventType::Press : EventType::Release; e.source = "keyboard";
        f.macro.events.push_back(e);
    }
    f.macro.finalRun.exists = true;
    f.macro.finalRun.spans.push_back({0, 0, endTick});
    f.macro.macroHash = computeMacroHash(f.macro);
    return f;
}

static void runToEnd(G::VerifyJob& j) { for (int i = 0; i < 100000 && !j.pump(1000.0); i++) {} }

struct Rig {
    FakeGame game;
    G::GameSimulator sim{&game};
    Rig() { G::State::get().sim.begin(); G::State::get().stepMode = G::StepMode::Direct; G::State::get().stepKick = false; }
    ~Rig() { G::State::get().sim.end(); }
};

static std::vector<int> pressTicks(GdfwrFile const& f) {
    std::vector<int> v;
    for (auto const& e : finalRunEvents(f.macro)) if (e.type == EventType::Press) v.push_back(e.editedTick);
    return v;
}

int main() {
    // T1: a good macro passes untouched
    {
        Rig r; r.game.obstacles = {{110, 95, 105}, {310, 295, 305}};
        auto f = makeFile({{100, 0, 1}, {150, 0, 0}, {300, 0, 1}, {340, 0, 0}});
        G::VerifyJob j(&r.game, f, r.sim); runToEnd(j);
        CHECK(j.done() && j.ok()); CHECK(j.repairs().empty());
        CHECK(j.file().header.verify == VerifyStatus::Verified); CHECK(j.file().header.edit == EditStatus::Original);
        CHECK(j.file().baseline.has_value()); CHECK(j.file().level.endTick == 999);   // x reaches the portal (1000) after 1000 steps
        CHECK(pressTicks(j.file()) == (std::vector<int>{100, 300}));
    }
    // T2: the second click is 1 tick too late: the repair moves it EARLIER by 1 (after trying +1..+10 first)
    {
        Rig r; r.game.obstacles = {{110, 95, 105}, {310, 295, 305}};
        auto f = makeFile({{100, 0, 1}, {150, 0, 0}, {306, 0, 1}, {340, 0, 0}});
        G::VerifyJob j(&r.game, f, r.sim); runToEnd(j);
        CHECK(j.ok());
        CHECK(j.repairs().size() == 1);
        CHECK(j.repairs().size() == 1 && j.repairs()[0].find("1 tick earlier") != std::string::npos);
        CHECK(pressTicks(j.file()) == (std::vector<int>{100, 305}));
        CHECK(j.file().header.edit == EditStatus::Edited); CHECK(j.file().header.verify == VerifyStatus::Verified);
        CHECK(j.file().macro.edits.size() == 2);                 // press and release moved together
        CHECK(finalRunEvents(j.file().macro)[3].editedTick == 339);   // hold time kept (34 ticks)
        CHECK(j.file().macro.macroHash != f.macro.macroHash);
        CHECK(j.file().baseline && j.file().baseline->macroHash == j.file().macro.macroHash);
    }
    // T2b: 3 ticks too EARLY: fixed by the very first direction (+3), tried before any "earlier" shift
    {
        Rig r; r.game.obstacles = {{310, 303, 310}};
        auto f = makeFile({{300, 0, 1}, {340, 0, 0}});
        G::VerifyJob j(&r.game, f, r.sim); runToEnd(j);
        CHECK(j.ok() && j.repairs().size() == 1 && j.repairs()[0].find("3 ticks later") != std::string::npos);
        CHECK(pressTicks(j.file()) == (std::vector<int>{303}));
    }
    // T3: unfixable within 10 ticks: reported as the ORIGINAL failure, the file is untouched, the partial baseline is the original's
    {
        Rig r; r.game.obstacles = {{110, 95, 105}, {310, 295, 305}};
        auto f = makeFile({{100, 0, 1}, {150, 0, 0}, {400, 0, 1}, {440, 0, 0}});
        G::VerifyJob j(&r.game, f, r.sim); runToEnd(j);
        CHECK(j.done() && !j.ok() && j.failKind() == G::FailKind::Death); CHECK(j.failTick() == 310);
        CHECK(j.file().macro.macroHash == f.macro.macroHash); CHECK(j.file().macro.edits.empty());
        CHECK(j.file().header.edit == EditStatus::Original); CHECK(j.file().header.verify == VerifyStatus::Failed);
        CHECK(j.baseline().hashes.size() == 311);                // ticks 0..310 of the ORIGINAL run
        CHECK(j.message().find("Auto-repair moved click 2 (player 1) and click 1 (player 1)") != std::string::npos);   // not holding at the death: the next click first
        CHECK(j.message().find("The macro was not changed") != std::string::npos);
        CHECK(j.repairs().empty());
        CHECK(r.sim.count() > 0);                                // saved states belong to the original run again
    }
    // T4: two bad clicks one after the other: both are repaired
    {
        Rig r; r.game.obstacles = {{310, 295, 305}, {510, 495, 505}};
        auto f = makeFile({{306, 0, 1}, {340, 0, 0}, {512, 0, 1}, {540, 0, 0}});
        G::VerifyJob j(&r.game, f, r.sim); runToEnd(j);
        CHECK(j.ok() && j.repairs().size() == 2);
        CHECK(pressTicks(j.file()) == (std::vector<int>{305, 505}));   // click 1 by -1; click 2 (too LATE: dies before it is even pressed) by -7
    }
    // T5: the repair switched off: the failure is reported as it is
    {
        Rig r; r.game.obstacles = {{310, 295, 305}};
        auto f = makeFile({{306, 0, 1}, {340, 0, 0}});
        G::VerifyJob j(&r.game, f, r.sim, false); runToEnd(j);
        CHECK(!j.ok() && j.failKind() == G::FailKind::Death && j.failTick() == 310);
        CHECK(j.message().find("Auto-repair") == std::string::npos); CHECK(j.file().macro.edits.empty());
    }
    // T6: death before the first click: nothing to blame
    {
        Rig r; r.game.obstacles = {{50, 40, 45}};
        auto f = makeFile({{100, 0, 1}, {150, 0, 0}});
        G::VerifyJob j(&r.game, f, r.sim); runToEnd(j);
        CHECK(!j.ok() && j.failTick() == 50); CHECK(j.failKind() == G::FailKind::Death);
        CHECK(j.message().find("Auto-repair moved click 1") != std::string::npos);   // the first click was too late: tried, but 10 ticks are not enough
        CHECK(j.file().macro.edits.empty());
    }
    // T6b: a macro with no click at all: nothing to move
    {
        Rig r; r.game.obstacles = {{50, 40, 45}};
        auto f = makeFile({});
        G::VerifyJob j(&r.game, f, r.sim); runToEnd(j);
        CHECK(!j.ok() && j.failTick() == 50); CHECK(j.message().find("no click near this tick") != std::string::npos);
    }
    // T6c: the first click is too LATE by 4 ticks: found among the "earlier" shifts of the NEXT click
    {
        Rig r; r.game.obstacles = {{50, 40, 45}};
        auto f = makeFile({{49, 0, 1}, {80, 0, 0}});
        G::VerifyJob j(&r.game, f, r.sim); runToEnd(j);
        CHECK(j.ok() && j.repairs().size() == 1); CHECK(pressTicks(j.file()) == (std::vector<int>{45}));
    }
    // T7: the game only moves through update(): the stepper check finds it
    {
        Rig r; r.game.updateOnly = true; r.game.obstacles = {{110, 95, 105}};
        auto f = makeFile({{100, 0, 1}, {150, 0, 0}});
        G::VerifyJob j(&r.game, f, r.sim); runToEnd(j);
        CHECK(j.ok()); CHECK(G::State::get().stepMode == G::StepMode::Update);
    }
    // T8: the game does not start until startGame() (the delayed call of the real game): the kick finds it
    {
        Rig r; r.game.needsKick = true; r.game.obstacles = {{110, 95, 105}};
        auto f = makeFile({{100, 0, 1}, {150, 0, 0}});
        G::VerifyJob j(&r.game, f, r.sim); runToEnd(j);
        CHECK(j.ok()); CHECK(G::State::get().stepKick);
    }
    // T9: nothing moves in any way: an immediate, clear failure (no two minutes of nothing)
    {
        Rig r; r.game.broken = true;
        auto f = makeFile({{100, 0, 1}, {150, 0, 0}});
        G::VerifyJob j(&r.game, f, r.sim); runToEnd(j);
        CHECK(j.done() && !j.ok() && j.failKind() == G::FailKind::Other);
        CHECK(j.message().find("did not move") != std::string::npos); CHECK(j.message().find("stepper:") != std::string::npos);
    }
    // T10: the macro plays fine but the end of the level is never seen (no portal): reported as NoEnd, not as a death
    {
        Rig r; r.game.portal.setPositionX(0); r.game.m_endPortal = nullptr;
        auto f = makeFile({{100, 0, 1}, {150, 0, 0}}, 0);
        f.header.origin = Origin::Imported;       // imports have no recorded end to fall back on
        G::VerifyJob j(&r.game, f, r.sim); runToEnd(j);
        CHECK(j.done() && !j.ok() && j.failKind() == G::FailKind::NoEnd);
        CHECK(j.message().find("end of the level was not detected") != std::string::npos);
    }
    // T11: same level, but a RECORDED file: the recorded end tick is the last resort
    {
        Rig r; r.game.portal.setPositionX(0); r.game.m_endPortal = nullptr;
        auto f = makeFile({{100, 0, 1}, {150, 0, 0}}, 600);
        G::VerifyJob j(&r.game, f, r.sim); runToEnd(j);
        CHECK(j.ok()); CHECK(j.file().level.endTick == 600);
    }
    // T12: more click edges than "Max saved states": the verification no longer fails, it keeps no states and says so
    {
        geode::Mod::stubInts()["max-checkpoints"] = 8;
        Rig r;
        std::vector<std::array<int, 3>> acts;
        for (int i = 0; i < 9; i++) { int P = 100 + 100 * i; acts.push_back({P, 0, 1}); acts.push_back({P + 20, 0, 0}); r.game.obstacles.push_back({P + 10, P - 3, P + 4}); }
        auto f = makeFile(acts, 1000);
        G::VerifyJob j(&r.game, f, r.sim); runToEnd(j);
        CHECK(j.done() && j.ok()); CHECK(j.needsStreaming()); CHECK_EQ(r.sim.count(), size_t(0));
        geode::Mod::stubInts()["max-checkpoints"] = 3000;
        Rig r2; r2.game.obstacles = r.game.obstacles;
        G::VerifyJob j2(&r2.game, f, r2.sim); runToEnd(j2);
        CHECK(j2.ok()); CHECK(!j2.needsStreaming()); CHECK(r2.sim.count() > 0);
    }
    // T13: the analysis in chunks gives the same result as the analysis with every state kept, and never keeps more than the limit
    {
        std::vector<std::array<int, 3>> acts;
        std::vector<Obstacle> obs;
        for (int i = 0; i < 9; i++) { int P = 100 + 100 * i; acts.push_back({P, 0, 1}); acts.push_back({P + 20, 0, 0}); obs.push_back({P + 10, P - 3, P + 4}); }
        auto f = makeFile(acts, 1000);
        GdfwrFile verified;
        AnalysisData whole;
        {   // reference: every state kept
            Rig r; r.game.obstacles = obs;
            G::VerifyJob j(&r.game, f, r.sim); runToEnd(j); CHECK(j.ok() && !j.needsStreaming());
            verified = j.file();
            Baseline base = *verified.baseline;
            G::AnalyzeJob a(&r.game, verified, r.sim, base, INT_MAX, {});
            for (int i = 0; i < 1000000 && !a.pump(1000.0); i++) {}
            CHECK(a.file().analysis.has_value()); if (a.file().analysis) whole = *a.file().analysis;
        }
        CHECK_EQ(whole.clicks.size(), size_t(9));
        for (auto const& c : whole.clicks) { CHECK_EQ(c.in.early, 3); CHECK_EQ(c.in.late, 4); CHECK(c.in.stopEarly == StopReason::Death && c.in.stopLate == StopReason::Death); }
        CHECK(!whole.wholeClick);
        geode::Mod::stubInts()["max-checkpoints"] = 8;     // 18 states needed: 3 chunks of at most 4 clicks
        {
            Rig r; r.game.obstacles = obs;
            G::VerifyJob j(&r.game, f, r.sim); runToEnd(j); CHECK(j.ok() && j.needsStreaming());
            Baseline base = *j.file().baseline;
            G::StreamedAnalyzeJob a(&r.game, j.file(), r.sim, base, INT_MAX);
            size_t most = 0;
            for (int i = 0; i < 1000000 && !a.pump(0.05); i++) most = std::max(most, r.sim.count());
            if (a.failed()) std::printf("  streamed message: %s\n", a.message().c_str());
            CHECK(!a.failed()); CHECK(most > 0 && most <= 8);
            CHECK(a.file().analysis.has_value());
            if (a.file().analysis) {
                auto const& s = *a.file().analysis;
                CHECK_EQ(s.clicks.size(), whole.clicks.size());
                bool same = s.clicks.size() == whole.clicks.size();
                for (size_t i = 0; same && i < s.clicks.size(); i++)
                    same = s.clicks[i].in.early == whole.clicks[i].in.early && s.clicks[i].in.late == whole.clicks[i].in.late &&
                           s.clicks[i].out.early == whole.clicks[i].out.early && s.clicks[i].out.late == whole.clicks[i].out.late &&
                           s.clicks[i].in.mapEarly == whole.clicks[i].in.mapEarly && s.clicks[i].flags == whole.clicks[i].flags;
                CHECK(same);
                CHECK_EQ(s.counters.b7_10, whole.counters.b7_10); CHECK(!s.incomplete);
                CHECK_EQ(s.summary.tightestInput.size(), whole.summary.tightestInput.size());
            }
        }
        geode::Mod::stubInts()["max-checkpoints"] = 3000;
    }
    // T14: the second replay does not match the first (the game is not reproducible): the streamed analysis stops and says so
    {
        geode::Mod::stubInts()["max-checkpoints"] = 8;
        Rig r;
        std::vector<std::array<int, 3>> acts;
        for (int i = 0; i < 9; i++) { int P = 100 + 100 * i; acts.push_back({P, 0, 1}); acts.push_back({P + 20, 0, 0}); r.game.obstacles.push_back({P + 10, P - 3, P + 4}); }
        auto f = makeFile(acts, 1000);
        G::VerifyJob j(&r.game, f, r.sim); runToEnd(j); CHECK(j.ok() && j.needsStreaming());
        Baseline base = *j.file().baseline;
        base.hashes[150] ^= 1;                                         // pretend the first replay saw something else at tick 150
        G::StreamedAnalyzeJob a(&r.game, j.file(), r.sim, base, INT_MAX);
        for (int i = 0; i < 1000000 && !a.pump(1000.0); i++) {}
        CHECK(a.failed()); CHECK(a.message().find("tick 150") != std::string::npos);
        CHECK(a.file().analysis.has_value() && a.file().analysis->incomplete);
        geode::Mod::stubInts()["max-checkpoints"] = 3000;
    }
    // T15 (the report \"The player died at tick 2\" on a level with one spike): update() kills the player at tick 2 for a reason that has
    // nothing to do with the macro, processCommands() runs fine. The stepper check must not pick the way that dies.
    {
        Rig r; r.game.updateMoves = true; r.game.updateKillTick = 2; r.game.obstacles = {{110, 95, 105}};
        auto f = makeFile({{100, 0, 1}, {150, 0, 0}});
        G::VerifyJob j(&r.game, f, r.sim); runToEnd(j);
        CHECK(j.ok());
        CHECK(G::State::get().stepMode == G::StepMode::Direct);
        CHECK(j.repairs().empty());
    }
    // T16: BOTH ways of stepping kill the player at tick 2 (the player has not met anything): it is reported as it is, and the report says what
    // killed it and how the game was stepped, so that a start-up problem can be told from a bad click
    {
        Rig r; r.game.updateMoves = true; r.game.updateKillTick = 2; r.game.directKillTick = 2; r.game.obstacles = {{110, 95, 105}};
        auto f = makeFile({{100, 0, 1}, {150, 0, 0}});
        G::VerifyJob j(&r.game, f, r.sim); runToEnd(j);
        CHECK(j.done() && !j.ok() && j.failKind() == G::FailKind::Death && j.failTick() == 2);
        CHECK(j.message().find("Killed by: object id 8") != std::string::npos);
        CHECK(j.message().find("direct +2.0 (died at tick 2), update +2.0 (died at tick 2)") != std::string::npos);
        CHECK(j.message().find("stepper:") != std::string::npos);
        CHECK(j.message().find("Auto-repair moved click 1") != std::string::npos);   // the old text is still there
    }
    // T17: the other way round (processCommands dies, update runs): update is used, as before
    {
        Rig r; r.game.updateMoves = true; r.game.directKillTick = 2; r.game.obstacles = {{110, 95, 105}};
        auto f = makeFile({{100, 0, 1}, {150, 0, 0}});
        G::VerifyJob j(&r.game, f, r.sim); runToEnd(j);
        CHECK(j.ok()); CHECK(G::State::get().stepMode == G::StepMode::Update);
    }
    // T18: both die, but one of them later: the one that lasts longer is used
    {
        Rig r; r.game.updateMoves = true; r.game.directKillTick = 2; r.game.updateKillTick = 5;
        auto f = makeFile({{100, 0, 1}, {150, 0, 0}});
        G::VerifyJob j(&r.game, f, r.sim); runToEnd(j);
        CHECK(!j.ok() && j.failTick() == 5); CHECK(G::State::get().stepMode == G::StepMode::Update);
    }
    // T19: a REAL early death (spike at tick 40) is no reason to refuse a stepping way: the click at 36 saves the player, the check passes
    {
        Rig r; r.game.obstacles = {{40, 35, 38}};
        auto f = makeFile({{36, 0, 1}, {60, 0, 0}});
        G::VerifyJob j(&r.game, f, r.sim); runToEnd(j);
        CHECK(j.ok()); CHECK(j.repairs().empty()); CHECK(G::State::get().stepMode == G::StepMode::Direct);
    }
    // T20: the game's own anti-cheat call (destroyPlayer with its spike) is not a death of the macro, in either way of stepping
    {
        Rig r; r.game.updateMoves = true; r.game.antiCheatTick = 2; r.game.obstacles = {{110, 95, 105}};
        auto f = makeFile({{100, 0, 1}, {150, 0, 0}});
        G::VerifyJob j(&r.game, f, r.sim); runToEnd(j);
        CHECK(j.ok()); CHECK(j.repairs().empty());
    }
    // T21: simDestroyPlayer itself: the anti-cheat spike is told apart by the OBJECT (not by its id), another object or none is a death
    {
        Rig r; GameObject other; other.m_objectID = 8;
        auto& S = G::State::get();
        S.sim.clear();
        G::simDestroyPlayer(&r.game, &r.game.p1, &r.game.acSpike);
        CHECK(!S.sim.dead); CHECK(G::killerText().empty());
        G::simDestroyPlayer(&r.game, &r.game.p1, &other);
        CHECK(S.sim.dead); CHECK(S.sim.diedMask == 1); CHECK(G::killerText().find("object id 8") != std::string::npos);
        S.sim.clear();
        G::simDestroyPlayer(&r.game, &r.game.p1, nullptr);
        CHECK(S.sim.dead); CHECK(G::killerText().find("no object at all") != std::string::npos);
        S.sim.clear();
        CHECK(G::isAnticheatSpike(&r.game, &r.game.acSpike)); CHECK(!G::isAnticheatSpike(&r.game, &other)); CHECK(!G::isAnticheatSpike(&r.game, nullptr));
    }
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
