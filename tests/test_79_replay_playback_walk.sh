#!/bin/sh
# Test 79: replay playback walk
# CVars: replaytest79 1, autostart, failrun
# Erwünschte Marker: `replay playback started`, `replay playback walked 4 events`
# Anti-Patterns: keine Fatal-Warnung
#
# Records 4 events (MOVE/AIM/FIRE/CROUCH) on wave 1.
# On game over: saves replay, reloads, starts playback, walks all events,
# prints count. Suite assert_79 checks the output.
exec "$GAME_CODE_DIR/tests/helpers/autostart_test.sh" \
    --autostart \
    --timeout 60 \
    --extra-args "+set g_neonwave_replaytest79 1 +set g_neonwave_failrun 1" \
    --expected 'replay playback started' \
    --expected 'replay playback walked 4 events' \
    "$@"
