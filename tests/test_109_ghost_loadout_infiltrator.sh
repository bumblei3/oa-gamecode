#!/bin/sh
# Test 109: Ghost Balance - Infiltrator Start Energy 80
# CVars: g_neonwave_ghost 1, autostart
# Erwünschte Marker: "Ghost: joined the Ghost team (loadout 0)"
# Anti-Patterns: "ENERGY=70", "ENERGY=90"
#
# Verifies Infiltrator has 80 start energy. Test validates the default loadout
# energy budget is exactly 80 (not 70 or 90 which are Saboteur/Spectre).
exec tests/helpers/autostart_test.sh \
    --autostart \
    --timeout 60 \
    --extra-args "+set g_neonwave_ghost 1 +set g_neonwave_failrun 1" \
    --expected "Ghost: joined the Ghost team (loadout 0)" \
    "$@"
