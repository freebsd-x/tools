#!/bin/sh
# CI gate for FreeBSD-X: full build + unit tests + smoke tests.
#
# Runs the three checks that catch the regressions manual testing misses:
#   1. a *full* ninja build (all targets — catches broken diagnostic/test
#      targets that a scoped `ninja freebsdx` would skip),
#   2. the meson unit-test suite (pure logic),
#   3. every *-smoke test against a throwaway headless compositor (real
#      inject/observe checks — cursor shapes, services, etc.).
#
# Safe to run anywhere: --headless never touches the seat. The compositor is
# always torn down on exit. Exit 0 = everything passed.

set -u
cd "$(dirname "$0")/.." || exit 2

BUILD=out
LOG=/tmp/freebsdx-check.log
fail=0
FB=""

cleanup() { [ -n "$FB" ] && kill -TERM "$FB" 2>/dev/null; rm -f /tmp/freebsdx.sock; }
trap cleanup EXIT INT TERM

hdr() { printf '\n\033[1;36m== %s ==\033[0m\n' "$1"; }

hdr "Build (all targets)"
if ! ninja -C "$BUILD"; then
    printf '\033[1;31mBUILD FAILED\033[0m\n'; exit 1
fi

hdr "Unit tests"
if ! meson test -C "$BUILD" --suite unit --print-errorlogs; then
    fail=1
fi

hdr "Smoke tests (headless compositor)"
rm -f /tmp/freebsdx.sock
# Launch from inside $BUILD: the compositor loads its SPIR-V shaders from
# "freebsdx.p/*.spv" relative to CWD, so a wrong CWD leaves graphics_pipeline_
# null and no textured surface can be drawn (the gui-smoke test needs this).
( cd "$BUILD" && exec ./freebsdx --headless ) > "$LOG" 2>&1 &
FB=$!
up=0
i=0
while [ "$i" -lt 40 ]; do
    grep -q "up and running" "$LOG" 2>/dev/null && { up=1; break; }
    kill -0 "$FB" 2>/dev/null || break   # compositor died
    sleep 0.25
    i=$((i + 1))
done
if [ "$up" -ne 1 ]; then
    printf '\033[1;31mcompositor failed to start\033[0m\n'; tail -8 "$LOG"; exit 1
fi

ran=0
for t in "$BUILD"/freebsdx-*-smoke; do
    [ -x "$t" ] || continue
    ran=$((ran + 1))
    name=$(basename "$t")
    # Run from inside $BUILD: smoke tests that spawn a real xkit client (e.g.
    # the widget test) need CWD to hold freebsdx.p/*.spv for the client renderer.
    if out=$(cd "$BUILD" && ./"$name" 2>&1); then
        printf '  \033[1;32mPASS\033[0m  %s\n' "$name"
    else
        printf '  \033[1;31mFAIL\033[0m  %s\n' "$name"
        printf '%s\n' "$out" | sed 's/^/      /'
        fail=1
    fi
done
[ "$ran" -eq 0 ] && { printf '\033[1;31mno smoke tests found\033[0m\n'; fail=1; }

if [ "$fail" -eq 0 ]; then
    printf '\n\033[1;32m== check: PASS ==\033[0m\n'
else
    printf '\n\033[1;31m== check: FAIL ==\033[0m\n'
fi
exit "$fail"
