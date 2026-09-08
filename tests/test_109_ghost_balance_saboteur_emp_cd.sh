#!/bin/sh
# Test 109: Ghost Balance - Saboteur EMP-CD 22s
# CVars: g_neonwave_ghost 1, g_ghost_loadout 1, autostart
# Erwünschte Marker: "Ghost: joined the Ghost team (loadout 1)"
# Anti-Patterns: "loadout 0", "loadout 2"
#
# Verifies Saboteur loadout is active. EMP-CD is 22s (GH_EMP_CD - 3000 = 22000).
# Test validates loadout switch and energy budget.
exec tests/helpers/autostart_test.sh \
    --autostart \
    --timeout 60 \
    --extra-args "+set g_neonwave_ghost 1 +set g_ghost_loadout 1 +set g_neonwave_failrun 1" \
    --expected "Ghost: joined the Ghost team (loadout 1)" \
    --pattern "ENERGY=70" \
    "$@"
