cmake_minimum_required(VERSION 3.6)
project(LINKSAWAKENING)

# Game sources come from the LinksAwakening submodule at the repo root:
# rom_main.c, the runtime (gbrecomp/runtime) and gen/ (generated locally
# from your ROM, never committed).
set(LA "${CMAKE_CURRENT_SOURCE_DIR}/../../../../LinksAwakening")
set(RT "${LA}/gbrecomp/runtime")
set(GEN "${LA}/gen")

add_library(main SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/android_main.c
    ${GEN}/rom.c
    ${GEN}/rom_rom.c
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
    ${GEN}
    ${RT}/include
    ${RT}/src
)

# GB_HAS_SDL2 selects the real game loop in rom_main.c and the SDL2 backend
# in platform_sdl.cpp. ImGui + multiplayer stay off (plain-C stubs), same as
# the PS4 build. __ANDROID__ (auto-defined by the NDK) enables touch controls.
target_compile_definitions(main PRIVATE GB_HAS_SDL2)
target_compile_options(main PRIVATE -w)

target_link_libraries(main SDL2)
