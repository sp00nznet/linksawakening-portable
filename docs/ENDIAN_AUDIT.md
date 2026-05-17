# Phase 3 — Endianness + 32-bit audit

Hunt for little-endian-specific patterns that will break on big-endian PowerPC
(Xbox 360 Xenon, PS3 Cell PPE) and for 32-bit user-space assumptions that
matter on every non-x86-64 target.

## TL;DR

| Severity | Finding | Where | Fix scope |
| --- | --- | --- | --- |
| **Critical — runtime correctness** | CPU register unions hard-code LE layout | `gbrt.h:84-99` | ~16 lines, `#ifdef __BIG_ENDIAN__` swap |
| Cross-platform save compatibility | Save state writes host-endian multi-byte fields | `platform_sdl.cpp:808-868` | ~50 lines, standardize file format as LE |
| Cross-platform MP only | Network protocol uses host-endian multi-byte | `mp_protocol.h:216-233` | Defer until cross-platform MP is real |
| Cosmetic (debug-only) | BMP writer assumes host-LE | `asset_viewer.cpp:64-78` | `#ifdef` off on BE or add byteswap |
| **No issue** | `gb_read16`/`gb_write16` are byte-by-byte | `gbrt.c:752-759` | n/a — already correct |
| **No issue** | Core engine has zero reinterpret casts | `gbrt.c`, `ppu.c`, `audio.c`, `interpreter.c` | n/a |
| **No issue** | No 64-bit pointer math, no tagged pointers | n/a | n/a |

**The runtime is mostly endian-clean.** The one critical fix is the CPU
register union — without it, every register-pair access (`ctx->af`, `ctx->bc`,
etc.) would read the wrong bytes on BE.

## Critical finding #1 — CPU register unions

```c
// runtime/include/gbrt.h:84-99
union {
    struct { uint8_t f, a; };  /* AF register pair (little-endian) */
    uint16_t af;
};
union { struct { uint8_t c, b; }; uint16_t bc; };
union { struct { uint8_t e, d; }; uint16_t de; };
union { struct { uint8_t l, h; }; uint16_t hl; };
```

The struct field order (`f, a`) puts `f` at offset 0 and `a` at offset 1. On
a little-endian host, offset 0 is the low byte, so `af = (a << 8) | f` =
expected GB semantics. On a big-endian host, offset 0 is the **high** byte, so
`af = (f << 8) | a` — wrong.

The comment in the header literally calls this out as "little-endian." The
recompiled `rom.c` does both `ctx->a = x` and `ctx->af = y` style accesses
constantly, so this MUST be fixed before BE targets work.

### Fix

```c
union {
#if defined(__BIG_ENDIAN__) || \
    (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    struct { uint8_t a, f; };
#else
    struct { uint8_t f, a; };
#endif
    uint16_t af;
};
union {
#if defined(__BIG_ENDIAN__) || \
    (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    struct { uint8_t b, c; };
#else
    struct { uint8_t c, b; };
#endif
    uint16_t bc;
};
/* same for de, hl */
```

This is the **standard pattern in every cross-platform GB emulator** (SameBoy,
mGBA, BGB all do this). The upstream `gb-recompiled` should take this patch
gladly — it's the kind of cleanup that costs nothing on existing LE builds and
unlocks BE targets.

### Verification approach

After the fix, on a BE build (e.g., libxenon), the recompiled rom.c should
behave identically to the LE build. Test with a hardware-trace diff against
SameBoy reference output the same way the existing `tools/sb_tracer.c` does.

## Critical finding #2 — Save state file format

`platform_sdl.cpp:808-868` writes multi-byte register/state fields directly
via `fwrite(&ctx->af, sizeof(uint16_t), 1, f)`. The bytes hit disk in **host
byte order**.

Consequence: a save state written on Windows (LE) won't load correctly on
Xbox 360 (BE), and vice versa.

The multi-byte fields written:

| Field | Size | Source |
| --- | --- | --- |
| `magic`, `version` | 4×2 | header |
| `af`, `bc`, `de`, `hl`, `sp`, `pc`, `rom_bank`, `div_counter` | 2×8 | CPU regs |
| `speed_switch_halt`, `cycles`, `frame_cycles`, `last_sync_cycles`, `eram_sz` | 4×5 | counters |
| `dma.cycles_remaining` | 2 | inside DMA struct |
| Entire `GBPPU` struct | sizeof(GBPPU) | LCD regs, palette RAM, HDMA src/dst (uint16), mode_cycles (uint32), rgb_framebuffer (uint32[]) |

### Fix options

**Option A — Standardize file format as LE (recommended)**

