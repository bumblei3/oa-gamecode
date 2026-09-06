#!/bin/sh
# Test 43: Rocket Launcher Weapon
# CVars: autostart, startwave 10, weapon 8 (rocketlauncher)
# Erwünschte Marker: `NeonWave: Rocket Launcher equipped`, `bo[a-z]+ somewhere`, `rumble` 
# Anti-Patterns: keine

exec tests/helpers/autostart_test.sh \
    --autostart \
    --startwave 10 \
    --weapon 8 \
    --expected 'NW_WEAPON_ROCKETLAUNCHER' \
    --pattern 'rocket' \
    --pattern 'Rocket Launcher' \
    "$@"
