#!/bin/sh
# Helper: autostart test runner for NeonArena headless tests.
# Wraps ioq3ded with autostart cvars and optional extra cvars, then
# greps the log for --expected and --pattern markers.
#
# Usage: autostart_test.sh --autostart [--startwave N] [--cvar name val ...]
#                            [--expected PATTERN] [--pattern PATTERN ...]
#                            [--extra-args "..."] [--timeout SECS] "$@"
#
# The helper builds the ioq3ded command line, runs it, saves the log to
# $LOGDIR/test${TEST_NUM}.log (set by the caller / run_suite.sh), and
# greps for the required markers. Exit 0 on match, 1 on miss.

set -u

# ---- defaults ----
EXTRA_ARGS=""
TIMEOUT_SECS="${TIMEOUT_SECS:-60}"
EXPECTED=""
PATTERNS=""
CVARS=""
AUTOSTART=0
STARTWAVE=0

# ---- parse args ----
while [ $# -gt 0 ]; do
  case "$1" in
    --autostart) AUTOSTART=1; shift ;;
    --startwave) STARTWAVE="$2"; shift 2 ;;
    --cvar) CVARS="$CVARS +set $2"; shift 2 ;;
    --expected) EXPECTED="$2"; shift 2 ;;
    --pattern) PATTERNS="$PATTERNS $2"; shift 2 ;;
    --timeout) TIMEOUT_SECS="$2"; shift 2 ;;
    --extra-args) EXTRA_ARGS="$2"; shift 2 ;;
    *) shift ;;  # ignore unknown (forwarded to suite)
  esac
done

# ---- build command ----
CMDLINE="+set dedicated 1"
CMDLINE="$CMDLINE +set fs_homepath \"$(na_root)\""
CMDLINE="$CMDLINE +set sv_maxclients 24"
CMDLINE="$CMDLINE +set fs_game neonarena +set g_gametype 14 +map oa_shine"
if [ "$AUTOSTART" -eq 1 ]; then
  CMDLINE="$CMDLINE +set g_neonwave_autostart 1"
fi
if [ "$STARTWAVE" -gt 0 ]; then
  CMDLINE="$CMDLINE +set g_neonwave_startwave $STARTWAVE"
fi
CMDLINE="$CMDLINE $CVARS"
CMDLINE="$CMDLINE $EXTRA_ARGS"

# ---- run ----
LOGDIR="${LOGDIR:-/tmp/nw-suite-$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$LOGDIR"
TEST_NUM="${TEST_NUM:-0}"
LOGFILE="$LOGDIR/test${TEST_NUM}.log"

RUNNER=""
if command -v xvfb-run >/dev/null && [ "${OA_BIN:-}" != "/usr/lib/ioquake3/ioq3ded" ]; then
  RUNNER="xvfb-run -a"
fi

$RUNNER timeout "$TIMEOUT_SECS" "${OA_BIN:-/usr/lib/ioquake3/ioq3ded}" $CMDLINE > "$LOGFILE" 2>&1 || true

# ---- assert ----
FAILED=0
if [ -n "$EXPECTED" ]; then
  grep -q "$EXPECTED" "$LOGFILE" || FAILED=1
fi
for p in $PATTERNS; do
  grep -q "$p" "$LOGFILE" || FAILED=1
done

if [ "$FAILED" -eq 1 ]; then
  echo "FAIL: missing expected/pattern markers in $LOGFILE"
  echo "--- log tail ---"
  tail -20 "$LOGFILE"
  exit 1
fi
echo "PASS: all markers found"
exit 0
