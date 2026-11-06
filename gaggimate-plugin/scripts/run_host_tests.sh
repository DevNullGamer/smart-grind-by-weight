#!/bin/sh
# Builds and runs the plugin's host tests with plain g++ (no PlatformIO, no GaggiMate checkout needed).
# The tests include <unity.h>: here a small stand-in (test/host/unity.h); inside GaggiMate's `pio test -e native` the
# real Unity is used. Run from anywhere. Needs g++ with C++17.
set -e
cd "$(dirname "$0")/.."
OUT="${TMPDIR:-/tmp}/gaggimate-plugin-host-tests"
mkdir -p "$OUT"
PLUGIN=src/display/plugins/grinderlink
g++ -std=gnu++17 -Wall -Wextra -Werror -O1 -g \
    -I test/host -I "$PLUGIN" -I test/test_grinderlink \
    test/test_grinderlink/test_main.cpp test/test_grinderlink/test_glink_messages.cpp test/test_grinderlink/test_glink_client.cpp \
    "$PLUGIN/GLinkMessages.cpp" "$PLUGIN/GLinkClient.cpp" "$PLUGIN/bluetooth/glink_frame.cpp" "$PLUGIN/bluetooth/glink_json.cpp" \
    -o "$OUT/host_tests"
# glink_vectors.h looks for tools/tests/glink_vectors.json relative to the working directory: run from the plugin root.
"$OUT/host_tests"
