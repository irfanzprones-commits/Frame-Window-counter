#!/bin/sh
# Runs the real VerifyJob / repair state machine against a fake game (tests/verify_sim_test.cpp). Needs only a C++23 compiler.
# It tests OUR logic, not Geometry Dash: the fake game is ours too.
set -e
cd "$(dirname "$0")/.."
${CXX:-g++} -std=c++23 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-overloaded-virtual -Itests/syntax-stubs -Isrc -DGDFWR_VERSION='"test"' \
    tests/verify_sim_test.cpp src/game/Jobs.cpp src/game/Bridge.cpp src/game/State.cpp src/core/*.cpp -o /tmp/gdfwr_verify_sim
/tmp/gdfwr_verify_sim
