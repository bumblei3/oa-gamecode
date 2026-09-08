#!/bin/sh
# Test 108: Ghost Loadout - Spectre (loadout 2)
# CVars: g_neonwave_ghost 1, g_ghost_loadout 2, autostart
# Erwünschte Marker: "Ghost: joined the Ghost team (loadout 2)", "ENERGY=90"
# Anti-Patterns: "loadout 0", "loadout 1"
#
# Spectre has 90 start energy (highest). Test verifies loadout switch.
exec tests/helpers/autostart_test.sh \
    --autostart \
    --timeout 60 \
    --extra-args "+set g_neonwave_ghost 1 +set g_ghost_loadout 2 +set g_neonwave_failrun 1" \
    --expected "Ghost: joined the Ghost team (loadout 2)" \
    "$@"
