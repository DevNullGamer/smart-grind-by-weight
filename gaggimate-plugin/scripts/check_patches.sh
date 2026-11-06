#!/bin/sh
# Checks, read-only, that every patch in patches/ applies to a GaggiMate checkout (nothing is written to it).
#   scripts/check_patches.sh /path/to/gaggimate     (or set GAGGIMATE_CHECKOUT)
# The patches are made against GaggiMate commit b78beee. Exit codes: 0 all apply, 1 a patch does not apply,
# 2 BLOCKED (no checkout, not a git checkout, git missing). A checkout at another commit only gets a warning:
# the result then says whether the patches still apply, not that they are right.
PINNED=b78beee
HERE="$(cd "$(dirname "$0")/.." && pwd)"
CHECKOUT="${1:-$GAGGIMATE_CHECKOUT}"
if [ -z "$CHECKOUT" ] || [ ! -d "$CHECKOUT/.git" ]; then
    echo "BLOCKED: give the GaggiMate checkout as the first argument or in GAGGIMATE_CHECKOUT"; exit 2
fi
command -v git >/dev/null 2>&1 || { echo "BLOCKED: git not found"; exit 2; }
HEAD_SHA="$(git -C "$CHECKOUT" rev-parse --short=7 HEAD 2>/dev/null)" || { echo "BLOCKED: not a git checkout"; exit 2; }
if [ "$HEAD_SHA" != "$PINNED" ]; then echo "WARNING: checkout is at $HEAD_SHA, the patches are made against $PINNED"; fi
status=0
for p in "$HERE"/patches/*.patch; do
    if git -C "$CHECKOUT" apply --check "$p" 2>/dev/null; then echo "ok      $(basename "$p")"; else echo "FAILED  $(basename "$p")"; status=1; fi
done
# all together, in order (a later patch must not clash with an earlier one)
if [ $status -eq 0 ]; then
    if git -C "$CHECKOUT" apply --check "$HERE"/patches/*.patch 2>/dev/null; then echo "ok      all patches together"; else echo "FAILED  patches together"; status=1; fi
fi
exit $status
