#!/bin/sh
# Test 106: Ghost Loadout - Infiltrator default
# CVars: g_neonwave_ghost 1, autostart
# Erwünschte Marker: "Ghost: joined the Ghost team (loadout 0)"
# Anti-Patterns: "loadout 1", "loadout 2"
#
# Default loadout is Infiltrator (0). Test verifies the log message.
exec tests/helpers/autostart_test.sh \
    --autostart \
    --timeout 60 \
    --extra-args "+set g_neonwave_ghost 1 +set g_neonwave_failrun 1" \
    --expected "Ghost: joined the Ghost team (loadout 0)" \
    "$@"
