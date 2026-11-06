#!/bin/sh
# Stages the plugin into a CLEAN clone of GaggiMate b78beee and builds it there:
#   scripts/check_against_gaggimate.sh DEST [SOURCE]
#     DEST    a directory that does not exist yet or is empty; the clone is made there. An existing checkout is never
#             reused or modified (this script refuses a non-empty DEST).
#     SOURCE  what to clone: a local checkout (preferred, no network) or a URL
#             (default: GAGGIMATE_SOURCE, else https://github.com/jniebuhr/gaggimate.git).
# Steps: clone, check out b78beee, `git apply` the patches in order, copy the plugin (src/display/plugins/grinderlink,
# the host tests, the web card), stage tools/tests/glink_vectors.json, then
#   pio test -e native -f test_grinderlink   (the gate), then pio test -e native for information
#   pio run -e display
#   pio run -e display-headless   only when GLINK_HEADLESS=1
# Nothing else in the clone runs except what PlatformIO runs for those commands. Logs go to DEST/.glink-check/.
# Set PLATFORMIO_CORE_DIR to keep the toolchain downloads away from your global ~/.platformio.
# Exit codes: 0 everything passed, 1 a step failed, 2 BLOCKED (no git/pio, no clone possible, DEST not empty, wrong commit).
# A BLOCKED or failed run says why; nothing here replaces it with a weaker check.
PINNED=b78beee
HERE="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$1"
SOURCE="${2:-${GAGGIMATE_SOURCE:-https://github.com/jniebuhr/gaggimate.git}}"

blocked() { echo "BLOCKED: $1"; exit 2; }
failed() { echo "FAILED: $1"; exit 1; }

[ -n "$DEST" ] || blocked "give a new or empty directory as the first argument"
mkdir -p "$(dirname "$DEST")" 2>/dev/null
DEST="$(cd "$(dirname "$DEST")" 2>/dev/null && pwd)/$(basename "$DEST")" || blocked "cannot resolve $DEST"
if [ -e "$DEST" ] && [ -n "$(ls -A "$DEST" 2>/dev/null)" ]; then blocked "$DEST is not empty (the check needs a fresh clone)"; fi
command -v git >/dev/null 2>&1 || blocked "git not found"
command -v pio >/dev/null 2>&1 || blocked "pio (PlatformIO) not found"

git clone -q --no-hardlinks "$SOURCE" "$DEST" || blocked "cannot clone $SOURCE (no network or not a repository)"
git -C "$DEST" -c advice.detachedHead=false checkout -q "$PINNED" || blocked "commit $PINNED is not in $SOURCE"
HEAD_SHA="$(git -C "$DEST" rev-parse --short=7 HEAD)"
[ "$HEAD_SHA" = "$PINNED" ] || blocked "clone is at $HEAD_SHA, expected $PINNED"
[ -z "$(git -C "$DEST" status --porcelain)" ] || blocked "the fresh clone is not clean"
echo "clone: $DEST at $HEAD_SHA"

for p in "$HERE"/patches/*.patch; do
    git -C "$DEST" apply "$p" || failed "patch $(basename "$p") does not apply"
    echo "applied $(basename "$p")"
done

mkdir -p "$DEST/src/display/plugins" "$DEST/test" "$DEST/tools/tests" "$DEST/web/src/pages/Settings"
cp -R "$HERE/src/display/plugins/grinderlink" "$DEST/src/display/plugins/" || failed "copy of the plugin sources"
cp -R "$HERE/test/test_grinderlink" "$DEST/test/" || failed "copy of the plugin tests"
cp "$HERE/tools/tests/glink_vectors.json" "$DEST/tools/tests/" || failed "staging glink_vectors.json"
cp "$HERE/web/src/pages/Settings/GrinderLinkCard.jsx" "$DEST/web/src/pages/Settings/" || failed "copy of the web card"
echo "staged the plugin, the tests, the web card and glink_vectors.json"

LOGS="$DEST/.glink-check"
mkdir -p "$LOGS"
cd "$DEST" || failed "cannot enter $DEST"

run() { # name, log, command...
    name="$1"; log="$LOGS/$2"; shift 2
    echo "running: $* (log: $log)"
    if "$@" >"$log" 2>&1; then echo "PASS $name"; else echo "FAIL $name"; tail -40 "$log"; exit 1; fi
}

# The tests include the plugin's headers by plain name and glink_vectors.h looks for tools/tests/glink_vectors.json below
# the working directory (the project root); the include paths are added through the environment, platformio.ini stays as is.
PLATFORMIO_BUILD_FLAGS="-Isrc/display/plugins/grinderlink -Itest/test_grinderlink"
export PLATFORMIO_BUILD_FLAGS
run "pio test -e native -f test_grinderlink" native-grinderlink.log pio test -e native -f test_grinderlink
grep -E 'test cases: ' "$LOGS/native-grinderlink.log" | tail -1
# The other suites of the tree are not part of the gate (on Windows test_ota_loadtest does not compile: arpa/inet.h);
# their result is printed for information.
if pio test -e native >"$LOGS/native-all.log" 2>&1; then echo "info: pio test -e native (all suites): PASS"; else echo "info: pio test -e native (all suites): not all suites passed"; fi
grep -E "^native +test_|test cases:" "$LOGS/native-all.log"
unset PLATFORMIO_BUILD_FLAGS
run "pio run -e display" display.log pio run -e display
if [ "$GLINK_HEADLESS" = "1" ]; then run "pio run -e display-headless" headless.log pio run -e display-headless; fi
grep -E "CONFIG_BT_NIMBLE_MAX_CONNECTIONS" src/display/plugins/grinderlink/GLinkPlugin.cpp | head -1
echo "ALL PASSED"
