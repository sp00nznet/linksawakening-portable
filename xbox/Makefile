# NXDK build for Link's Awakening — Original Xbox.
#
# Reuses the SDL2 backend (runtime/src/platform_sdl.cpp) through NXDK's
# bundled SDL2 port — the same approach as PS4 / Android / WebAssembly.
# x86 little-endian: the recompiled rom.c compiles unchanged.
#
# Build:  bash cmake/test/build_xbox.sh
# Needs:  NXDK at $NXDK_DIR (default C:/Users/nedch/nxdk), clang + lld + make.

XBE_TITLE = LinksAwakening
GEN_XISO  = $(XBE_TITLE).iso

NXDK_DIR ?= $(HOME)/nxdk
NXDK_SDL  = y
NXDK_CXX  = y

LA = $(CURDIR)/..
RT = $(LA)/runtime

SRCS = \
	$(LA)/rom.c \
	$(LA)/rom_rom.c \
	$(LA)/rom_main.c \
	$(RT)/src/gbrt.c \
	$(RT)/src/ppu.c \
	$(RT)/src/audio.c \
	$(RT)/src/interpreter.c \
	$(RT)/src/hwtrace.c \
	$(RT)/src/menu_gui_stubs.c \
	$(RT)/src/asset_viewer_stubs.c \
	$(RT)/src/platform_sdl.cpp

# GB_HAS_SDL2 selects the real game loop in rom_main.c + the SDL2 backend.
# GB_RECOMPILED_DISPATCH drops gbrt.c's weak gb_dispatch fallback — rom.c
# provides the real one, and NXDK's COFF lld rejects the duplicate.
# ImGui + multiplayer stay off (plain-C stubs), same as the PS4 build.
# -Os: the Xbox has only 64 MB RAM and the .xbe image is loaded whole —
# optimize the ~105 MB recompiled rom.c for size to keep the image small.
CFLAGS   += -Os -DGB_HAS_SDL2 -DGB_RECOMPILED_DISPATCH -I$(LA) -I$(RT)/include -I$(RT)/src -w
CXXFLAGS += -Os -DGB_HAS_SDL2 -DGB_RECOMPILED_DISPATCH -I$(LA) -I$(RT)/include -I$(RT)/src -w

include $(NXDK_DIR)/Makefile
