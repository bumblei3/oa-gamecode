#!/bin/sh
# Test 78: replay load and verify events
# CVars: replaytest78 1, autostart, failrun
# Erwünschte Marker: `saved . events to replay_78.dat`, `loaded . events`,
#   `NeonWave: REPLAY LOAD verify match=1`
# Anti-Patterns: keine Fatal-Warnung
#
# This test records 3 events (MOVE/AIM/FIRE) on wave 1, then on game over
# saves the replay file, reloads it, and verifies the event count + field
# values (timestampMs must be 0 for all 3 seeded events).
exec "$GAME_CODE_DIR/tests/helpers/autostart_test.sh" \
    --autostart \
    --timeout 60 \
    --extra-args "+set g_neonwave_replaytest78 1 +set g_neonwave_failrun 1" \
    --expected 'saved' \
    --expected 'loaded' \
    --expected 'NeonWave: REPLAY LOAD verify match=1' \
    "$@"
