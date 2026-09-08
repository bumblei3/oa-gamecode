#!/bin/sh
# Test 107: Ghost Loadout - Saboteur via cvar
# CVars: g_neonwave_ghost 1, g_ghost_loadout 1, autostart
# Erwünschte Marker: "Ghost: joined the Ghost team (loadout 1)"
# Anti-Patterns: "loadout 0", "loadout 2"
#
# g_ghost_loadout 1 forces Saboteur. Test verifies the join message.
exec tests/helpers/autostart_test.sh \
    --autostart \
    --timeout 60 \
    --extra-args "+set g_neonwave_ghost 1 +set g_ghost_loadout 1 +set g_neonwave_failrun 1" \
    --expected "Ghost: joined the Ghost team (loadout 1)" \
    "$@"
