#!/bin/sh
# Compiles the Geode glue (game/ and ui/) against tests/syntax-stubs, a tiny hand-written imitation of the API.
# It catches typos and type errors in OUR code. It does NOT prove that the binding names exist in the real SDK.
cd "$(dirname "$0")/.."
for f in src/game/*.cpp src/ui/*.cpp src/main.cpp; do
  ${CXX:-g++} -std=c++23 -fsyntax-only -Wall -Wextra -Wno-unused-parameter -Wno-overloaded-virtual \
      -Itests/syntax-stubs -Isrc -DGDFWR_VERSION='"0.3.1"' "$f" || exit 1
done
echo "glue syntax OK (stub API only)"
