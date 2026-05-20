# CMake toolchain file for cross-compiling to PlayStation 4 via the
# OpenOrbis PS4 Toolchain.
#
# Use:
#   cmake -S . -B build-ps4 -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-ps4.cmake
#   cmake --build build-ps4
#
# Requires:
#   - OpenOrbis PS4 Toolchain installed; OO_PS4_TOOLCHAIN env var set.
#     (release v0.5.4 — toolchain-llvm-18.tar.gz)
#   - System clang + ld.lld in PATH (Debian/Ubuntu: apt install clang lld).
#
# PS4 is x86-64 little-endian (AMD Jaguar) running a FreeBSD-derived
# userland. No endianness work needed — same arch family as the Windows
# reference build. The recompiled rom.c compiles unchanged.
#
# Backend strategy: reuse platform_sdl.cpp via OpenOrbis's bundled SDL2
# port (znullptr's port). The SDL2 backend already satisfies the
# platform_sdl.h contract.

set(CMAKE_SYSTEM_NAME       FreeBSD)         # PS4 userland is FreeBSD-derived
set(CMAKE_SYSTEM_PROCESSOR  x86_64)
set(CMAKE_CROSSCOMPILING    ON)

# --- Locate the OpenOrbis toolchain ----------------------------------------
if(NOT DEFINED OO_PS4_TOOLCHAIN)
    if(DEFINED ENV{OO_PS4_TOOLCHAIN})
        set(OO_PS4_TOOLCHAIN $ENV{OO_PS4_TOOLCHAIN})
    else()
        message(FATAL_ERROR
            "OO_PS4_TOOLCHAIN is not set. Install the OpenOrbis PS4 Toolchain "
            "and export OO_PS4_TOOLCHAIN to its directory.")
    endif()
endif()

if(NOT EXISTS "${OO_PS4_TOOLCHAIN}/link.x")
    message(FATAL_ERROR
        "OpenOrbis toolchain looks incomplete — ${OO_PS4_TOOLCHAIN}/link.x "
        "not found. Re-extract toolchain-llvm-18.tar.gz.")
endif()

# --- Compilers (system clang, PS4 FreeBSD target triple) -------------------
find_program(PS4_CLANG   NAMES clang   REQUIRED)
find_program(PS4_CLANGXX NAMES clang++ REQUIRED)
find_program(PS4_LLD     NAMES ld.lld  REQUIRED)

set(CMAKE_C_COMPILER   "${PS4_CLANG}")
set(CMAKE_CXX_COMPILER "${PS4_CLANGXX}")
set(CMAKE_ASM_COMPILER "${PS4_CLANG}")

# CMake's compiler check would try to link a host binary and fail — skip it.
set(CMAKE_C_COMPILER_WORKS   1)
set(CMAKE_CXX_COMPILER_WORKS 1)

set(PS4_TARGET "x86_64-pc-freebsd12-elf")

# --- Compile flags (from OpenOrbis sample Makefile) ------------------------
#   -fPIC          PS4 ELFs are position-independent
#   -funwind-tables required for the PS4 exception path
#   -isysroot      point clang's sysroot at the toolchain
#   -isystem       PS4 system headers
add_compile_options(
    --target=${PS4_TARGET}
    -fPIC
    -funwind-tables
    -isysroot "${OO_PS4_TOOLCHAIN}"
    -isystem  "${OO_PS4_TOOLCHAIN}/include"
)
# C++ also needs libc++ headers
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-isystem${OO_PS4_TOOLCHAIN}/include/c++/v1>)

include_directories(SYSTEM "${OO_PS4_TOOLCHAIN}/include")

# --- Link: ld.lld with the OpenOrbis link script ---------------------------
# CMake drives the link through the compiler by default; for OpenOrbis the
# link is done directly by ld.lld. Use ld.lld as the linker and pass the
# PS4 link recipe. crt1.o + the standard PS4 libs are appended.
set(CMAKE_C_LINK_EXECUTABLE
    "${PS4_LLD} <OBJECTS> -o <TARGET> <LINK_FLAGS> <LINK_LIBRARIES>")
set(CMAKE_CXX_LINK_EXECUTABLE "${CMAKE_C_LINK_EXECUTABLE}")

set(_ps4_ldflags
    "-m elf_x86_64 -pie --script ${OO_PS4_TOOLCHAIN}/link.x --eh-frame-hdr -L${OO_PS4_TOOLCHAIN}/lib")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_ps4_ldflags}")

# Standard PS4 libraries the LA build needs. SDL2 + the Sce* system modules.
# crt1.o must be present in the link; it is appended after the libs.
link_libraries(
    -lc -lkernel -lc++
    -lSceUserService -lSceVideoOut -lSceAudioOut -lScePad
    -lSceSysmodule -lSceFreeType
    -lSDL2
    "${OO_PS4_TOOLCHAIN}/lib/crt1.o"
)

# --- find_* scoping --------------------------------------------------------
set(CMAKE_FIND_ROOT_PATH "${OO_PS4_TOOLCHAIN}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Output is a plain ELF; create-fself / create-gp4 / PkgTool turn it into
# a .pkg afterwards (see cmake/test/build_ps4.sh).
set(CMAKE_EXECUTABLE_SUFFIX     ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_C   ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_CXX ".elf")

# --- Feature gates for the PS4 build ---------------------------------------
# Reuse platform_sdl.cpp via OpenOrbis's SDL2 port. ImGui is OFF: OpenOrbis's
# bundled SDL2 predates 2.0.17, so the ImGui SDL_Renderer backend
# (imgui_impl_sdlrenderer2, needs SDL_RenderGeometry) won't build. The ImGui
# calls inside platform_sdl.cpp are #ifdef LA_HAS_IMGUI gated, and the
# menu_gui/asset_viewer stubs are linked instead — same shape as libxenon.
#
# Multiplayer is OFF for the first build — ENet on PS4 sockets + SDL2 voice
# capture is a separate rabbit hole; get the game booting first.
set(LA_HAS_SDL2        ON  CACHE BOOL "PS4: use OpenOrbis's SDL2 port")
set(LA_HAS_IMGUI       OFF CACHE BOOL "PS4: OpenOrbis SDL2 too old for the ImGui renderer backend")
set(LA_MULTIPLAYER     OFF CACHE BOOL "PS4: no ENet/voice for the first build")

set(OO_PS4_TOOLCHAIN "${OO_PS4_TOOLCHAIN}" CACHE PATH "OpenOrbis toolchain root")