Add `gb_save_u16_le(FILE* f, uint16_t v)` / `gb_save_u32_le(FILE* f, uint32_t v)`
helpers that always write low byte first, and the matching read helpers. Replace
every multi-byte `fwrite`/`fread` call in save/load.

For the GBPPU bulk write, either:
- Replace `fwrite(ctx->ppu, sizeof(GBPPU), 1, f)` with field-by-field saves
  using the helpers, OR
- Document that save states don't round-trip across endianness (acceptable for
  now since cross-host save sharing isn't a stated goal).

**Option B — Embed endianness marker + version bump**

Bump `SAVE_STATE_VERSION`, add an endianness byte to the header. On load, if
the byte doesn't match host endianness, byteswap every multi-byte field.
Slightly more code but keeps existing saves loadable.

**Recommended**: Option A for new fields, treat existing LE save state file as
the canonical format. PPU bulk write becomes field-by-field. Affects ~70 lines.

This is Phase 6 work — Phase 5 (libxenon backend) can ship without
cross-platform save state support and the 360 saves to its own `state.bin`.

## Cross-platform MP — `mp_protocol.h`

```c
// mp_protocol.h:216
memcpy(dst + out, &run, 2);    out += 2;     // run = uint16_t
memcpy(dst + out, &pixel, 4);  out += 4;     // pixel = uint32_t ARGB
```

RLE compression of the framebuffer for transmission to clients. Host-endian
copies. Means a Windows host can't serve a 360 client correctly (and vice
versa) — same-platform MP works fine.

**Defer.** Cross-platform MP isn't a stated goal. When the libxenon backend
exists with multiplayer disabled, this is moot.

## Cosmetic — BMP writer in `asset_viewer.cpp`

```c
// asset_viewer.cpp:64
memcpy(hdr + 2, &file_size, 4);   // BMP file_size field at offset 2
memcpy(hdr + 14, &dib_size, 4);   // DIB header size
```

BMP format spec says these are LE on disk. The `memcpy(&host_int, 4)` only
matches the spec when host is LE.

**Defer.** This is a debug feature (asset viewer "save BMP" button), not
relevant to a 360 build. When we add a BE backend, either `#ifdef` the
asset viewer code off (which we're doing anyway via `LA_HAS_IMGUI=OFF` on
libxenon) or add `gb_write32_le(uint8_t* dst, uint32_t v)` helpers.

## Non-issues

### `gb_read16` / `gb_write16` are explicitly LE

```c
// gbrt.c:752
uint16_t gb_read16(GBContext* ctx, uint16_t addr) {
    return (uint16_t)gb_read8(ctx, addr) | ((uint16_t)gb_read8(ctx, addr + 1) << 8);
}
void gb_write16(GBContext* ctx, uint16_t addr, uint16_t value) {
    gb_write8(ctx, addr, value & 0xFF);
    gb_write8(ctx, addr + 1, value >> 8);
}
```

Byte-by-byte assembly. The Game Boy is LE, so this is correct semantics on
any host. Since the recompiled `rom.c` uses these for every multi-byte GB
memory access, the recompiled game code is endian-portable by construction.

### Core engine has zero dangerous reinterpret casts

Greps for `*(uint16_t*)`, `*(uint32_t*)`, `*(int16_t*)` etc. across
`runtime/src/`:
- `gbrt.c`, `ppu.c`, `audio.c`, `interpreter.c`, `hwtrace.c`: **zero hits**
- `platform_sdl.cpp`: only SDL surface casts (host-endian by definition)
- `asset_viewer.cpp`: one `(uint32_t*)calloc(...)` allocation cast (safe)
- `mp_voice.cpp`: SDL audio stream casts (host-endian, AUDIO_S16SYS)

### No 64-bit pointer assumptions

- All sizes are `size_t` (correct on 32- and 64-bit).
- All opaque pointers stored as `void*` without tagging or arithmetic.
- No `uintptr_t & ~7` style tagged-pointer tricks.
- No `sizeof(void*) == 8` assumptions.

### No raw `union` packing tricks beyond the CPU register pairs

The only unions in the runtime source are the four register-pair unions
covered above. ImGui has internal unions but they're for `int/float/void*`
type punning, not endian-sensitive.

### Audio reinterpret is endian-safe by SDL convention

```c
int16_t* output = (int16_t*)stream;     // platform_sdl.cpp:218
```

SDL gives us samples in `AUDIO_S16SYS` format which is **defined** as host
endianness. So reinterpreting `Uint8*` as `int16_t*` on a host that requested
S16SYS is correct on any host.

### PPU framebuffer storage is safe in memory

