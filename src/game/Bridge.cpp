// The ONLY place that reads game internals (implementation of Bridge.hpp).
// If a binding differs on your SDK, fix it here. Every line marked [BINDING] depends on a Geode / GD 2.2081 member or
// function name. Checked against geode-sdk/bindings (2.2081) and Geode v5.11.2.
//
// NOTE: the hook classes ($modify) live in Hooks.cpp. This file must NOT contain them (an earlier copy of Hooks.cpp was
// saved here by mistake, which left every function below undefined and broke the link step).
#include "Bridge.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/Util.hpp"

using namespace geode::prelude;

namespace gdfwr::game {
namespace {

// Random trigger (1912) and Advanced random trigger (2068). [BINDING] object ids
constexpr int kRandomTriggerId = 1912;
constexpr int kAdvancedRandomTriggerId = 2068;

// 2.2081: PlayerObject::m_holdingButtons is a map<int, bool> keyed by PlayerButton (Jump = 1), NOT an array. [BINDING]
bool heldIn(PlayerObject* p, int button) {
    if (!p) return false;
    auto it = p->m_holdingButtons.find(button);
    return it != p->m_holdingButtons.end() && it->second;
}

// Player 2 only takes part in 2-player levels and in dual mode; otherwise it is ignored everywhere (hash, sample, hold).
bool player2Active(GJBaseGameLayer* l) { return isTwoPlayer(l) || l->m_gameState.m_isDualMode; }

template <class T>
uint64_t mix(uint64_t h, T v) { return gdfwr::hashPod(h, v); }
uint64_t mixReal(uint64_t h, double v) {
    if (v == 0.0) v = 0.0;  // -0.0 and +0.0 are the same state
    return gdfwr::hashPod(h, v);
}

uint64_t hashPlayer(uint64_t h, PlayerObject* p) {
    if (!p) return mix(h, uint8_t(0));
    h = mix(h, uint8_t(1));
    h = mixReal(h, p->getPositionX());
    h = mixReal(h, p->getPositionY());
    h = mixReal(h, p->m_yVelocity);
    h = mixReal(h, p->m_playerSpeed);
    h = mixReal(h, p->m_gravityMod);
    uint32_t f = 0;
    int bit = 0;
    for (bool b : {p->m_isUpsideDown, p->m_isSideways, p->m_isShip, p->m_isBird, p->m_isBall, p->m_isDart, p->m_isRobot,
                   p->m_isSpider, p->m_isSwing, p->m_isOnGround, p->m_isDashing, p->m_isGoingLeft, heldIn(p, kJump)}) {
        if (b) f |= 1u << bit;
        bit++;
    }
    return mix(h, f);
}

void setHeld(PlayerObject* p, bool on) {
    if (p) p->m_holdingButtons[kJump] = on;
}

}  // namespace

// ---- tick / input ------------------------------------------------------------------------------------------------------------

// [BINDING] m_gameState.m_levelTime: seconds of level time. One tick = 1/240 s (Part 1, 2.4).
int tickOf(GJBaseGameLayer* l) {
    if (!l) return 0;
    return int(std::llround(l->m_gameState.m_levelTime * double(gdfwr::kTps)));
}

// [BINDING] PlayerObject::m_holdingButtons[Jump]
bool holding(PlayerObject* p) { return heldIn(p, kJump); }

// [BINDING] GJBaseGameLayer::m_levelSettings->m_twoPlayerMode
bool isTwoPlayer(GJBaseGameLayer* l) { return l && l->m_levelSettings && l->m_levelSettings->m_twoPlayerMode; }

// What the game really holds right now. In a 1-player level player 2 is not observed: `implied` is passed through, so
// the session never reports a missed event for a player that cannot receive input.
Hold holdState(GJBaseGameLayer* l, Hold implied) {
    Hold h = implied;
    if (!l) return h;
    h[0] = holding(l->m_player1) ? 1 : 0;
    if (isTwoPlayer(l)) h[1] = holding(l->m_player2) ? 1 : 0;
    return h;
}

// Makes the held state equal to `want` WITHOUT pushing a new press (a checkpoint load does not restore it). [BINDING]
void syncHold(PlayLayer* pl, Hold want) {
    if (!pl) return;
    setHeld(pl->m_player1, want[0] != 0);
    if (isTwoPlayer(pl)) setHeld(pl->m_player2, want[1] != 0);
}

// Part 5, step 1: "step the physics loop directly". One call = one tick, no rendering. How the game is advanced depends on
// State::stepMode, which calibrateStepper picks at the start of every verification (see below).
// [BINDING] processCommands(dt, isHalfTick, isLastTick) + PlayLayer::checkForEnd(), or GJBaseGameLayer::update(dt)
void stepTick(PlayLayer* pl) {
    if (!pl) return;
    auto& S = State::get();
    double before = pl->m_gameState.m_levelTime;
    if (S.stepMode == StepMode::Update) {
        // update(dt) runs round(dt * 60 * 4 / min(timeWarp, 1)) physics steps: with dt = 1/240 that is exactly one (timeWarp >= 1).
        float tw = pl->m_gameState.m_timeWarp;
        pl->update(float(kStep) * (tw > 0.f && tw < 1.f ? tw : 1.f));
    } else {
        pl->processCommands(float(kStep), false, true);
    }
    // The tick counter is the game's level time. If this build advances it somewhere else (not in the call above),
    // advance it here so that tickOf() still goes up by exactly one per step.
    if (pl->m_gameState.m_levelTime < before + kStep * 0.5) pl->m_gameState.m_levelTime = before + kStep;
    if (S.sim.dead) return;                              // a death in this tick wins over the end of the level
    if (S.stepMode == StepMode::Direct) pl->checkForEnd();  // the end test normally lives in postUpdate, which Direct does not run
}

// ---- end of the level (our own simulation) ------------------------------------------------------------------------------------

float playerX(PlayLayer* pl) { return pl && pl->m_player1 ? pl->m_player1->getPositionX() : 0.f; }

// [BINDING] GJBaseGameLayer::m_endPortal (EndPortalObject*). Every 2.2 level has one; if the member is empty the portal is looked
// for among the level objects once per run. 0 = not available.
namespace {
PlayLayer* s_portalFor = nullptr;
bool s_portalScanned = false;
float s_portalX = 0.f;
}
float endPortalX(PlayLayer* pl) {
    if (!pl) return 0.f;
    if (pl->m_endPortal) return pl->m_endPortal->getPositionX();
    if (s_portalFor != pl) { s_portalFor = pl; s_portalScanned = false; }
    if (!s_portalScanned) {
        s_portalScanned = true;
        s_portalX = 0.f;
        if (pl->m_objects) {
            for (auto* obj : CCArrayExt<GameObject*>(pl->m_objects)) {
                if (obj && typeinfo_cast<EndPortalObject*>(obj)) { s_portalX = obj->getPositionX(); break; }
            }
        }
    }
    return s_portalX;
}

// [BINDING] GJBaseGameLayer::m_levelEndAnimationStarted. The game sets it when the end animation starts.
void clearEndFlags(PlayLayer* pl) {
    if (pl) pl->m_levelEndAnimationStarted = false;
}

// Three tick-driven signs that the player has reached the end of the level alive. The first one that shows up wins; the same
// rule is used by the verification pass and by every shifted run, so both always agree on where the level ends.
//  1. State::sim.done: our hook of PlayLayer::playEndAnimationToPos / levelComplete saw the end (set by the caller, not here).
//  2. The game's own "end animation started" flag.
//  3. Position: the player is at or beyond the end portal. Only trusted when the portal really lies ahead of the start
//     (a portal that is not placed yet sits at x = 0 and must never end a run at tick 0).
char const* levelEndReason(PlayLayer* pl, float startX) {
    if (!pl) return nullptr;
    if (pl->m_levelEndAnimationStarted) return "end animation";
    float ex = endPortalX(pl);
    if (ex > startX + 120.f && playerX(pl) >= ex) return "end portal";
    return nullptr;
}

// ---- is the silent simulation really running? ----------------------------------------------------------------------------------
// The most damaging failure of all: the simulated player does not move (or does not move like in the real game), so the run
// neither dies nor ends and every verification says "did not reach the end". Before a verification the level is restarted and
// run for 60 ticks WITHOUT clicks in each way of stepping; the way that moves the player is used. Direct and Update are both
// the game's own code, so when both move the player by the same distance the faster one (Direct) is kept.
namespace {
constexpr int kProbeTicks = 60;

void kickStart(PlayLayer* pl) {
    if (!pl || pl->m_started) return;
    pl->startGame();                       // [BINDING] the game does this a moment AFTER a restart, in real time
    if (!pl->m_started) pl->m_started = true;
}

float probeRun(PlayLayer* pl, StepMode mode, bool kick) {
    auto& S = State::get();
    pl->resetLevelFromStart();
    clearEndFlags(pl);
    S.sim.clear();
    if (kick) kickStart(pl);
    S.stepMode = mode;
    float x0 = playerX(pl);
    for (int i = 0; i < kProbeTicks; i++) {
        stepTick(pl);
        if (S.sim.dead || S.sim.done) break;
    }
    return playerX(pl) - x0;
}
}  // namespace

StepProbe calibrateStepper(PlayLayer* pl) {
    StepProbe r;
    auto& S = State::get();
    S.stepMode = StepMode::Direct;
    S.stepKick = false;
    S.stepText.clear();
    if (!pl) return r;
    constexpr float kMoved = 1.f;
    char b[200];
    for (int pass = 0; pass < 2; pass++) {
        bool kick = pass == 1;
        if (kick) {
            pl->resetLevelFromStart();
            if (pl->m_started) continue;   // nothing to kick: the game had already started on its own
        }
        float dA = probeRun(pl, StepMode::Direct, kick);
        float dB = probeRun(pl, StepMode::Update, kick);
        std::snprintf(b, sizeof b, "%sdirect %+.1f, update %+.1f", kick ? "after start kick: " : "", dA, dB);
        if (!S.stepText.empty()) S.stepText += "; ";
        S.stepText += b;
        bool a = dA > kMoved, u = dB > kMoved;
        if (!a && !u) continue;
        r.moved = true;
        r.kick = kick;
        r.mode = (a && (!u || std::fabs(dA - dB) <= 0.5f)) ? StepMode::Direct : StepMode::Update;
        break;
    }
    S.stepMode = r.mode;
    S.stepKick = r.kick;
    r.text = S.stepText;
    log::info("GDFWR: stepper check: {} -> {}{}", r.text, r.moved ? (r.mode == StepMode::Direct ? "direct" : "update") : "the player did not move",
              r.kick ? " (with start kick)" : "");
    restartForSim(pl);
    return r;
}

void restartForSim(PlayLayer* pl) {
    if (!pl) return;
    auto& S = State::get();
    pl->resetLevelFromStart();
    clearEndFlags(pl);
    S.sim.clear();
    s_portalScanned = false;
    if (S.stepKick) kickStart(pl);
}

std::string simDiagnostics(PlayLayer* pl, float startX) {
    if (!pl) return {};
    auto& S = State::get();
    char b[420];
    std::snprintf(b, sizeof b,
                  "stepper: %s%s (%s)\nplayer x %.0f to %.0f, end portal x %.0f, level time %.2f s\nstarted %d, paused %d, end animation %d, GDFWR %s",
                  S.stepMode == StepMode::Direct ? "direct" : "update", S.stepKick ? " + start kick" : "", S.stepText.c_str(), startX, playerX(pl),
                  endPortalX(pl), pl->m_gameState.m_levelTime, pl->m_started ? 1 : 0, pl->m_isPaused ? 1 : 0, pl->m_levelEndAnimationStarted ? 1 : 0,
                  GDFWR_VERSION);
    return b;
}

// ---- level identity ----------------------------------------------------------------------------------------------------------

// [BINDING] GJGameLevel::m_levelString (compressed level data, stable for a given level version). FNV-1a 64.
uint64_t levelHash(GJGameLevel* level) {
    if (!level) return 0;
    uint64_t h = gdfwr::fnv1a64(level->m_levelString.c_str(), level->m_levelString.size());
    if (level->m_levelString.size() == 0) {  // data not loaded: never return the same constant for different levels
        h = mix(h, int32_t(level->m_levelID.value()));
        h = mix(h, int32_t(level->m_levelVersion));
    }
    return h;
}

// [BINDING] m_levelID.value(), m_levelName, m_levelVersion, isPlatformer(), m_levelSettings->m_startDual
gdfwr::LevelInfo captureLevel(PlayLayer* pl) {
    gdfwr::LevelInfo li;
    li.gameVersion = Loader::get()->getGameVersion();
    if (!pl || !pl->m_level) return li;
    auto* lv = pl->m_level;
    li.levelId = lv->m_levelID.value();
    li.name = std::string(lv->m_levelName);
    li.version = lv->m_levelVersion;
    li.dataHash = levelHash(lv);
    li.platformer = pl->m_isPlatformer || lv->isPlatformer();
    li.twoPlayer = isTwoPlayer(pl);
    li.dual = pl->m_levelSettings && pl->m_levelSettings->m_startDual;
    li.endTick = 0;  // filled in when the level is completed
    li.hasSeed = false;  // seeds are not captured in v1 (Part 1, 2.6)
    return li;
}

// Empty = this level can be recorded (Part 1, 2.1). v1: classic levels at 240 TPS only.
std::string unsupportedReason(PlayLayer* pl) {
    if (!pl || !pl->m_level) return "Open a level first.";
    if (pl->m_isPlatformer || pl->m_level->isPlatformer()) return "Platformer levels are not supported yet.";
    return {};
}

// x positions of Random / Advanced Random triggers, ascending. Used for the RANDOM flag (Part 1, 2.6). [BINDING] object ids
std::vector<float> randomTriggerXs(PlayLayer* pl) {
    std::vector<float> xs;
    if (!pl || !pl->m_objects) return xs;
    for (auto* obj : CCArrayExt<GameObject*>(pl->m_objects)) {
        if (!obj) continue;
        int id = obj->m_objectID;
        if (id == kRandomTriggerId || id == kAdvancedRandomTriggerId) xs.push_back(obj->getPositionX());
    }
    std::sort(xs.begin(), xs.end());
    xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
    return xs;
}

// ---- state for verification / analysis ---------------------------------------------------------------------------------------

// State at the START of a tick: the fields that the practice checkpoint restores (position, y velocity, mode flags,
// speed, gravity, ground, held jump) plus the tick itself. If clicks come out UNRELIABLE, a field is missing in the
// checkpoint restore: extend GameSimulator::run, not the engine (see README, "The one real risk").
uint64_t stateHash(PlayLayer* pl) {
    uint64_t h = 1469598103934665603ull;
    if (!pl) return h;
    h = mix(h, int32_t(tickOf(pl)));
    h = mix(h, uint8_t(pl->m_gameState.m_isDualMode ? 1 : 0));
    h = mixReal(h, pl->m_gameState.m_timeWarp);
    h = hashPlayer(h, pl->m_player1);
    if (player2Active(pl)) h = hashPlayer(h, pl->m_player2);
    return h;
}

// Position / velocity of both players (Part 3). Player 2 is zero in a 1-player, non-dual level.
gdfwr::Sample sampleOf(PlayLayer* pl) {
    gdfwr::Sample s;
    if (!pl) return s;
    if (auto* p = pl->m_player1) {
        s.x1 = p->getPositionX();
        s.y1 = p->getPositionY();
        s.vy1 = float(p->m_yVelocity);
    }
    if (auto* p = pl->m_player2; p && player2Active(pl)) {
        s.x2 = p->getPositionX();
        s.y2 = p->getPositionY();
        s.vy2 = float(p->m_yVelocity);
    }
    return s;
}

// ---- settings / UI -----------------------------------------------------------------------------------------------------------

// A click is only exact to one frame = 240 / FPS ticks, unless the player uses click-between-steps (Part 1, 2.3).
int clickResolution(float fps, bool* cbf) {
    bool exact = Mod::get()->getSettingValue<bool>("click-between-steps");
    if (cbf) *cbf = exact;
    if (exact) return 1;
    if (!(fps > 0.f)) fps = 60.f;
    int k = int(std::ceil(double(gdfwr::kTps) / double(fps)));
    return std::clamp(k, 1, gdfwr::kTps);
}

// Closes the pause menu (if open) the way the Resume button does. [BINDING] PauseLayer::onResume
void closePauseMenu() {
    auto* scene = CCDirector::get()->getRunningScene();
    if (!scene) return;
    if (auto* pause = scene->getChildByType<PauseLayer>(0)) pause->onResume(nullptr);
}

}  // namespace gdfwr::game
