// The ONLY place that reads game internals. If a binding differs on your SDK, fix it here.
// Every line marked [BINDING] depends on a Geode / GD 2.2081 member or function name.
#pragma once
#include "State.hpp"
#include "core/Analysis.hpp"

namespace gdfwr::game {

int tickOf(GJBaseGameLayer* l);                 // [BINDING] m_gameState.m_levelTime
bool holding(PlayerObject* p);                  // [BINDING] m_holdingButtons
bool isTwoPlayer(GJBaseGameLayer* l);           // [BINDING] m_levelSettings->m_twoPlayerMode
Hold holdState(GJBaseGameLayer* l, Hold implied);
std::string unsupportedReason(PlayLayer* pl);   // empty = this level can be recorded (Part 1, 2.1)
gdfwr::LevelInfo captureLevel(PlayLayer* pl);
uint64_t levelHash(GJGameLevel* level);
std::vector<float> randomTriggerXs(PlayLayer* pl);
uint64_t stateHash(PlayLayer* pl);              // state at the START of a tick (Part 3 / 4.10)
gdfwr::Sample sampleOf(PlayLayer* pl);
void syncHold(PlayLayer* pl, Hold want);
// Advance the physics by exactly one tick without rendering (Part 5, step 1: "step the physics loop directly").
void stepTick(PlayLayer* pl);                   // [BINDING] processCommands + checkForEnd, or update(), see State::stepMode

// ---- making sure the silent simulation really runs ----
// Result of the start-up check of the stepper: does the player move, and by which way of stepping.
struct StepProbe {
    bool moved = false;
    StepMode mode = StepMode::Direct;
    bool kick = false;
    bool died = false;       // v0.2.1: the player died inside the probe even in the way that was chosen (no click was applied)
    int deathTick = -1;
    std::string text;
};
// Restarts the level from the start and finds the way of stepping (and the start-up kick) that makes the player move. The choice is
// stored in State (stepMode / stepKick / stepText). Leaves the level restarted at tick 0 with the simulation flags cleared.
StepProbe calibrateStepper(PlayLayer* pl);
// Restart for a simulation run: level from the start, end flags cleared, the start-up kick applied when calibrateStepper needed it.
void restartForSim(PlayLayer* pl);
// One line of facts about the simulated game, for the failure report: "x 0 -> 44, end portal x 4000, started 1, paused 0 ...".
std::string simDiagnostics(PlayLayer* pl, float startX);

// ---- what killed the player in OUR OWN run (v0.2.1: for the failure report and the log) ----
// [BINDING, optional] true when `obj` is the game's own anti-cheat spike (PlayLayer::m_anticheatSpike). It is not a hazard of the level
// (noclip mods let it through on purpose). Always false on an SDK that has no such member: the code then simply compiles without it.
bool isAnticheatSpike(PlayLayer* pl, GameObject* obj);
// What the destroyPlayer hook does in OUR OWN run (Hooks.cpp calls it; tests/verify_sim_test.cpp calls it too): a call made with the anti-cheat
// spike is ignored (it is not a hazard of the level); any other call is a death: State::sim.dead is set and the facts are recorded.
void simDestroyPlayer(PlayLayer* pl, PlayerObject* player, GameObject* obj);
// "Killed by: object id 8 at (x, y); player at (x, y)." Empty when the hook saw no call.
std::string killerText();

// ---- end of the level, for OUR OWN simulation (verification / analysis) ----
// The game ends a level with an animation and a DELAYED levelComplete() (a timed action on the PlayLayer). Our stepping is
// much faster than real time, so waiting for levelComplete() never works. The end is detected from the tick-driven facts instead.
float playerX(PlayLayer* pl);                   // x of player 1 (0 when there is no player)
float endPortalX(PlayLayer* pl);                // [BINDING] m_endPortal x (else the end portal found among the objects); 0 when there is none
void clearEndFlags(PlayLayer* pl);              // [BINDING] m_levelEndAnimationStarted = false (a stale flag would end the run at tick 0)
// nullptr = the level has not ended. Otherwise a short reason: "end animation" / "end portal".
// startX = x of the player at tick 0 (guards the position rule against a portal that is not placed yet).
char const* levelEndReason(PlayLayer* pl, float startX);
int clickResolution(float fps, bool* cbf);
void closePauseMenu();

}  // namespace gdfwr::game
