cmake_minimum_required(VERSION 3.16)
project(rom C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-O3)
endif()

# Per-target backend gates. Defaults match the Windows reference build (matches
# the upstream la-dx-recompiled binary the user runs). The libxenon/wasm/etc.
# toolchain files will flip LA_MULTIPLAYER off where the network stack isn't
# realistic to port.
option(LA_MULTIPLAYER "Build with ENet + multiplayer overlay (mp_*.cpp)" ON)

# Runtime lives in-tree (vendored from sp00nznet/gb-recompiled).
set(GBRT_DIR "${CMAKE_SOURCE_DIR}/runtime")

# SDL2 backend is currently the only platform implementation. The libxenon /
# wasm / android backends will replace platform_sdl.cpp with a sibling
# platform_<target>.cpp that satisfies the same runtime/include/platform_sdl.h
# contract (gb_platform_init / poll_events / render_frame / vsync / ...).
find_package(SDL2 REQUIRED)

# --- Runtime library (gbrt) -------------------------------------------------
add_library(gbrt STATIC
    ${GBRT_DIR}/src/gbrt.c
    ${GBRT_DIR}/src/ppu.c
    ${GBRT_DIR}/src/audio.c
    ${GBRT_DIR}/src/interpreter.c
    ${GBRT_DIR}/src/hwtrace.c
    ${GBRT_DIR}/src/platform_sdl.cpp
    ${GBRT_DIR}/src/menu_gui.cpp
    ${GBRT_DIR}/src/asset_viewer.cpp
)

# ImGui is part of the base runtime (menu_gui.cpp uses it unconditionally).
target_sources(gbrt PRIVATE
    ${GBRT_DIR}/third_party/imgui/imgui.cpp
    ${GBRT_DIR}/third_party/imgui/imgui_draw.cpp
    ${GBRT_DIR}/third_party/imgui/imgui_tables.cpp
    ${GBRT_DIR}/third_party/imgui/imgui_widgets.cpp
    ${GBRT_DIR}/third_party/imgui/imgui_demo.cpp
    ${GBRT_DIR}/third_party/imgui/backends/imgui_impl_sdl2.cpp
    ${GBRT_DIR}/third_party/imgui/backends/imgui_impl_sdlrenderer2.cpp
)

if(LA_MULTIPLAYER)
    target_sources(gbrt PRIVATE
        ${GBRT_DIR}/third_party/enet/callbacks.c
        ${GBRT_DIR}/third_party/enet/compress.c
        ${GBRT_DIR}/third_party/enet/host.c
        ${GBRT_DIR}/third_party/enet/list.c
        ${GBRT_DIR}/third_party/enet/packet.c
        ${GBRT_DIR}/third_party/enet/peer.c
        ${GBRT_DIR}/third_party/enet/protocol.c
        ${GBRT_DIR}/third_party/enet/win32.c

        ${GBRT_DIR}/src/multiplayer/mp_net.cpp
        ${GBRT_DIR}/src/multiplayer/mp_session.cpp
        ${GBRT_DIR}/src/multiplayer/mp_menu.cpp
        ${GBRT_DIR}/src/multiplayer/mp_color.cpp
        ${GBRT_DIR}/src/multiplayer/mp_worldsync.cpp
        ${GBRT_DIR}/src/multiplayer/mp_indicators.cpp
        ${GBRT_DIR}/src/multiplayer/mp_delta.cpp
        ${GBRT_DIR}/src/multiplayer/mp_trade.cpp
        ${GBRT_DIR}/src/multiplayer/mp_voice.cpp
        ${GBRT_DIR}/src/multiplayer/mp_pvp.cpp
        ${GBRT_DIR}/src/multiplayer/mp_sprites.cpp
    )
    target_compile_definitions(gbrt PUBLIC LA_HAS_MULTIPLAYER)
endif()

target_include_directories(gbrt PUBLIC
    ${GBRT_DIR}/include
    ${GBRT_DIR}/third_party/imgui
    ${GBRT_DIR}/third_party/enet/include
    ${GBRT_DIR}/src
)

target_link_libraries(gbrt PUBLIC SDL2::SDL2)
if(WIN32 AND LA_MULTIPLAYER)
    target_link_libraries(gbrt PUBLIC ws2_32 winmm)
endif()
target_compile_definitions(gbrt PUBLIC GB_HAS_SDL2)

# --- Main executable --------------------------------------------------------
# rom.c and rom_rom.c are recompiler output (gitignored); user regenerates
# them locally from their ROM via gb-recompiled.
add_executable(rom
    rom_main.c
    rom.c
    rom_rom.c
)

target_link_libraries(rom gbrt)
