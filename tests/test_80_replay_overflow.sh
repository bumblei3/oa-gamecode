#!/bin/sh
# Test 80: replay overflow
# CVars: replaytest80 1, autostart, failrun
# Erwünschte Marker: `NeonWave: REPLAY overflow recorded=32773 stored=32768`
# Anti-Patterns: keine Fatal-Warnung
#
# Records REPLAY_MAX_EVENTS+5 = 32768+5 = 32773 events on wave 1.
# On game over: prints overflow info. Suite assert_80 checks the output.
exec "$GAME_CODE_DIR/tests/helpers/autostart_test.sh" \
    --autostart \
    --timeout 90 \
    --extra-args "+set g_neonwave_replaytest80 1 +set g_neonwave_failrun 1" \
    --expected 'NeonWave: REPLAY overflow recorded=32773 stored=32768' \
    "$@"
