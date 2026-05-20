#!/bin/bash
# How does the OpenOrbis SDL2 sample read the DualShock?
S="$HOME/PS4Toolchain/samples/SDL2/SDL2"
echo '=== SDL_Init / joystick / gamecontroller / pad usage ==='
grep -rnE 'SDL_Init|SDL_Joystick|SDL_GameController|SDL_NumJoysticks|JoystickOpen|GameControllerOpen|ScePad|scePad|SDL_CONTROLLERAXIS|SDL_JOYBUTTON|SDL_JOYAXIS' "$S" 2>/dev/null
echo
echo '=== does OpenOrbis SDL2 have a controller db / mapping? ==='
find "$HOME/PS4Toolchain" -iname '*gamecontroller*' -o -iname '*controllerdb*' 2>/dev/null | head