`GBPPU.framebuffer[GB_FRAMEBUFFER_SIZE]` is `uint8_t[]` (2-bit indices, 1
byte per pixel) and `GBPPU.rgb_framebuffer[]` is `uint32_t[]` (host-endian
ARGB). Both consumed in-process by `gb_platform_render_frame(const uint32_t*)`
where the platform layer is expected to interpret pixels in host format
(SDL_PIXELFORMAT_ARGB8888 maps to this on every host). No cross-host
serialization of framebuffers.

### ENet is already endian-aware

`runtime/third_party/enet/include/enet/{win32,unix}.h` defines
`ENET_HOST_TO_NET_16/32` via `htons`/`htonl`. The library wraps all wire
fields in these macros. ENet itself is BE-safe on libxenon (devkitPPC
provides `htons`/`htonl` via lwIP).

### `memcpy` of 2/4 bytes outside the above contexts

The remaining hits are all internal-state copies that round-trip on the same
host: ImGui's internal serialization, ENet's per-packet header munging
(already wrapped), and the multiplayer RLE compression already noted above.

## 32-bit user-space audit

| Target | Bitness | Concerns |
| --- | --- | --- |
| Windows x86-64 | 64-bit | n/a (current build) |
| Xbox 360 (libxenon) | **32-bit** | `sizeof(void*) == 4`, `sizeof(size_t) == 4` |
| PS3 (PSL1GHT) | **32-bit** | same |
| OG Xbox (NXDK) | **32-bit** | same |
| Dreamcast (KOS) | **32-bit** | same, plus 16 MB total RAM is the tightest squeeze |
| WASM | **32-bit** | same |
| Android arm64 | 64-bit | armv7 variant is 32-bit |

### Findings

**No code is broken by 32-bit user space.** All pointer storage is `void*`,
all sizes are `size_t`, the runtime allocates with `calloc(count, size)`
and `malloc(size)` — both honor whatever `size_t` is.

**One soft concern**: `printf("%lu", some_size_t_value)` would truncate on a
32-bit host where `long` is 32 bits and `size_t` is 32 bits, but `%lu` for a
`size_t` value greater than 4 GB would also already be broken — and we don't
push values that large through any logging. The runtime uses `%u` and `%d`
mostly. Spot check: no obvious offenders.

**One real concern** — `sizeof(GBContext)` differs between 32 and 64-bit hosts
because it contains 9 pointer fields. The save state code writes individual
scalar fields (not the struct), so it doesn't break. But `fwrite(ctx->ppu,
sizeof(GBPPU), 1, f)` writes the GBPPU which has **no pointers** — just
scalars and arrays — so its size is the same on 32 and 64-bit hosts. Save
state is size-portable across bitness. (It's still byte-order non-portable —
see save state finding above.)

### Dreamcast RAM squeeze (informational)

The Dreamcast has 16 MB of main RAM. Currently:

- `rom.c` compiles to ~25 MB code (executable size). The DC linker has to
  place this in cartridge ROM-equivalent (CD-ROM, ~700 MB), not in main RAM.
  KOS handles this naturally; no issue.
- `rom_rom.c` is ~6 MB of ROM bytes as a C array — must live in RAM at
  runtime since we `gb_read8(ctx->rom, addr)`. Eats 6 MB of the 16 MB budget.
- Runtime + framebuffers + GB-emulated RAM (32 KB WRAM + 16 KB VRAM + OAM +
  HRAM) + APU buffers: ~1 MB.

That leaves ~9 MB for KOS, the C library, and the actual recompiled code's
runtime heap. Tight but feasible. **Real DC port concern, not a 360 concern.**

## Recommended fix order

Phase 5a (the existing pre-360 patch phase) should add these:

1. **CPU register union BE swap** in `gbrt.h:84-99` (~16 lines). Required.
2. **Settings PAL extension** (`gb_platform_fs_read/write`) from Phase 6 PAL
   audit — fold in here. Required for save battery RAM on libxenon (FATFS
   path).

Phase 5 (the libxenon backend itself) gets these automatically once the above
land — no extra endianness work needed in `platform_libxenon.cpp`.

Phase 6+ (deferred):

3. Standardize save state file format as LE (~70 lines in platform_sdl.cpp).
4. BMP writer endian-safety (or just `#ifdef` it off when `LA_HAS_IMGUI=OFF`).
5. MP protocol endian-safety (only if cross-platform MP becomes a goal).

## What changes in the project plan

Phase 3 deliverable: this document, plus the **one critical patch** needed
upstream in `gb-recompiled` to make BE targets viable at all (the AF/BC/DE/HL
union swap). That patch lands as part of Phase 5a along with the
`LA_MULTIPLAYER` / `LA_HAS_IMGUI` gates.

The rest of the endian work is either deferred (save state cross-platform
support, MP cross-platform) or absent-by-coincidence (`gb_read16` is already
correct, asset viewer is stubbed off on libxenon anyway).
