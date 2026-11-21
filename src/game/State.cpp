#include "State.hpp"

#include <Geode/ui/Notification.hpp>

#include <ctime>

#include "Bridge.hpp"
#include "core/FileIO.hpp"

using namespace geode::prelude;

namespace gdfwr::game {

std::filesystem::path recordingsDir() { return Mod::get()->getSaveDir() / "gdfwr"; }

std::tm localTime() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

void notify(std::string const& text, bool good) {
    Notification::create(text, good ? NotificationIcon::Success : NotificationIcon::Error)->show();
}

void alertBox(std::string const& title, std::string const& text) {
    FLAlertLayer::create(title.c_str(), text, "OK")->show();
}

std::string describeCode(std::string const& c) {
    static const std::pair<const char*, const char*> t[] = {
        {"COARSE_INPUT", "Click timing is only exact to a few ticks (no click-between-frames)."},
        {"SYNTHETIC_EVENTS", "The mod added press/release events to keep the pairs correct."},
        {"RANDOM_UNCAPTURED", "The level has random triggers that could not be captured."},
        {"LEVEL_MISMATCH", "This file belongs to a different level."},
        {"LEVEL_CHANGED", "The level data changed since the file was made. Analyze again."},
        {"DERIVED_DROPPED", "A damaged derived section (baseline or analysis) was dropped."},
        {"TPS_CHANGED", "The step length looked different for a while during the recording (TPS bypass or a game slowdown?). Analysis still runs: if the macro was not recorded at 240 TPS, the check at the start of Analyze fails and says so."},
        {"RESTORE_REPLAYED", "The game did not restore some saved states exactly, so the clicks that start there were measured by replaying the level from the start (slower, still exact)."},
        {"UNRELIABLE_INPUT", "An input event may have been missed in a segment."},
        {"UNRELIABLE_TICK", "The tick counter was not trustworthy in a segment."},
        {"INVALID_SEQUENCE", "Press and release do not alternate. Fix it first."},
        {"IMPORT_UNKNOWN_FORMAT", "The file format is not recognised."},
        {"IMPORT_FILE_DAMAGED", "The file is damaged or cannot be read."},
        {"IMPORT_FPS_NOT_DIVISOR", "The macro's frame rate does not divide 240. It is refused, not rounded."},
        {"IMPORT_PLATFORMER", "Platformer macros are not supported."},
        {"IMPORT_NOT_FROM_START", "The macro does not start at the beginning of the level."},
        {"IMPORT_BAD_SEQUENCE", "The macro has an impossible event order."},
        {"IMPORT_PLAYER_MISMATCH", "The macro has player 2 inputs but the level is not 2-player."},
        {"IMPORT_DUPLICATE_DROPPED", "Duplicate press/release events were dropped."},
        {"IMPORT_ORPHAN_RELEASE", "A release without a press was dropped."},
        {"IMPORT_SYNTHETIC_RELEASE", "A release was added after the last press."},
        {"IMPORT_LEVEL_UNCHECKED", "The level could not be checked against the macro."},
        {"LOAD_NEEDS_NEWER_MOD", "This file needs a newer version of the mod."},
        {"LOAD_FILE_DAMAGED", "The file is damaged."},
        {"SAVE_WRITE_FAILED", "The file could not be written."},
    };
    for (auto const& [k, v] : t) if (c == k) return v;
    return c;
}

void beginSession(PlayLayer* pl) {
    auto& S = State::get();
    gdfwr::SessionConfig cfg;
    cfg.level = captureLevel(pl);
    cfg.timing.tps = gdfwr::kTps;
    cfg.timing.clickResolution = clickResolution(S.fpsAvg, &cfg.timing.cbf);
    cfg.modVersion = GDFWR_VERSION;
    cfg.createdUtc = std::time(nullptr);
    cfg.randomUncaptured = !randomTriggerXs(pl).empty();  // seeds / branches are not captured in v1 (Part 1, 2.6)
    S.tpsWarned = false;
    S.tpsOffSteps = 0;
    S.tpsLastTick = -1;
    S.lastTick = -1;
    S.session.begin(cfg, tickOf(pl), holdState(pl, {0, 0}));
}

void finishRecording(PlayLayer* pl, gdfwr::EndReason why) {
    auto& S = State::get();
    using St = gdfwr::SessionModel::State;
    if (S.session.state() == St::Idle) return;
    S.session.noteFps(S.fpsAvg, S.fpsMin);
    if (S.session.unfinished()) S.session.stop(pl ? tickOf(pl) : S.lastTick, why);
    gdfwr::GdfwrFile f = S.session.finish(Loader::get()->getGameVersion());
    S.session.abandon();
    S.file = std::move(f);
    S.path.clear();
    S.dirty = true;
    bool done = S.file->header.rec == gdfwr::RecStatus::Completed;
    if (Mod::get()->getSettingValue<bool>("auto-save")) {
        auto p = recordingsDir() / gdfwr::defaultFileName(S.file->level.name, localTime(), true, ".gdfwr");
        auto r = gdfwr::saveAtomic(p, *S.file);
        if (r.ok) {
            S.path = p;
            S.dirty = false;
            notify(std::string(done ? "Recording saved: " : "Partial recording saved: ") + p.filename().string());
        } else {
            alertBox("Could not save", r.errorText);
        }
    } else {
        notify(done ? "Recording finished (not saved yet). Open the GDFWR panel to save it." : "Partial recording kept (not saved yet).");
    }
}

}  // namespace gdfwr::game
