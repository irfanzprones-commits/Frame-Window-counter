// Global mod state shared by hooks, jobs and UI. Everything that touches the game lives in game/ and ui/;
// everything under core/ is plain C++ without Geode.
#pragma once
#include <Geode/Geode.hpp>

#include <ctime>
#include <filesystem>
#include <optional>

#include "core/Metrics.hpp"
#include "core/Model.hpp"
#include "core/Session.hpp"

#ifndef GDFWR_VERSION
#define GDFWR_VERSION "0.1.0"
#endif

namespace gdfwr::game {

using Hold = std::array<uint8_t, 2>;
inline constexpr double kStep = 1.0 / 240.0;
inline constexpr int kTpsConfirmSteps = 12;   // consecutive steps of the wrong length before the TPS counts as changed (a lag spike is not a TPS change)

// Flags of OUR OWN re-simulation (verification / analysis). While `active`, all recording hooks stay out of the way.
struct SimFlags {
    bool active = false, dead = false, done = false;
    uint8_t diedMask = 0;
    void begin() { active = true; dead = done = false; diedMask = 0; }
    void clear() { dead = done = false; diedMask = 0; }
    void end() { active = false; clear(); }
};

// How OUR silent simulation advances the game by one tick (chosen by calibrateStepper, see Bridge.cpp):
//  Direct: GJBaseGameLayer::processCommands(1/240) + checkForEnd(). Fast, and what the first versions always did.
//  Update: GJBaseGameLayer::update(1/240), the game's own per-frame function (one physics step, end test included).
enum class StepMode : uint8_t { Direct = 0, Update = 1 };

struct ReplayState {
    bool active = false;
    std::vector<gdfwr::Event> events;  // final run, in execution order
    size_t next = 0;
    gdfwr::ReplayPlan plan;
    std::array<int, gdfwr::kReplayBucketCount> counters{};
    geode::Ref<cocos2d::CCNode> hud;  // ui::ReplayHud
};

struct State {
    static State& get() { static State s; return s; }

    gdfwr::SessionModel session;
    std::optional<gdfwr::GdfwrFile> file;  // the document the panel works on (the "current macro")
    std::filesystem::path path;            // empty = never saved
    bool dirty = false;
    bool pendingStart = false;             // Record was pressed: the next level reset starts the session
    SimFlags sim;
    ReplayState replay;
    StepMode stepMode = StepMode::Direct;  // set by calibrateStepper at the start of every verification
    bool stepKick = false;                 // the level needed PlayLayer::startGame() after a restart before the player moved
    std::string stepText;                  // what the calibration saw (for the failure report)

    // live tick bookkeeping (Part 1, 2.4: the counter must never go backwards)
    int lastTick = -1;
    double lastLevelTime = -1;
    bool justReset = true;
    bool tpsWarned = false;
    // TPS watch: only whole ticks (not half ticks) are compared, and the TPS is only called changed after kTpsConfirmSteps odd steps in a row
    int tpsLastTick = -1, tpsOffSteps = 0;
    double tpsLastTime = -1;
    float fpsAvg = 60.f, fpsMin = 1000.f;
    int fpsSamples = 0;
};

std::filesystem::path recordingsDir();
std::tm localTime();  // thread-safe, portable wrapper
void notify(std::string const& text, bool good = true);
void alertBox(std::string const& title, std::string const& text);

// Builds the session config from the loaded level and starts segment 0 (called from the level reset that Record triggers).
void beginSession(PlayLayer* pl);
// Stops the session, builds the document and (setting "auto-save") writes it to the recordings folder.
void finishRecording(PlayLayer* pl, gdfwr::EndReason why);
// Plain-language text for a warning / error code (Appendix B).
std::string describeCode(std::string const& code);
}  // namespace gdfwr::game
