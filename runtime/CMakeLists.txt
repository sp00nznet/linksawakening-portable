# runtime/CMakeLists.txt
# GameBoy Runtime Library

# --- Per-target feature gates ----------------------------------------------
# Defaults: enabled when SDL2 is found on the host, disabled otherwise.
# Cross-compile toolchains (libxenon, etc.) should set both to OFF explicitly.
find_package(SDL2 QUIET)
option(LA_HAS_IMGUI       "Build ImGui menu + asset viewer (requires SDL2 renderer)" ${SDL2_FOUND})
option(LA_HAS_MULTIPLAYER "Build ENet + multiplayer overlay (sp00nznet/la-mp)"        ${SDL2_FOUND})

# --- Core sources (always built) -------------------------------------------
add_library(gbrt STATIC
    src/gbrt.c
    src/interpreter.c
    src/ppu.c
    src/audio.c
    src/hwtrace.c
    src/platform_sdl.cpp
)

# --- Menu / asset viewer: real impl when ImGui is on, stubs when off -------
if(LA_HAS_IMGUI)
    target_sources(gbrt PRIVATE
        src/menu_gui.cpp
        src/asset_viewer.cpp
        third_party/imgui/imgui.cpp
        third_party/imgui/imgui_draw.cpp
        third_party/imgui/imgui_tables.cpp
        third_party/imgui/imgui_widgets.cpp
        third_party/imgui/imgui_demo.cpp
        third_party/imgui/backends/imgui_impl_sdl2.cpp
        third_party/imgui/backends/imgui_impl_sdlrenderer2.cpp
    )
    target_include_directories(gbrt PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/backends
    )
    target_compile_definitions(gbrt PUBLIC LA_HAS_IMGUI)
else()
    target_sources(gbrt PRIVATE
        src/menu_gui_stubs.c
        src/asset_viewer_stubs.c
    )
endif()

# --- Multiplayer overlay (la-mp sources + ENet) ----------------------------
if(LA_HAS_MULTIPLAYER)
    target_sources(gbrt PRIVATE
        src/multiplayer/mp_net.cpp
        src/multiplayer/mp_session.cpp
        src/multiplayer/mp_menu.cpp
        src/multiplayer/mp_color.cpp
        src/multiplayer/mp_worldsync.cpp
        src/multiplayer/mp_indicators.cpp
        src/multiplayer/mp_delta.cpp
        src/multiplayer/mp_trade.cpp
        src/multiplayer/mp_voice.cpp
        src/multiplayer/mp_pvp.cpp
        src/multiplayer/mp_sprites.cpp
    )
    target_compile_definitions(gbrt PUBLIC LA_HAS_MULTIPLAYER)
    add_subdirectory(third_party/enet)
    target_link_libraries(gbrt PUBLIC enet)
    if(WIN32)
        target_link_libraries(gbrt PUBLIC ws2_32 winmm)
    endif()
endif()

target_include_directories(gbrt PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/multiplayer
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/enet/include
)

target_compile_features(gbrt PUBLIC c_std_11 cxx_std_20)

# --- Debug logging knobs ---------------------------------------------------
option(GB_DEBUG       "Enable debug logging"           OFF)
option(GB_DEBUG_VRAM  "Enable VRAM debug logging"      OFF)
option(GB_DEBUG_FRAME "Enable frame debug logging"     OFF)
option(GB_DEBUG_REGS  "Enable register debug logging"  OFF)
option(GB_DEBUG_PPU   "Enable PPU debug logging"       OFF)

if(GB_DEBUG)
    target_compile_definitions(gbrt PUBLIC GB_DEBUG)
    message(STATUS "GameBoy debug logging enabled")
endif()
if(GB_DEBUG_VRAM)  target_compile_definitions(gbrt PUBLIC GB_DEBUG_VRAM) endif()
if(GB_DEBUG_FRAME) target_compile_definitions(gbrt PUBLIC GB_DEBUG_FRAME) endif()
if(GB_DEBUG_REGS)  target_compile_definitions(gbrt PUBLIC GB_DEBUG_REGS) endif()
if(GB_DEBUG_PPU)   target_compile_definitions(gbrt PUBLIC GB_DEBUG_PPU) endif()

# --- Host platform identifier ----------------------------------------------
if(APPLE)
    target_compile_definitions(gbrt PRIVATE GB_PLATFORM_MACOS)
elseif(WIN32)
    target_compile_definitions(gbrt PRIVATE GB_PLATFORM_WINDOWS)
else()
    target_compile_definitions(gbrt PRIVATE GB_PLATFORM_LINUX)
endif()

# --- SDL2 (when present) ---------------------------------------------------
if(SDL2_FOUND)
    target_link_libraries(gbrt PUBLIC SDL2::SDL2)
    target_compile_definitions(gbrt PUBLIC GB_HAS_SDL2)
endif()
