#!/bin/sh
# Test 107: Ghost Loadout - Saboteur EMP cost reduction
# CVars: g_neonwave_ghost 1, autostart, g_ghost_loadout 1
# Erwünschte Marker: "SABOTEUR", "EMP_COST=25"
# Anti-Patterns: "EMP_COST=35"
#
# Saboteur EMP costs 25 energy (vs 35 default). Test verifies loadout command
# switches to Saboteur and the cost reduction is active.
exec tests/helpers/autostart_test.sh \
    --autostart \
    --timeout 60 \
    --extra-args "+set g_neonwave_ghost 1 +set g_ghost_loadout 1 +set g_neonwave_failrun 1" \
    --expected "LOADOUT: SABOTEUR" \
    --pattern "EMP_COST=25" \
    "$@"
