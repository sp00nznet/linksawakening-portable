# CMake toolchain file for cross-compiling to Xbox 360 via libxenon.
#
# Use:
#   cmake -S . -B build-xenon -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-libxenon.cmake
#   cmake --build build-xenon
#
# Requires: libxenon installed at /usr/local/xenon/ (or set DEVKITXENON env).
# In WSL: install via ~/libxenon/toolchain/build-xenon-toolchain {toolchain,libs}.
# This must run from a Linux env (WSL works) — xenon-gcc isn't a Windows binary.

set(CMAKE_SYSTEM_NAME       Generic)        # bare-metal, no host OS
set(CMAKE_SYSTEM_PROCESSOR  powerpc)
set(CMAKE_CROSSCOMPILING    ON)

# Where libxenon lives. Default matches setup.sh from xbox360nfs.
if(NOT DEFINED DEVKITXENON)
    if(DEFINED ENV{DEVKITXENON})
        set(DEVKITXENON $ENV{DEVKITXENON})
    else()
        set(DEVKITXENON "/usr/local/xenon")
    endif()
endif()

if(NOT EXISTS "${DEVKITXENON}/bin/xenon-gcc")
    message(FATAL_ERROR
        "xenon-gcc not found at ${DEVKITXENON}/bin/xenon-gcc.\n"
        "Build libxenon first:\n"
        "  cd ~/libxenon/toolchain && ./build-xenon-toolchain toolchain && ./build-xenon-toolchain libs")
endif()

# Cross compilers (all use xenon- prefix per devkitxenon/rules).
set(CMAKE_C_COMPILER   "${DEVKITXENON}/bin/xenon-gcc")
set(CMAKE_CXX_COMPILER "${DEVKITXENON}/bin/xenon-g++")
set(CMAKE_AR           "${DEVKITXENON}/bin/xenon-ar"      CACHE FILEPATH "")
set(CMAKE_RANLIB       "${DEVKITXENON}/bin/xenon-ranlib"  CACHE FILEPATH "")
set(CMAKE_OBJCOPY      "${DEVKITXENON}/bin/xenon-objcopy" CACHE FILEPATH "")
set(CMAKE_STRIP        "${DEVKITXENON}/bin/xenon-strip"   CACHE FILEPATH "")
set(CMAKE_ASM_COMPILER "${DEVKITXENON}/bin/xenon-gcc"     CACHE FILEPATH "")

# Don't run a test-compile that links — the linker needs flags we set below
# and CMake's default compiler check would fail otherwise.
set(CMAKE_C_COMPILER_WORKS   1)
set(CMAKE_CXX_COMPILER_WORKS 1)

# MACHDEP from libxenon's devkitxenon/rules:
#   -DXENON   = identify the platform
#   -m32      = 32-bit user space (Xenon kernel runs apps in 32-bit)
#   -maltivec = enable AltiVec/VMX vector instructions
#   -fno-pic  = no position-independent code (Xenon doesn't use it)
#   -mpowerpc64 = PowerPC 64-bit ISA (CPU is 64-bit even though apps are 32-bit)
#   -mhard-float = use hardware FPU
set(LIBXENON_MACHDEP "-DXENON -m32 -maltivec -fno-pic -mpowerpc64 -mhard-float")

# Compile flags: MACHDEP + libxenon include + the GCC 32-bit lib path
# CMake propagates these to all targets via add_compile_options below.
add_compile_options(-DXENON -m32 -maltivec -fno-pic -mpowerpc64 -mhard-float)
add_compile_options(-Wall -Wno-unused-function -Wno-unused-variable)

include_directories(SYSTEM
    "${DEVKITXENON}/usr/include"
    "${DEVKITXENON}/usr/include/lwip"
)

# Linker: must use the libxenon app.lds linker script (sets ENTRY(_start),
# .text at 0x80000000, heap layout). MACHDEP must also be passed at link time
# so the right libgcc + crt files are pulled in.
add_link_options(
    -m32 -maltivec -fno-pic -mpowerpc64 -mhard-float
    "-L${DEVKITXENON}/xenon/lib/32"
    "-L${DEVKITXENON}/usr/lib"
    -Wl,--gc-sections
    "-T${DEVKITXENON}/app.lds"
)

# Standard libxenon link line. Order matters: -lxenon must come before -lm /
# -lc / -lgcc since it forward-references things in them.
link_libraries(-lxenon -lm -lc -lgcc)

# Restrict find_* to the sysroot — host /usr/include shouldn't be searched.
set(CMAKE_FIND_ROOT_PATH "${DEVKITXENON}" "${DEVKITXENON}/usr")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Output is *.elf32, not *.exe — strip the EXE extension CMake assumes.
set(CMAKE_EXECUTABLE_SUFFIX      ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_C    ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_CXX  ".elf")

# Target-side capability gates inferred from PAL/Endian audits — these are
# the things the libxenon backend simply can't do without huge upstream work.
# Override on the command line with -DLA_MULTIPLAYER=ON etc. if you want.
set(LA_MULTIPLAYER OFF CACHE BOOL "MP requires SDL2 voice capture + Winsock — not viable on libxenon")
set(LA_HAS_IMGUI   OFF CACHE BOOL "ImGui requires SDL renderer — no SDL on libxenon")
set(LA_HAS_SDL2    OFF CACHE BOOL "No SDL2 on libxenon target")

# Expose DEVKITXENON to CMakeLists.txt so it can find app.lds + elf2xex if
# the post-build XEX-conversion rule is wired up there.
set(DEVKITXENON "${DEVKITXENON}" CACHE PATH "libxenon install prefix")
set(LIBXENON_INCLUDE_DIR "${DEVKITXENON}/usr/include" CACHE PATH "")
set(LIBXENON_LIB_DIR     "${DEVKITXENON}/usr/lib"     CACHE PATH "")
set(LIBXENON_LD_SCRIPT   "${DEVKITXENON}/app.lds"     CACHE FILEPATH "")
