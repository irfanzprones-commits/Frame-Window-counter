#!/bin/sh
# Runs the REAL replay HUD, ring sound and pitch popup (src/ui/Overlays.cpp, PitchPopup.cpp, src/game/RingSound.cpp, and feedReplay of src/game/Jobs.cpp) against the stub of the Geode API
# (tests/hud_test.cpp): rings that follow a scrolling level, the invisible "any" ring, the sound and its pitch, every button of the popup, the picture
# files. It tests OUR logic, not Geometry Dash. Needs only a C++23 compiler.
set -e
cd "$(dirname "$0")/.."
${CXX:-g++} -std=c++23 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-overloaded-virtual -Itests/syntax-stubs -Isrc -DGDFWR_VERSION='"test"' \
    tests/hud_test.cpp src/ui/Overlays.cpp src/ui/PitchPopup.cpp src/game/RingSound.cpp src/game/Jobs.cpp src/game/State.cpp src/game/Bridge.cpp src/core/*.cpp -o /tmp/gdfwr_hud_test
/tmp/gdfwr_hud_test
