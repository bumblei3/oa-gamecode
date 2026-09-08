#!/bin/sh
# Test 109: Controller CVars registration
# CVars: joy_deadzone, joy_sensitivity, in_joystick
# Erwünschte Marker: "NeonWave" (server started)
# Anti-Patterns: "error", "undefined"
#
# Verifies that controller CVars can be set without errors.
# Tests M13 Engine-Controller integration.
exec tests/helpers/autostart_test.sh \
    --autostart \
    --timeout 30 \
    --extra-args "+set joy_deadzone 0.15 +set joy_sensitivity 2.5 +set in_joystick 1" \
    --pattern "NeonWave" \
    "$@"
