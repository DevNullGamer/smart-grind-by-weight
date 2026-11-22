#!/bin/sh
# Builds and runs the plugin's host tests with plain g++ (no PlatformIO, no GaggiMate checkout needed).
# The tests include <unity.h>: here a small stand-in (test/host/unity.h); inside GaggiMate's `pio test -e native` the
# real Unity is used. Run from anywhere. Needs g++ with C++17.
# Every test/test_grinderlink/test_*.cpp is built, and every pure .cpp of the plugin folder; the device-only glue
# (GLinkPlugin.cpp, GLinkGrinder.cpp) is left out.
set -e
cd "$(dirname "$0")/.."
OUT="${TMPDIR:-/tmp}/gaggimate-plugin-host-tests"
mkdir -p "$OUT"
PLUGIN=src/display/plugins/grinderlink
SRCS=""
for f in "$PLUGIN"/*.cpp "$PLUGIN"/bluetooth/*.cpp; do
    case "$(basename "$f")" in GLinkPlugin.cpp|GLinkGrinder.cpp) continue ;; esac
    SRCS="$SRCS $f"
done
TESTS="$(ls test/test_grinderlink/test_*.cpp)"
FLAGS="-std=gnu++17 -Wall -Wextra -Werror -O1 -g -I test/host -I $PLUGIN -I test/test_grinderlink"
# `pio test -e native` inside GaggiMate compiles every pure source into ONE translation unit (glink_sources.cpp): names that
# are fine in separate files can clash there. Catch that on the host too.
g++ $FLAGS -fsyntax-only test/test_grinderlink/glink_sources.cpp
g++ $FLAGS $TESTS $SRCS -o "$OUT/host_tests"
# glink_vectors.h looks for tools/tests/glink_vectors.json relative to the working directory: run from the plugin root.
"$OUT/host_tests"
