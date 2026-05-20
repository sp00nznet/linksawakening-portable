cmake_minimum_required(VERSION 3.6)
project(LINKSAWAKENING)

# The recompiled game + vendored runtime live in the linksawakening-portable
# repo. rom.c / rom.h / rom_rom.c are generated locally and gitignored.
set(LA "D:/ports/la360")
set(RT "${LA}/runtime")

add_library(main SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/android_main.c
    ${LA}/rom.c
    ${LA}/rom_rom.c
    ${LA}/rom_main.c
    ${RT}/src/gbrt.c
    ${RT}/src/ppu.c
    ${RT}/src/audio.c
    ${RT}/src/interpreter.c
    ${RT}/src/hwtrace.c
    ${RT}/src/menu_gui_stubs.c
    ${RT}/src/asset_viewer_stubs.c
    ${RT}/src/platform_sdl.cpp
)

target_include_directories(main PRIVATE
    ${LA}
    ${RT}/include
    ${RT}/src
)

# GB_HAS_SDL2 selects the real game loop in rom_main.c and the SDL2 backend
# in platform_sdl.cpp. ImGui + multiplayer stay off (plain-C stubs), same as
# the PS4 build. __ANDROID__ (auto-defined by the NDK) enables touch controls.
target_compile_definitions(main PRIVATE GB_HAS_SDL2)
target_compile_options(main PRIVATE -w)

target_link_libraries(main SDL2)
