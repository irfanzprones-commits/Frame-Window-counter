// Runs the REAL VerifyJob / calibrateStepper / repair state machine (src/game/Jobs.cpp, Bridge.cpp) against a tiny fake game.
// It proves our logic (which click is blamed, which shifts are tried and in which order, what is kept, what is put back, how the
// way of stepping is chosen). It does NOT prove anything about the real Geometry Dash: the fake game is ours.
// Build + run: sh tests/verify_sim.sh
#include <Geode/Geode.hpp>

#include <climits>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
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
    int lastPress = -1000;      // the state a real jump shows: y = ticks since the last press, up to 20 (then it is the same whatever the press tick was)
    long stepsRun = 0;          // how many ticks the game really simulated (to see what the early stop on convergence saves)
    bool badRestore = false;    // loadFromCheckpoint puts the player a little off: a saved state that does NOT reproduce the run (like an inexact checkpoint)
    bool clockStuck = false;    // v0.3.2: the game's own call does not move m_levelTime (stepTick then has to do it): for the step statistics
    int inputLag = 0;           // v0.3.2: a press / release reaches the player this many ticks LATE (a game that applies the inputs later than the macro says)
    struct Pending { int due; bool down; };
    std::vector<Pending> pending;
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
        x = 0; tick = 0; presses.clear(); lastPress = -1000;
        m_gameState.m_levelTime = 0;
        m_started = !needsKick;
        m_levelEndAnimationStarted = false;
        p1.setPositionX(0); p1.setPositionY(20); p1.m_holdingButtons.clear(); p1.m_yVelocity = 0; pending.clear();
    }
    void startGame() override { m_started = true; }
    // like the real game: the hold is part of the state (and of its hash); only a press counts for an obstacle
    // v0.3.2: a press also changes the sampled state AT ONCE (the vertical speed, set back by the step): a sample taken AFTER the inputs of a tick
    // therefore differs from one taken before them, so the tests see a mistake in the sampling moment.
    void applyInput(bool down) { p1.m_holdingButtons[1] = down; if (down) { presses.push_back(tick); lastPress = tick; p1.m_yVelocity = 10; } }
    void handleButton(bool down, int, bool) override {
        if (inputLag > 0) pending.push_back({tick + inputLag, down});
        else applyInput(down);
    }
    void step() {
        if (broken || !m_started) return;
        for (size_t i = 0; i < pending.size();) {     // inputs that were held back (inputLag) arrive at the start of their tick
            if (pending[i].due <= tick) { applyInput(pending[i].down); pending.erase(pending.begin() + long(i)); }
            else i++;
        }
        for (auto const& o : obstacles) {
            if (o.tick != tick) continue;
            bool ok = false;
            for (int p : presses) if (p >= o.lo && p <= o.hi) ok = true;
            if (!ok) return die();
        }
        x += 1; tick++; stepsRun++;
        if (!clockStuck) m_gameState.m_levelTime = tick / 240.0;
        p1.setPositionX(x); p1.setPositionY(float(std::min(tick - lastPress, 20))); p1.m_yVelocity = 0;
    }
    // a real checkpoint: the whole state of this fake game (position, tick, the presses so far)
    struct Cp : CheckpointObject { float x = 0; int tick = 0, lastPress = 0; std::vector<int> presses; };
    CheckpointObject* createCheckpoint() override { auto* c = new Cp; c->x = x; c->tick = tick; c->lastPress = lastPress; c->presses = presses; return c; }
    void loadFromCheckpoint(CheckpointObject* o) override {
        auto* c = static_cast<Cp*>(o);
        x = c->x + (badRestore ? 0.5f : 0.f); tick = c->tick; presses = c->presses; lastPress = c->lastPress;
        m_gameState.m_levelTime = tick / 240.0;
        p1.setPositionX(x); p1.setPositionY(float(std::min(tick - lastPress, 20))); p1.m_yVelocity = 0; pending.clear();
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


// ---- analysis helper: verify a macro, then measure it with the REAL AnalyzeJob / GameSimulator against the fake game ----------------
struct AnalysisRun {
    bool ok = false;
    AnalysisData data;
    long steps = 0;
    int untrusted = 0, zeroRuns = 0, converged = 0;
};
static AnalysisRun analyseWith(std::vector<std::array<int, 3>> acts, std::vector<Obstacle> obs, int endTick, bool badRestore,
                               std::function<void(GdfwrFile&)> prep = {}) {
    AnalysisRun out;
    auto f = makeFile(std::move(acts), endTick);
    if (prep) prep(f);
    Rig r; r.game.obstacles = std::move(obs); r.game.badRestore = badRestore;
    G::VerifyJob j(&r.game, f, r.sim); runToEnd(j);
    if (!j.ok()) return out;
    GdfwrFile verified = j.file();
    Baseline base = *verified.baseline;
    r.game.stepsRun = 0;
    G::AnalyzeJob a(&r.game, verified, r.sim, base, INT_MAX, {});
    for (int i = 0; i < 1000000 && !a.pump(1000.0); i++) {}
    out.ok = a.file().analysis.has_value();
    if (out.ok) out.data = *a.file().analysis;
    out.steps = r.game.stepsRun;
    out.untrusted = r.sim.untrustedStates(); out.zeroRuns = r.sim.zeroRuns(); out.converged = r.sim.convergedRuns();
    return out;
}

// v0.3.2: the same sequence that Hooks.cpp runs for every processCommands call of a REAL playback: replayPre (the sample of the tick BEFORE its
// inputs, then the inputs), the game's own processCommands, then replayEnd (the clock check). It calls the same functions as the hook, in its order.
// `lag`: the fake game applies every input that many ticks LATE (a real game that is out of step with the silent replay). Four calls make one display
// frame, like a 60 FPS game at 240 TPS.
// `feedLag` (a test-only path, NOT the order of the hook): the mod feeds the inputs that many ticks late (a call whose tick number is behind the macro's),
// to check what the report says about an input that was fed in a call other than its own.
static bool playReal(FakeGame& g, GdfwrFile const& f, int calls, int lag = 0, gdfwr::TraceEnd end = gdfwr::TraceEnd::Completed, int feedLag = 0) {
    auto& S = G::State::get();
    S.file = f;
    static cocos2d::CCNode uiLayer;
    g.m_uiLayer = &uiLayer;
    std::string err;
    g.inputLag = lag;
    bool started = G::startReplay(&g, err);
    for (int i = 0; started && i < calls; i++) {
        int tick = G::tickOf(&g);
        int idx = -1;
        if (feedLag == 0) {
            idx = S.replay.active ? G::replayPre(&g, tick, 1.f / 240.f, false, true) : -1;
        } else {
            idx = G::replayBegin(&g, tick, 1.f / 240.f, false, true);
            G::feedReplay(&g, tick - feedLag);
        }
        g.processCommands(1.f / 240.f, false, true);
        if (idx >= 0) G::replayEnd(&g, idx);
        if (i % 4 == 3) S.trace.noteFrame(1.f / 60.f);
    }
    if (started) G::stopReplay(nullptr, end);
    g.inputLag = 0;
    return started;
}
static bool lineHas(std::vector<std::string> const& v, char const* needle) {
    for (auto const& l : v) if (l.find(needle) != std::string::npos) return true;
    return false;
}

// the report lines are wrapped at 120 columns, so a phrase can run over two lines: search the text with the line breaks and the continuation indent folded into one space
static bool textHas(std::vector<std::string> const& v, char const* needle) {
    std::string all;
    for (auto const& l : v) {
        size_t a = l.find_first_not_of(' ');
        if (!all.empty()) all += ' ';
        if (a != std::string::npos) all += l.substr(a);
    }
    return all.find(needle) != std::string::npos;
}

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
        CHECK(j.message().find("Fit moved click 2 (player 1) and click 1 (player 1)") != std::string::npos);   // not holding at the death: the next click first
        // v0.3.1: the failure says that the SILENT replay died, that the macro was not changed, and which clicks are around the death
        CHECK(j.message().find("The silent replay died at tick 310. Your macro was NOT changed.") != std::string::npos);
        CHECK(j.message().find("the fault is in the analysis") != std::string::npos);
        CHECK(j.message().find("Last click before the death: click 1 (player 1): press 100, release 150") != std::string::npos);
        CHECK(j.message().find("Next click after the death: click 2 (player 1): press 400, release 440") != std::string::npos);
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
        CHECK(j.message().find("Fit moved") == std::string::npos); CHECK(j.file().macro.edits.empty());
        CHECK(j.message().find("The silent replay died at tick 310. Your macro was NOT changed.") != std::string::npos);   // v0.3.1
        CHECK(j.message().find("Last click before the death: click 1 (player 1): press 306, release 340") != std::string::npos);
        CHECK(j.message().find("Next click after the death") == std::string::npos);                                         // there is none
        CHECK(j.file().macro.macroHash == f.macro.macroHash); CHECK(j.repairs().empty());                                    // nothing was moved
    }
    // T6: death before the first click: nothing to blame
    {
        Rig r; r.game.obstacles = {{50, 40, 45}};
        auto f = makeFile({{100, 0, 1}, {150, 0, 0}});
        G::VerifyJob j(&r.game, f, r.sim); runToEnd(j);
        CHECK(!j.ok() && j.failTick() == 50); CHECK(j.failKind() == G::FailKind::Death);
        CHECK(j.message().find("Fit moved click 1") != std::string::npos);   // the first click was too late: tried, but 10 ticks are not enough
        CHECK(j.message().find("Next click after the death: click 1 (player 1): press 100, release 150") != std::string::npos);   // v0.3.1: nothing before the death
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
        CHECK(j.message().find("Fit moved click 1") != std::string::npos);   // the old text is still there (v0.3.1: "Auto-repair" is now called "Fit")
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
    // T22 (v0.3.0): a recording whose tick counter was flagged is still counted: the replay measures the LEVEL, not the recording
    {
        std::vector<std::array<int, 3>> acts{{100, 0, 1}, {120, 0, 0}, {300, 0, 1}, {320, 0, 0}};
        std::vector<Obstacle> obs{{110, 97, 104}, {310, 297, 304}};
        auto flag = [](GdfwrFile& f) { f.macro.segments[0].flags = SegFlag::UnreliableTick; };
        auto a = analyseWith(acts, obs, 1000, false, flag);
        CHECK(a.ok); CHECK_EQ(a.data.clicks.size(), size_t(2));
        for (auto const& c : a.data.clicks) { CHECK((c.flags & ClickFlag::Unreliable) == 0); CHECK_EQ(c.in.early, 3); CHECK_EQ(c.in.late, 4); }
        geode::Mod::stubBools()["count-untrusted-recording"] = false;                 // the old rule is still there as a setting
        auto b = analyseWith(acts, obs, 1000, false, flag);
        geode::Mod::stubBools()["count-untrusted-recording"] = true;
        CHECK(b.ok); for (auto const& c : b.data.clicks) CHECK((c.flags & ClickFlag::Unreliable) != 0);
    }
    // T23 (v0.3.0): a saved state that does not restore exactly no longer ruins the clicks: what starts there is replayed from the level start
    {
        std::vector<std::array<int, 3>> acts; std::vector<Obstacle> obs;
        for (int i = 0; i < 4; i++) { int P = 100 + 100 * i; acts.push_back({P, 0, 1}); acts.push_back({P + 20, 0, 0}); obs.push_back({P + 10, P - 3, P + 4}); }
        auto good = analyseWith(acts, obs, 1000, false);
        auto bad = analyseWith(acts, obs, 1000, true);
        CHECK(good.ok); CHECK(bad.ok); CHECK_EQ(good.untrusted, 0); CHECK(bad.untrusted > 0); CHECK(bad.zeroRuns > 0);
        CHECK_EQ(bad.data.clicks.size(), good.data.clicks.size());
        bool same = bad.data.clicks.size() == good.data.clicks.size(), reliable = true;
        for (size_t i = 0; same && i < bad.data.clicks.size(); i++) {
            auto const &g = good.data.clicks[i], &b = bad.data.clicks[i];
            same = g.in.early == b.in.early && g.in.late == b.in.late && g.out.early == b.out.early && g.out.late == b.out.late;
            reliable = reliable && (b.flags & ClickFlag::Unreliable) == 0;
        }
        CHECK(same); CHECK(reliable);
        if (!bad.data.clicks.empty()) { CHECK_EQ(bad.data.clicks[0].in.early, 3); CHECK_EQ(bad.data.clicks[0].in.late, 4); }
        auto warns = [](AnalysisRun const& a, char const* code) { for (auto const& w : a.data.warnings) if (w.code == code) return true; return false; };
        CHECK(warns(bad, "RESTORE_REPLAYED")); CHECK(!warns(good, "RESTORE_REPLAYED"));       // the user is told when the slow way was needed
    }
    // T24 (v0.3.0): a run that is back on the verified run stops there: the same windows, far fewer ticks simulated
    {
        std::vector<std::array<int, 3>> acts{{100, 0, 1}, {120, 0, 0}, {400, 0, 1}, {420, 0, 0}};
        std::vector<Obstacle> obs{{110, 97, 104}, {410, 397, 404}};
        geode::Mod::stubBools()["fast-converge"] = false;
        auto slow = analyseWith(acts, obs, 2000, false);
        geode::Mod::stubBools()["fast-converge"] = true;
        auto fast = analyseWith(acts, obs, 2000, false);
        CHECK(slow.ok); CHECK(fast.ok); CHECK_EQ(slow.data.clicks.size(), fast.data.clicks.size());
        bool same = slow.data.clicks.size() == fast.data.clicks.size();
        for (size_t i = 0; same && i < fast.data.clicks.size(); i++) {
            auto const &s = slow.data.clicks[i], &f = fast.data.clicks[i];
            same = s.in.early == f.in.early && s.in.late == f.in.late && s.in.stopEarly == f.in.stopEarly && s.in.stopLate == f.in.stopLate &&
                   s.out.early == f.out.early && s.out.late == f.out.late;
        }
        CHECK(same); CHECK(fast.converged > 0); CHECK_EQ(slow.converged, 0);
        CHECK(fast.steps * 2 < slow.steps);
        std::printf("  (T24) ticks simulated: %ld without the early stop, %ld with it\n", slow.steps, fast.steps);
    }
    // T25 (v0.3.0): a recording that is off by a few ticks at EVERY click is fitted click by click (the limit used to be 5 spots, then
    // everything was dropped), each by the smallest shift that works
    {
        std::vector<std::array<int, 3>> acts; std::vector<Obstacle> obs;
        for (int i = 0; i < 8; i++) { int P = 100 + 100 * i; acts.push_back({P, 0, 1}); acts.push_back({P + 20, 0, 0}); obs.push_back({P + 10, P + 2, P + 8}); }
        auto f = makeFile(acts, 1000);
        Rig r; r.game.obstacles = obs;
        G::VerifyJob j(&r.game, f, r.sim); runToEnd(j);
        CHECK(j.ok()); CHECK_EQ(j.repairs().size(), size_t(8));
        auto p = pressTicks(j.file());
        bool plus2 = p.size() == 8; for (size_t i = 0; plus2 && i < p.size(); i++) plus2 = p[i] == 100 + 100 * int(i) + 2;
        CHECK(plus2);                                                  // +1 and -1 fail, +2 passes: the smallest shift
        bool kept = true; for (auto const& e : j.file().macro.events) kept = kept && e.editedTick >= e.tick - 2 && e.editedTick <= e.tick + 2;
        CHECK(kept);
    }
    // T26 (v0.3.0): how far a click may be moved follows the recording: a file made at 14 FPS (17 ticks per frame) is searched 19 ticks each way
    {
        std::vector<std::array<int, 3>> acts{{100, 0, 1}, {140, 0, 0}};
        std::vector<Obstacle> obs{{125, 115, 120}};                  // the press has to be at 115..120: 15 ticks later than recorded
        {
            auto f = makeFile(acts, 1000); f.timing.clickResolution = 1;
            Rig r; r.game.obstacles = obs;
            G::VerifyJob j(&r.game, f, r.sim); runToEnd(j);
            CHECK(!j.ok());                                          // 10 ticks (the setting) is not enough for an exact recording
        }
        {
            auto f = makeFile(acts, 1000); f.timing.clickResolution = 17;
            Rig r; r.game.obstacles = obs;
            G::VerifyJob j(&r.game, f, r.sim); runToEnd(j);
            CHECK(j.ok()); auto p = pressTicks(j.file()); CHECK(p.size() == 1 && p[0] == 115);
        }
    }
    // ---- v0.3.2 (Phase 2): the real playback is traced and compared with the silent replay. MEASUREMENT ONLY. ----------------------------------------
    // The fake game says nothing about the real Geometry Dash; these tests check OUR logic: that both traces use the same tick convention, what is
    // compared, what is reported, and that the measurement changes nothing about the silent replay.
    // T27: the same macro played "for real" (as the hook plays it) and replayed silently gives the same state at the start of EVERY tick. That is
    // also the proof of the sampling moment: a sample taken after the inputs of the tick would differ at every click.
    {
        auto f = makeFile({{100, 0, 1}, {150, 0, 0}, {300, 0, 1}, {340, 0, 0}});
        Rig r; r.game.obstacles = {{110, 95, 105}, {310, 295, 305}};
        G::VerifyJob j(&r.game, f, r.sim); runToEnd(j);
        CHECK(j.ok()); CHECK_EQ(j.baseline().samples.size(), size_t(1000));
        CHECK(playReal(r.game, f, 1000));
        auto& S = G::State::get();
        CHECK_EQ(S.trace.calls.size(), size_t(1000)); CHECK(S.trace.end == gdfwr::TraceEnd::Completed); CHECK(!S.trace.recording);
        CHECK_EQ(S.trace.macroHash, computeMacroHash(f.macro)); CHECK_EQ(S.trace.levelId, int64_t(1)); CHECK_EQ(S.trace.events, size_t(4));
        // the inputs were fed in the call of their own tick, and the trace says so
        CHECK_EQ(S.trace.calls[100].fedCount, uint16_t(1)); CHECK_EQ(S.trace.calls[100].fedFirst, 0); CHECK_EQ(S.trace.calls[150].fedFirst, 1);
        CHECK_EQ(S.trace.calls[300].fedFirst, 2); CHECK_EQ(S.trace.calls[340].fedFirst, 3); CHECK_EQ(S.trace.calls[101].fedCount, uint16_t(0));
        CHECK_EQ(S.trace.calls[7].tick, 7); CHECK(S.trace.calls[7].clockAfter - S.trace.calls[7].clock > 0.999 && S.trace.calls[7].clockAfter - S.trace.calls[7].clock < 1.001);
        CHECK(S.trace.calls[10].flags == gdfwr::TraceFlag::LastTick);
        auto rep = G::makeTraceReport(j);
        CHECK(rep.compared); CHECK(rep.cmp.ok); CHECK(rep.cmp.verdict == gdfwr::TraceVerdict::Identical);
        CHECK_EQ(rep.cmp.firstOver, -1); CHECK_EQ(rep.cmp.firstAny, -1); CHECK_EQ(rep.cmp.compared, 1000); CHECK_EQ(rep.cmp.overCount, 0);
        CHECK(rep.headline == "identical at every compared tick");
        CHECK(lineHas(rep.popup, "NO DIFFERENCE")); CHECK(lineHas(rep.popup, "Step mode: Direct")); CHECK(lineHas(rep.popup, "click-between-steps: OFF"));
        CHECK(lineHas(rep.popup, "TPS (real playback 240.0)")); CHECK(lineHas(rep.popup, "FPS (real playback avg 60.0"));
        CHECK_EQ(rep.stats.stepsPerFrame.size(), size_t(1)); CHECK_EQ(rep.stats.stepsPerFrame[0].first, 4);
        CHECK(!rep.callsCsv.empty() && !rep.compareCsv.empty());
        S.file.reset();
    }
    // T28: the GAME applies every input ONE tick later than the silent replay does (the fake game's input lag). The inputs are fed in the call of their own
    // tick, so the trace says "applied at the macro tick"; the difference is in the states: the first one is the tick after the press, with both sets of
    // values. x stays in step, so the whole-state test finds no shift, and the vertical-only test finds k=+1.
    {
        auto f = makeFile({{100, 0, 1}, {150, 0, 0}, {300, 0, 1}, {340, 0, 0}});
        Rig r; r.game.obstacles = {{110, 95, 105}, {310, 295, 305}};
        G::VerifyJob j(&r.game, f, r.sim); runToEnd(j);
        CHECK(j.ok());
        CHECK(playReal(r.game, f, 1000, 1));
        auto rep = G::makeTraceReport(j);
        CHECK(rep.compared); CHECK_EQ(rep.cmp.firstOver, 101);
        CHECK(rep.cmp.simAt.y1 == 1.f); CHECK(rep.cmp.realAt.y1 == 20.f); CHECK(rep.cmp.simAt.x1 == 101.f); CHECK(rep.cmp.realAt.x1 == 101.f);
        CHECK(rep.headline == "first difference at tick 101");
        CHECK(lineHas(rep.popup, "FIRST TICK WHERE THEY DIFFER: tick 101")); CHECK(lineHas(rep.popup, "dy -19.0000"));
        CHECK(rep.cmp.verdict == gdfwr::TraceVerdict::NotAShift);                  // x moves with the tick count in step: no k explains the whole state ...
        CHECK_EQ(rep.cmp.xFirstOver, -1);
        CHECK(rep.cmp.vverdict == gdfwr::TraceVerdict::ShiftedWhole); CHECK_EQ(rep.cmp.vverdictK, 1);   // ... but the jump is one tick EARLIER in the silent replay
        CHECK(textHas(rep.popup, "VERTICAL ONLY (y and yVel; x ignored)")); CHECK(textHas(rep.popup, "SHIFTED by k=+1")); CHECK(textHas(rep.popup, "EARLIER in the silent replay"));
        CHECK(lineHas(rep.result, "Shift test")); CHECK(lineHas(rep.result, "Vertical-only shift test"));
        CHECK(lineHas(rep.log, "real playback applied it in the call at tick 100")); CHECK(!lineHas(rep.log, "NOT the macro tick"));   // the mod fed it in its own call
        CHECK(lineHas(rep.log, "vertical only (y, yVel; x ignored): best shift in this window k=+1"));
        G::State::get().file.reset();
    }
    // T28b: the same difference, but this time the MOD feeds the input in a later call (a call whose tick number is behind the macro's): the trace says so
    {
        auto f = makeFile({{100, 0, 1}, {150, 0, 0}, {300, 0, 1}, {340, 0, 0}});
        Rig r; r.game.obstacles = {{110, 95, 105}, {310, 295, 305}};
        G::VerifyJob j(&r.game, f, r.sim); runToEnd(j);
        CHECK(j.ok());
        CHECK(playReal(r.game, f, 1000, 0, gdfwr::TraceEnd::Completed, 1));
        auto rep = G::makeTraceReport(j);
        CHECK(rep.compared); CHECK_EQ(rep.cmp.firstOver, 101);
        CHECK(lineHas(rep.log, "real playback applied it in the call at tick 101")); CHECK(lineHas(rep.log, "NOT the macro tick (+1)"));
        CHECK(rep.cmp.vverdict == gdfwr::TraceVerdict::ShiftedWhole); CHECK_EQ(rep.cmp.vverdictK, 1);
        G::State::get().file.reset();
    }
    // T29: the reported problem in small: the silent replay DIES at a jump that the real playback passes (the real game takes the press 3 ticks later than
    // the macro says, the obstacle needs it there). The report says where the two first differ, what the silent replay did in its dying step, which clicks
    // are nearest to the death, and that the jump is shifted by 3 ticks while x is in step.
    {
        auto f = makeFile({{300, 0, 1}, {340, 0, 0}, {600, 0, 1}, {640, 0, 0}});
        Rig r; r.game.obstacles = {{310, 303, 310}};
        G::VerifyJob j(&r.game, f, r.sim, false); runToEnd(j);
        CHECK(j.done() && !j.ok() && j.failKind() == G::FailKind::Death); CHECK_EQ(j.failTick(), 310);
        CHECK(j.hasDeathSample()); CHECK(j.deathSample().x1 == 310.f); CHECK(j.killer().find("Killed by: object id 8") == 0);
        CHECK(playReal(r.game, f, 1000, 3));                                         // the real playback passes: the press lands in 303..310
        CHECK_EQ(G::State::get().trace.calls.size(), size_t(1000));
        auto rep = G::makeTraceReport(j);
        CHECK(rep.compared); CHECK_EQ(rep.cmp.firstOver, 301);                       // silent: pressed at 300, y = 1 at tick 301; real: not yet pressed, y = 20
        CHECK(rep.cmp.simAt.y1 == 1.f); CHECK(rep.cmp.realAt.y1 == 20.f);
        CHECK(rep.cmp.deathKnown); CHECK_EQ(rep.cmp.deathTick, 310); CHECK(rep.cmp.realContinued); CHECK(rep.cmp.deathRealHave);
        CHECK(rep.cmp.deathSim.x1 == 310.f); CHECK(rep.cmp.deathReal.x1 == 311.f); CHECK(!rep.cmp.deathEqual);
        CHECK_EQ(rep.cmp.simTicks, 311);                                             // ticks 0..310 of the silent replay
        CHECK(rep.cmp.verdict == gdfwr::TraceVerdict::NotAShift); CHECK(rep.cmp.vverdict == gdfwr::TraceVerdict::ShiftedWhole); CHECK_EQ(rep.cmp.vverdictK, 3);
        CHECK(lineHas(rep.popup, "The silent replay DIED during tick 310")); CHECK(textHas(rep.popup, "the real playback went on (it has tick 311)"));
        CHECK(lineHas(rep.popup, "FIRST TICK WHERE THEY DIFFER: tick 301"));
        CHECK(textHas(rep.popup, "SHIFTED by k=+3")); CHECK(textHas(rep.popup, "x is equal at every compared tick"));
        CHECK(lineHas(rep.log, "Silent replay: 311 ticks (0..310), died during tick 310 - Killed by: object id 8"));
        CHECK(lineHas(rep.log, "click #1 (P1): press 300, release 340 - the player was holding it at that tick"));
        CHECK(lineHas(rep.log, "real playback applied it in the call at tick 300"));  // fed in its own call: the delay is inside the game, not in the feeding
        CHECK(lineHas(rep.log, "vertical only (y, yVel; x ignored): best shift in this window k=+3"));
        CHECK(lineHas(rep.log, "click #2 (P1): press 600"));                         // the second click is near too
        CHECK(lineHas(rep.log, "the silent replay ended at tick 310, before this tick: nothing to compare here"));   // the release at 340 is after the death
        CHECK(lineHas(rep.log, "clicks nearest tick 310 (the death of the silent replay)"));
        CHECK(!lineHas(rep.popup, "click #1"));                                      // the click facts are for the log
        G::State::get().file.reset();
    }
    // T29b: the same death, but the real playback passes because the MOD fed the press 3 ticks late: the click facts name the call it was fed in
    {
        auto f = makeFile({{300, 0, 1}, {340, 0, 0}, {600, 0, 1}, {640, 0, 0}});
        Rig r; r.game.obstacles = {{310, 303, 310}};
        G::VerifyJob j(&r.game, f, r.sim, false); runToEnd(j);
        CHECK(j.done() && !j.ok());
        CHECK(playReal(r.game, f, 1000, 0, gdfwr::TraceEnd::Completed, 3));
        auto rep = G::makeTraceReport(j);
        CHECK(rep.compared); CHECK_EQ(rep.cmp.firstOver, 301);
        CHECK(lineHas(rep.log, "real playback applied it in the call at tick 303")); CHECK(lineHas(rep.log, "NOT the macro tick (+3)"));
        G::State::get().file.reset();
    }
    // T30: nothing is compared when the trace does not belong to the checked macro, when there is none, or when the setting is off; the text says why
    {
        auto f = makeFile({{100, 0, 1}, {150, 0, 0}});
        auto other = makeFile({{120, 0, 1}, {150, 0, 0}});
        Rig r; r.game.obstacles = {{110, 95, 125}};
        G::VerifyJob j(&r.game, f, r.sim); runToEnd(j);
        CHECK(j.ok());
        CHECK(playReal(r.game, other, 300));                                         // a playback of ANOTHER macro
        auto a = G::makeTraceReport(j);
        CHECK(!a.compared); CHECK(a.headline.rfind("no comparison:", 0) == 0); CHECK(lineHas(a.popup, "not the macro that was checked")); CHECK(a.callsCsv.empty());
        G::State::get().trace = gdfwr::RealTrace{};                                  // no playback at all
        auto b = G::makeTraceReport(j);
        CHECK(!b.compared); CHECK(lineHas(b.popup, "No real playback was recorded")); CHECK(lineHas(b.popup, "press Replay"));
        CHECK(playReal(r.game, f, 300));
        CHECK(G::makeTraceReport(j).compared);
        geode::Mod::stubBools()["trace-playback"] = false;                           // the setting off: nothing is recorded, nothing is compared
        CHECK(playReal(r.game, f, 300));
        CHECK(!G::State::get().trace.any()); CHECK(!G::State::get().trace.recording);
        auto c = G::makeTraceReport(j);
        CHECK(!c.compared); CHECK(lineHas(c.popup, "setting")); CHECK(lineHas(c.popup, "is off"));
        geode::Mod::stubBools()["trace-playback"] = true;
        G::State::get().file.reset();
    }
    // T31: the facts about HOW the silent replay stepped the game. The game's own call moved the clock by one tick per step; when it does not, the mod
    // moves it (as before) and the report counts how often.
    {
        auto f = makeFile({{100, 0, 1}, {150, 0, 0}});
        {
            Rig r; r.game.obstacles = {{110, 95, 105}};
            G::VerifyJob j(&r.game, f, r.sim); runToEnd(j);
            CHECK(j.ok()); CHECK_EQ(j.stepStats().steps, uint64_t(1000)); CHECK_EQ(j.stepStats().forced, uint64_t(0));
            CHECK(std::fabs(j.stepStats().advMin - 1.0) < 1e-6 && std::fabs(j.stepStats().advMax - 1.0) < 1e-6);
            CHECK(playReal(r.game, f, 1000));
            auto rep = G::makeTraceReport(j);
            CHECK(lineHas(rep.log, "1000 steps")); CHECK(textHas(rep.log, "moved by the game itself in 1000 of 1000 steps and by the mod in 0"));
            G::State::get().file.reset();
        }
        {
            Rig r; r.game.clockStuck = true; r.game.obstacles = {{110, 95, 105}};
            G::VerifyJob j(&r.game, f, r.sim); runToEnd(j);
            CHECK(j.ok()); CHECK_EQ(j.stepStats().steps, uint64_t(1000)); CHECK_EQ(j.stepStats().forced, uint64_t(1000));
            CHECK(j.stepStats().advMin == 0.0 && j.stepStats().advMax == 0.0);
            CHECK(j.baseline().samples.size() == 1000);                              // the silent replay behaves as before: the mod moved the clock
            r.game.clockStuck = false;
            CHECK(playReal(r.game, f, 1000));
            auto rep = G::makeTraceReport(j);
            CHECK(textHas(rep.log, "moved by the game itself in 0 of 1000 steps and by the mod in 1000"));
            G::State::get().file.reset();
        }
        G::SimCallStats sc;                                                          // what the hook counts while the silent replay runs
        sc.note(1.f / 240.f, false, true); sc.note(1.f / 480.f, true, false); sc.note(1.f / 240.f, false, true);
        CHECK_EQ(sc.calls, uint64_t(3)); CHECK_EQ(sc.half, uint64_t(1)); CHECK_EQ(sc.last, uint64_t(2)); CHECK(sc.dtMin == 1.f / 480.f && sc.dtMax == 1.f / 240.f);
        // a second verification run starts from zero (the probe steps and the earlier runs are not counted into the next report)
        {
            Rig r; r.game.obstacles = {{110, 95, 105}};
            G::VerifyJob j1(&r.game, f, r.sim); runToEnd(j1);
            G::VerifyJob j2(&r.game, f, r.sim); runToEnd(j2);
            CHECK_EQ(j1.stepStats().steps, uint64_t(1000)); CHECK_EQ(j2.stepStats().steps, uint64_t(1000));
        }
    }
    // T32: how a playback ends is kept in the trace; a second stop does nothing; a new playback starts a new trace
    {
        auto f = makeFile({{100, 0, 1}, {150, 0, 0}});
        Rig r;
        CHECK(playReal(r.game, f, 50, 0, gdfwr::TraceEnd::Left));
        auto& S = G::State::get();
        CHECK(S.trace.end == gdfwr::TraceEnd::Left); CHECK_EQ(S.trace.calls.size(), size_t(50)); CHECK(!S.replay.active);
        G::stopReplay("again", gdfwr::TraceEnd::Died);                               // not active any more: nothing changes
        CHECK(S.trace.end == gdfwr::TraceEnd::Left);
        CHECK_EQ(G::replayBegin(&r.game, 5, 1.f / 240.f, false, true), -1);          // no trace is recording: no entry, nothing breaks
        G::replayEnd(&r.game, -1); G::replayEnd(&r.game, 99999);
        CHECK(playReal(r.game, f, 20, 0, gdfwr::TraceEnd::Analyze));
        CHECK_EQ(S.trace.calls.size(), size_t(20)); CHECK(S.trace.end == gdfwr::TraceEnd::Analyze);   // the first playback's trace is gone
        S.file.reset();
    }
    // T33: the two csv files: one row per call and one row per tick; nothing is written when nothing was compared
    {
        auto f = makeFile({{100, 0, 1}, {150, 0, 0}, {300, 0, 1}, {340, 0, 0}});
        Rig r; r.game.obstacles = {{110, 95, 105}, {310, 295, 305}};
        G::VerifyJob j(&r.game, f, r.sim); runToEnd(j);
        CHECK(playReal(r.game, f, 1000, 1));
        auto rep = G::makeTraceReport(j);
        auto dir = std::filesystem::temp_directory_path() / "gdfwr_trace_test";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::string err;
        auto files = G::writeTraceFiles(rep, "My: Level", dir, err);
        CHECK(err.empty()); CHECK_EQ(files.size(), size_t(2));
        if (files.size() == 2) {
            auto count = [](std::filesystem::path const& p, std::string& first) {
                std::ifstream in(p, std::ios::binary);
                std::string line; int n = 0;
                while (std::getline(in, line)) { if (n == 0) first = line; n++; }
                return n;
            };
            std::string h1, h2;
            CHECK_EQ(count(files[0], h1), 1001); CHECK_EQ(count(files[1], h2), 1001);   // header + 1000 calls, header + 1000 ticks
            CHECK(h1.rfind("call,tick,clock,", 0) == 0); CHECK(h2.rfind("tick,silent_x1,", 0) == 0);
            CHECK(files[0].filename().string().find("My_ Level_") == 0);                // the level name is made safe for a file name
            CHECK(files[0].filename().string().find("_trace-real-playback.csv") != std::string::npos);
            CHECK(files[1].filename().string().find("_trace-compare.csv") != std::string::npos);
        }
        std::filesystem::remove_all(dir, ec);
        gdfwr::TraceReport none;                                                        // nothing compared: nothing written
        std::string err2;
        CHECK(G::writeTraceFiles(none, "x", dir, err2).empty()); CHECK(!std::filesystem::exists(dir, ec));
        G::State::get().file.reset();
    }
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
