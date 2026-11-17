#!/bin/sh
# Builds and runs the Geode-free core tests. Needs only a C++23 compiler (g++ 13+, clang 17+).
set -e
cd "$(dirname "$0")/.."
${CXX:-g++} -std=c++23 -O1 -g -Wall -Wextra -Isrc tests/test_core.cpp src/core/*.cpp -o /tmp/gdfwr_tests
/tmp/gdfwr_tests
