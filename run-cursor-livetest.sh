#!/bin/sh
# Safe live-seat launcher for verifying cursor-shape behaviour on real hardware.
#
# WHY A WRAPPER: freebsdx (non-headless) takes DRM master + seatd + libinput and
# owns the display for its whole run — your normal seat/CLI is unavailable until
# it exits. This wrapper bounds that with a hard timeout so the session always
# comes back on its own, and tees logs so you can inspect what happened after.
#
# RECOVERY while it runs:
#   * Ctrl+Alt+Esc      → built-in emergency shutdown (sends SIGTERM)
#   * otherwise it self-exits after ${TIMEOUT:-60}s via timeout(1)
#
# Run it yourself from a seat you're willing to have taken over (ideally a spare
# VT, e.g. switch with Ctrl+Alt+F2 first). Do NOT run it from the terminal that
# hosts a Claude Code session you want to keep using.

set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/out"
TIMEOUT="${TIMEOUT:-60}"
# This launcher exists to verify the desktop/cursor, so boot straight to xshell
# and skip the login greeter by default. Set FREEBSDX_SKIP_GREETER=0 to keep the
# greeter (to test the login flow).
export FREEBSDX_SKIP_GREETER="${FREEBSDX_SKIP_GREETER:-1}"
LOG="$ROOT/logs/cursor-livetest-$(date +%Y%m%d-%H%M%S).log"
mkdir -p "$ROOT/logs"

# Always build first so you never accidentally run a stale binary that predates
# a fix (set SKIP_BUILD=1 to bypass).
if [ -z "${SKIP_BUILD:-}" ]; then
    echo "Building (ninja -C $BUILD freebsdx xshell)..."
    if ! ninja -C "$BUILD" freebsdx xshell; then
        echo "error: build failed — fix the build before launching" >&2
        exit 1
    fi
fi

if [ ! -x "$BUILD/freebsdx" ]; then
    echo "error: $BUILD/freebsdx not found — build first: ninja -C out freebsdx" >&2
    exit 1
fi

# Clear any leftover compositor from a previous run BEFORE starting. A stale
# freebsdx still holding /tmp/freebsdx.sock makes a fresh compositor unable to
# bind it, which breaks client/input setup (frozen cursor, dead Ctrl+Alt+Esc).
pkill -9 -f "$BUILD/freebsdx" 2>/dev/null || true
pkill -9 -f "$BUILD/xshell"   2>/dev/null || true
rm -f /tmp/freebsdx.sock

echo "Launching freebsdx for ${TIMEOUT}s (log: $LOG)."
echo "Emergency exit: Ctrl+Alt+Esc.  It will also self-exit after ${TIMEOUT}s."
# Run from the build dir: the compositor (and the xshell it spawns) load their
# SPIR-V shaders from freebsdx.p/*.spv relative to CWD, so a wrong CWD leaves the
# graphics pipeline null and nothing textured renders.
cd "$BUILD"
# timeout sends SIGTERM at the deadline; -k 5 escalates to SIGKILL 5s later so a
# deadlocked compositor can't keep the seat frozen (the kernel drops DRM master
# on death) — you should never have to hard-reboot. stdbuf -oL line-buffers the
# output so the log captures how far init got even if it hangs, and sync flushes
# it to disk so it survives even a subsequent reboot.
stdbuf -oL -eL timeout -k 5 --signal=TERM "${TIMEOUT}" ./freebsdx 2>&1 | tee "$LOG"
sync
echo "Session ended. Log saved to: $LOG"
