#include <Geode/Geode.hpp>
#include <Geode/loader/GameEvent.hpp>
#include <Geode/loader/SettingV3.hpp>

#include "ui/Panel.hpp"

using namespace geode::prelude;

// Hotkeys are settings of type "keybind" (mod.json), so the player rebinds them in the mod settings.
$on_game(Loaded) {
    listenForKeybindSettingPresses("key-panel", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat) gdfwr::ui::openPanel();
    });
    listenForKeybindSettingPresses("key-record", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat) gdfwr::ui::toggleRecording();
    });
    listenForKeybindSettingPresses("key-pause-record", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat) gdfwr::ui::togglePauseRecording();
    });
}
