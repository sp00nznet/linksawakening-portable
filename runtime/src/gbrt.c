#include "gbrt.h"
#include "ppu.h"
#include "audio.h"
#include "platform_sdl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gbrt_debug.h"

/* ============================================================================
 * Definitions
 * ========================================================================== */

#define WRAM_BANK_SIZE 0x1000
#define VRAM_SIZE      0x2000
#define OAM_SIZE       0xA0
#define IO_SIZE        0x80
#define HRAM_SIZE      0x7F

/* ============================================================================
 * Globals
 * ========================================================================== */

bool gbrt_trace_enabled = false;
uint64_t gbrt_instruction_count = 0;
uint64_t gbrt_instruction_limit = 0;

static char* gbrt_trace_filename = NULL;


/* ============================================================================
 * Context Management
 * ========================================================================== */

GBContext* gb_context_create(const GBConfig* config) {
    GBContext* ctx = (GBContext*)calloc(1, sizeof(GBContext));
    if (!ctx) return NULL;
    
    ctx->wram = (uint8_t*)calloc(1, WRAM_BANK_SIZE * 8);
    ctx->vram = (uint8_t*)calloc(1, VRAM_SIZE * 2);
    ctx->oam = (uint8_t*)calloc(1, OAM_SIZE);
    ctx->hram = (uint8_t*)calloc(1, HRAM_SIZE);
    ctx->io = (uint8_t*)calloc(1, IO_SIZE + 1);
    
    if (!ctx->wram || !ctx->vram || !ctx->oam || !ctx->hram || !ctx->io) {
        gb_context_destroy(ctx);
        return NULL;
    }
    
    GBPPU* ppu = (GBPPU*)calloc(1, sizeof(GBPPU));
    if (ppu) {
        ppu_init(ppu);
        ctx->ppu = ppu;
    }
    
    ctx->apu = gb_audio_create();
    gb_context_reset(ctx, true);
    (void)config;

    if (gbrt_trace_filename) {
        ctx->trace_file = fopen(gbrt_trace_filename, "w");
        if (ctx->trace_file) {
            ctx->trace_entries_enabled = true;
            fprintf(stderr, "[GBRT] Tracing entry points to %s\n", gbrt_trace_filename);
        }
    }

    return ctx;
}

void gb_context_destroy(GBContext* ctx) {
    if (!ctx) return;
    
    /* Save RAM before destroying if available */
    if (ctx->eram && ctx->ram_enabled && ctx->callbacks.save_battery_ram) {
        gb_context_save_ram(ctx);
    }
    
    if (ctx->trace_file) fclose((FILE*)ctx->trace_file);
    free(ctx->wram);
    free(ctx->vram);
    free(ctx->oam);
    free(ctx->hram);
    free(ctx->io);
    
    if (ctx->eram) free(ctx->eram);
    
    if (ctx->ppu) free(ctx->ppu);
    if (ctx->apu) gb_audio_destroy(ctx->apu);
    if (ctx->rom) free(ctx->rom);
    free(ctx);
}

void gb_context_reset(GBContext* ctx, bool skip_bootrom) {
    /* Reset DMA state */
    ctx->dma.active = 0;
    ctx->dma.source_high = 0;
    ctx->dma.progress = 0;
    ctx->dma.cycles_remaining = 0;
    
    /* Reset HALT bug state */
    ctx->halt_bug = 0;
    
    /* Reset interrupt state */
    ctx->ime = 0;
    ctx->ime_pending = 0;
    ctx->halted = 0;
    ctx->stopped = 0;
    
    /* Reset APU state */
    if (ctx->apu) {
        gb_audio_reset(ctx->apu);
    }

    /* Reset RTC state */
    ctx->rtc.s = 0;
    ctx->rtc.m = 0;
    ctx->rtc.h = 0;
    ctx->rtc.dl = 0;
    ctx->rtc.dh = 0;
    ctx->rtc.latched_s = 0;
    ctx->rtc.latched_m = 0;
    ctx->rtc.latched_h = 0;
    ctx->rtc.latched_dl = 0;
    ctx->rtc.latched_dh = 0;
    ctx->rtc.latch_state = 0;
    ctx->rtc.last_time = 0;
    ctx->rtc.active = true;  /* RTC oscillator active by default */
    
    /* Reset MBC state */
    ctx->rtc_mode = 0;
    ctx->rtc_reg = 0;
    ctx->ram_enabled = 0;
    ctx->mbc_mode = 0;
    ctx->rom_bank_upper = 0;
    
    if (skip_bootrom) {
        ctx->pc = 0x0100;
        ctx->sp = 0xFFFE;
        ctx->af = 0x1180;  /* A=0x11 for CGB mode, F=0x80 (Z flag only, N/H/C clear) */
        ctx->bc = 0x0000;
        ctx->de = 0xFF56;
        ctx->hl = 0x000D;
        gb_unpack_flags(ctx);
        ctx->rom_bank = 1;
        ctx->wram_bank = 1;

        /* DIV counter - match SameBoy's post-bootrom init value of 8.
         * SameBoy calls GB_set_internal_div_counter(gb, 8) after init.
         * The speed switch STOP instruction will reset DIV to 0. */
        ctx->div_counter = 8;
        ctx->io[0x04] = (uint8_t)(ctx->div_counter >> 8);

        ctx->io[0x05] = 0x00; /* TIMA */
        ctx->io[0x06] = 0x00; /* TMA */
        ctx->io[0x07] = 0x00; /* TAC */
        ctx->io[0x10] = 0x80; /* NR10 */
        ctx->io[0x11] = 0xBF; /* NR11 */
        ctx->io[0x12] = 0xF3; /* NR12 */
        ctx->io[0x14] = 0xBF; /* NR14 */
        ctx->io[0x16] = 0x3F; /* NR21 */
        ctx->io[0x17] = 0x00; /* NR22 */
        ctx->io[0x19] = 0xBF; /* NR24 */
        ctx->io[0x1A] = 0x7F; /* NR30 */
        ctx->io[0x1B] = 0xFF; /* NR31 */
        ctx->io[0x1C] = 0x9F; /* NR32 */
        ctx->io[0x1E] = 0xBF; /* NR34 */
        ctx->io[0x20] = 0xFF; /* NR41 */
        ctx->io[0x21] = 0x00; /* NR42 */
        ctx->io[0x22] = 0x00; /* NR43 */
        ctx->io[0x23] = 0xBF; /* NR44 */
        ctx->io[0x24] = 0x77; /* NR50 */
        ctx->io[0x25] = 0xF3; /* NR51 */
        ctx->io[0x26] = 0xF1; /* NR52 */
        ctx->io[0x40] = 0x91; /* LCDC */
        ctx->io[0x42] = 0x00; /* SCY */
        ctx->io[0x43] = 0x00; /* SCX */
        ctx->io[0x45] = 0x00; /* LYC */
        ctx->io[0x47] = 0xFC; /* BGP */
        ctx->io[0x48] = 0x00; /* OBP0 - CGB post-bootrom value is 0x00 */
        ctx->io[0x49] = 0x00; /* OBP1 - CGB post-bootrom value is 0x00 */
        ctx->io[0x4A] = 0x00; /* WY */
        ctx->io[0x4B] = 0x00; /* WX */
        ctx->io[0x0F] = 0x01; /* IF - VBlank interrupt pending (post-bootrom) */
        ctx->io[0x80] = 0x00; /* IE - game sets this itself */
        ctx->ime = 0;         /* IME - game enables via EI instruction */
    }
}

bool gb_context_load_rom(GBContext* ctx, const uint8_t* data, size_t size) {
    if (ctx->rom) free(ctx->rom);
    ctx->rom = (uint8_t*)malloc(size);
    if (!ctx->rom) return false;
    memcpy(ctx->rom, data, size);
    ctx->rom_size = size;
    
    /* Parse Header for MBC type, RAM, and Battery info */
    if (size > 0x149) {
        uint8_t type = ctx->rom[0x147];
        uint8_t ram_size_code = ctx->rom[0x149];
        ctx->mbc_type = type;  /* Store cart type for MBC bank switching */
        
        /* Check if battery is present */
        bool has_battery = false;
        switch (type) {
            case 0x03: /* MBC1+RAM+BATTERY */
            case 0x06: /* MBC2+BATTERY */
            case 0x09: /* ROM+RAM+BATTERY */
            case 0x0D: /* MMM01+RAM+BATTERY */
            case 0x0F: /* MBC3+TIMER+BATTERY */
            case 0x10: /* MBC3+TIMER+RAM+BATTERY */
            case 0x13: /* MBC3+RAM+BATTERY */
            case 0x1B: /* MBC5+RAM+BATTERY */
            case 0x1E: /* MBC5+RUMBLE+RAM+BATTERY */
            case 0x22: /* MBC7+SENSOR+RUMBLE+RAM+BATTERY */
            case 0xFF: /* HuC1+RAM+BATTERY */
                has_battery = true;
                break;
        }
        
        /* Calculate RAM size */
        size_t ram_bytes = 0;
        
        /* MBC2 has fixed 512x4 bits (256 bytes effective, usually 512 allocated) */
        if (type == 0x05 || type == 0x06) {
            ram_bytes = 512;
            ram_size_code = 0; /* Override */
        } else {
            switch (ram_size_code) {
                case 0x00: ram_bytes = 0; break;
                case 0x01: ram_bytes = 2 * 1024; break; /* 2KB */
                case 0x02: ram_bytes = 8 * 1024; break; /* 8KB */
                case 0x03: ram_bytes = 32 * 1024; break; /* 32KB (4 banks) */
                case 0x04: ram_bytes = 128 * 1024; break; /* 128KB (16 banks) */
                case 0x05: ram_bytes = 64 * 1024; break; /* 64KB (8 banks) */
                default: ram_bytes = 0; break;
            }
        }
        
        /* Allocate RAM */
        if (ctx->eram) free(ctx->eram);
        ctx->eram = NULL;
        ctx->eram_size = 0;
        
        if (ram_bytes > 0) {
            ctx->eram = (uint8_t*)calloc(1, ram_bytes);
            if (ctx->eram) {
                ctx->eram_size = ram_bytes;
                printf("[GBRT] Allocated %zu bytes for External RAM\n", ram_bytes);
                
                /* Load Save Data if Battery Present */
                if (has_battery && ctx->callbacks.load_battery_ram) {
                    /* Get ROM title for filename */
                    char title[17] = {0};
                    memcpy(title, &ctx->rom[0x134], 16);
                    /* Sanitize title */
                    for(int i=0; i<16; i++) {
                        if(title[i] == 0 || title[i] < 32 || title[i] > 126) title[i] = 0;
                    }
                    if(title[0] == 0) strcpy(title, "UNKNOWN_GAME");
                    
                    if (ctx->callbacks.load_battery_ram(ctx, title, ctx->eram, ctx->eram_size)) {
                         printf("[GBRT] Loaded battery RAM for '%s'\n", title);
                    }
                }
            }
        }
    }
    
    return true;
}

bool gb_context_save_ram(GBContext* ctx) {
    if (!ctx || !ctx->eram || !ctx->eram_size || !ctx->callbacks.save_battery_ram) {
        return false;
    }
    
    /* Get ROM title for filename */
    char title[17] = {0};
    if (ctx->rom_size > 0x143) {
        memcpy(title, &ctx->rom[0x134], 16);
        for(int i=0; i<16; i++) {
            if(title[i] == 0 || title[i] < 32 || title[i] > 126) title[i] = 0;
        }
    }
    if(title[0] == 0) strcpy(title, "UNKNOWN_GAME");
    
    bool result = ctx->callbacks.save_battery_ram(ctx, title, ctx->eram, ctx->eram_size);
    if (result) {
        printf("[GBRT] Saved battery RAM for '%s'\n", title);
    } else {
        printf("[GBRT] Failed to save battery RAM for '%s'\n", title);
    }
    return result;
}

/* ============================================================================
 * Memory Access
 * ========================================================================== */

/* Forward declaration - defined in Timing section below */
static inline void gb_sync(GBContext* ctx);

uint8_t gb_read8(GBContext* ctx, uint16_t addr) {
    /* During OAM DMA, only HRAM (0xFF80-0xFFFE) is accessible */
    if (ctx->dma.active && !(addr >= 0xFF80 && addr < 0xFFFF)) {
        return 0xFF;  /* Bus conflict - return undefined */
    }
    
    /* ROM Bank 0 (0x0000-0x3FFF) */
    if (addr < 0x4000) {
        /* MBC1 Mode 1: Upper bits affect bank 0 region too */
        if (ctx->mbc_type >= 0x01 && ctx->mbc_type <= 0x03 && ctx->mbc_mode == 1) {
            uint32_t bank0 = (uint32_t)ctx->rom_bank_upper << 5;
            uint32_t rom_addr = (bank0 * 0x4000) + addr;
            if (rom_addr < ctx->rom_size) {
                return ctx->rom[rom_addr];
            }
            return 0xFF;
        }
        return ctx->rom[addr];
    }
    
    /* ROM Bank N (0x4000-0x7FFF) */
    if (addr < 0x8000) {
        uint32_t rom_addr = ((uint32_t)ctx->rom_bank * 0x4000) + (addr - 0x4000);
        if (rom_addr < ctx->rom_size) {
            return ctx->rom[rom_addr];
        }
        return 0xFF;
    }
    
    /* VRAM (0x8000-0x9FFF) */
    if (addr < 0xA000) {
        /* Mode 3 VRAM read block disabled: our PPU sync granularity is
         * per-instruction, not per-cycle, so the STAT mode bits can be
         * stale when this check runs. False Mode 3 blocks cause the game's
         * tile data copy/restore routines to read 0xFF instead of real
         * data, corrupting tiles (visible as yellow blocks after text). */
        return ctx->vram[(ctx->vram_bank * VRAM_SIZE) + (addr - 0x8000)];
    }
    
    /* External RAM / RTC (0xA000-0xBFFF) */
    if (addr < 0xC000) {
        if (!ctx->ram_enabled) return 0xFF;
        
        /* MBC3 RTC mode */
        if (ctx->rtc_mode) {
            switch (ctx->rtc_reg) {
                case 0x08: return ctx->rtc.latched_s;
                case 0x09: return ctx->rtc.latched_m;
                case 0x0A: return ctx->rtc.latched_h;
                case 0x0B: return ctx->rtc.latched_dl;
                case 0x0C: return ctx->rtc.latched_dh;
                default: return 0xFF;
            }
        }
        
        /* MBC2: 512x4 bit internal RAM (upper 4 bits always high) */
        if (ctx->mbc_type >= 0x05 && ctx->mbc_type <= 0x06) {
            /* MBC2 RAM is only 512 bytes, echoed throughout 0xA000-0xBFFF */
            if (ctx->eram) {
                return ctx->eram[(addr - 0xA000) & 0x1FF] | 0xF0;
            }
            return 0xFF;
        }
        
        /* Standard external RAM */
        if (ctx->eram) {
            uint32_t eram_addr = ((uint32_t)ctx->ram_bank * 0x2000) + (addr - 0xA000);
            if (eram_addr < ctx->eram_size) {
                uint8_t val = ctx->eram[eram_addr];
                        return val;
            }
        }
        return 0xFF;
    }
    if (addr < 0xD000) return ctx->wram[addr - 0xC000];
    if (addr < 0xE000) return ctx->wram[(ctx->wram_bank * WRAM_BANK_SIZE) + (addr - 0xD000)];
    if (addr < 0xFE00) return gb_read8(ctx, addr - 0x2000);
    if (addr < 0xFEA0) {
        uint8_t stat = ctx->io[0x41] & 3;
        if (stat == 2 || stat == 3) return 0xFF;
        return ctx->oam[addr - 0xFE00];
    }
    if (addr < 0xFF00) return 0xFF;
    if (addr < 0xFF80) {
        if (addr == 0xFF00) {
             // DBG_GENERAL("Reading JOYP 0xFF00");
             uint8_t joyp = ctx->io[0x00];
             // Bits 6-7 always 1. Bits 4-5 return what was written.
             uint8_t res = 0xC0 | (joyp & 0x30) | 0x0F;
             if (!(joyp & 0x10)) res &= g_joypad_dpad;
             if (!(joyp & 0x20)) res &= g_joypad_buttons;
             return res;
        }
        if (addr == 0xFF04) return (uint8_t)(ctx->div_counter >> 8);
        if (addr >= 0xFF40 && addr <= 0xFF4B) {
            /* Sync PPU before reading its registers so mode/LY are up-to-date */
            gb_sync(ctx);
            return ppu_read_register((GBPPU*)ctx->ppu, addr);
        }
        if (addr >= 0xFF68 && addr <= 0xFF6B) return ppu_read_register((GBPPU*)ctx->ppu, addr);
        if (addr == 0xFF4F) return ctx->vram_bank | 0xFE; /* VBK: only bit 0 readable */
        if (addr == 0xFF70) return ctx->wram_bank ? ctx->wram_bank : 1; /* SVBK */
        if (addr >= 0xFF51 && addr <= 0xFF55) {
            GBPPU* ppu = (GBPPU*)ctx->ppu;
            if (addr == 0xFF55) return ppu->hdma_active ? (ppu->hdma_len & 0x7F) : 0xFF;
            return 0xFF; /* FF51-FF54 are write-only */
        }
        if (addr >= 0xFF10 && addr <= 0xFF3F) return gb_audio_read(ctx, addr);
        return ctx->io[addr - 0xFF00];
    }
    if (addr < 0xFFFF) return ctx->hram[addr - 0xFF80];
    if (addr == 0xFFFF) return ctx->io[0x80];
    return 0xFF;
}

void gb_write8(GBContext* ctx, uint16_t addr, uint8_t value) {
    /* During OAM DMA, only HRAM (0xFF80-0xFFFE) is writable */
    if (ctx->dma.active && !(addr >= 0xFF80 && addr < 0xFFFF)) {
        return;  /* Bus conflict - write ignored */
    }
    
    /* MBC Write Handling */
    if (addr < 0x8000) {
        /* ================================================================
         * MBC1 (Cartridge types 0x01, 0x02, 0x03)
         * ================================================================ */
        if (ctx->mbc_type >= 0x01 && ctx->mbc_type <= 0x03) {
            if (addr < 0x2000) {
                /* 0x0000-0x1FFF: RAM Enable */
                ctx->ram_enabled = ((value & 0x0F) == 0x0A);
            } else if (addr < 0x4000) {
                /* 0x2000-0x3FFF: ROM Bank Number (lower 5 bits) */
                uint8_t bank = value & 0x1F;
                if (bank == 0) bank = 1;  /* Bank 0 is not selectable */
                ctx->rom_bank = (ctx->rom_bank & 0x60) | bank;
            } else if (addr < 0x6000) {
                /* 0x4000-0x5FFF: RAM Bank / Upper ROM Bank bits */
                ctx->rom_bank_upper = value & 0x03;
                if (ctx->mbc_mode == 0) {
                    /* Mode 0: Upper 2 bits go to ROM bank */
                    ctx->rom_bank = (ctx->rom_bank & 0x1F) | (ctx->rom_bank_upper << 5);
                } else {
                    /* Mode 1: Used as RAM bank */
                    ctx->ram_bank = ctx->rom_bank_upper;
                }
            } else {
                /* 0x6000-0x7FFF: Banking Mode Select */
                ctx->mbc_mode = value & 0x01;
                if (ctx->mbc_mode == 0) {
                    /* Mode 0: RAM bank fixed to 0, upper bits go to ROM */
                    ctx->ram_bank = 0;
                    ctx->rom_bank = (ctx->rom_bank & 0x1F) | (ctx->rom_bank_upper << 5);
                } else {
                    /* Mode 1: RAM bank from upper bits, ROM bank fixed lower region */
                    ctx->ram_bank = ctx->rom_bank_upper;
                }
            }
            /* MBC1 quirk: Banks 0x00, 0x20, 0x40, 0x60 map to 0x01, 0x21, 0x41, 0x61 */
            if ((ctx->rom_bank & 0x1F) == 0) {
                ctx->rom_bank = (ctx->rom_bank & 0x60) | 0x01;
            }
        }
        /* ================================================================
         * MBC2 (Cartridge types 0x05, 0x06)
         * ================================================================ */
        else if (ctx->mbc_type >= 0x05 && ctx->mbc_type <= 0x06) {
            if (addr < 0x4000) {
                /* MBC2: Bit 8 of addr determines RAM enable vs ROM bank */
                if (addr & 0x0100) {
                    /* 0x2100-0x3FFF: ROM Bank Number (lower 4 bits) */
                    ctx->rom_bank = value & 0x0F;
                    if (ctx->rom_bank == 0) ctx->rom_bank = 1;
                } else {
                    /* 0x0000-0x1FFF: RAM Enable (if bit 8 is 0) */
                    ctx->ram_enabled = ((value & 0x0F) == 0x0A);
                }
            }
            /* 0x4000-0x7FFF: Unused for MBC2 */
        }
        /* ================================================================
         * MBC3 (Cartridge types 0x0F, 0x10, 0x11, 0x12, 0x13)
         * ================================================================ */
        else if (ctx->mbc_type >= 0x0F && ctx->mbc_type <= 0x13) {
            if (addr < 0x2000) {
                /* RAM/RTC Enable */
                ctx->ram_enabled = ((value & 0x0F) == 0x0A);
            } else if (addr < 0x4000) {
                /* ROM Bank Number (1-127) */
                ctx->rom_bank = value & 0x7F;
                if (ctx->rom_bank == 0) ctx->rom_bank = 1;
            } else if (addr < 0x6000) {
                /* RAM Bank Number or RTC Register Select */
                if (value <= 0x03) {
                    ctx->rtc_mode = 0;
                    ctx->ram_bank = value;
                } else if (value >= 0x08 && value <= 0x0C) {
                    ctx->rtc_mode = 1;
                    ctx->rtc_reg = value;
                }
            } else {
                /* Latch Clock Data */
                if (ctx->rtc.latch_state == 0 && value == 0) {
                    ctx->rtc.latch_state = 1;
                } else if (ctx->rtc.latch_state == 1 && value == 1) {
                    ctx->rtc.latch_state = 0;
                    /* Latch current time */
                    ctx->rtc.latched_s = ctx->rtc.s;
                    ctx->rtc.latched_m = ctx->rtc.m;
                    ctx->rtc.latched_h = ctx->rtc.h;
                    ctx->rtc.latched_dl = ctx->rtc.dl;
                    ctx->rtc.latched_dh = ctx->rtc.dh;
                } else {
                    ctx->rtc.latch_state = 0;
                }
            }
        }
        /* ================================================================
         * MBC5 (Cartridge types 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E)
         * ================================================================ */
        else if (ctx->mbc_type >= 0x19 && ctx->mbc_type <= 0x1E) {
            if (addr < 0x2000) {
                /* RAM Enable */
                uint8_t old = ctx->ram_enabled;
                ctx->ram_enabled = ((value & 0x0F) == 0x0A);
                if (old != ctx->ram_enabled) {
                    fprintf(stderr, "[MBC5] RAM %s (wrote 0x%02X to 0x%04X)\n", ctx->ram_enabled ? "ENABLED" : "DISABLED", value, addr);
                }
            } else if (addr < 0x3000) {
                /* ROM Bank Number (lower 8 bits) */
                uint16_t new_bank = (ctx->rom_bank & 0x100) | value;
                /* Mask to actual ROM size to prevent out-of-bounds reads */
                uint16_t max_bank = (ctx->rom_size / 0x4000);
                if (max_bank > 0 && new_bank >= max_bank) {
                    fprintf(stderr, "[MBC5] Bank %d out of range (max %d), masking to %d. PC=%04X A=%02X value=%02X old_bank=%d\n",
                            new_bank, max_bank - 1, new_bank % max_bank, ctx->pc, ctx->a, value, ctx->rom_bank);
                    new_bank = new_bank % max_bank;
                }
                ctx->rom_bank = new_bank;
                /* MBC5 allows bank 0 - no fixup needed */
            } else if (addr < 0x4000) {
                /* ROM Bank Number (9th bit) */
                ctx->rom_bank = (ctx->rom_bank & 0xFF) | ((value & 0x01) << 8);
            } else if (addr < 0x6000) {
                /* RAM Bank Number (0-15) */
                ctx->ram_bank = value & 0x0F;
            }
            /* 0x6000-0x7FFF: Unused for MBC5 */
        }
        /* ================================================================
         * No MBC / ROM Only (type 0x00) or Unknown
         * ================================================================ */
        else {
            /* Simple fallback: just ROM bank register */
            if (addr >= 0x2000 && addr < 0x4000) {
                ctx->rom_bank = value & 0x1F;
                if (ctx->rom_bank == 0) ctx->rom_bank = 1;
            }
        }
        return;
    }
    if (addr < 0xA000) {
        /* VRAM Write - Check STAT mode 3 */
        // if ((ctx->io[0x41] & 3) == 3) return;
        
        ctx->vram[(ctx->vram_bank * VRAM_SIZE) + (addr - 0x8000)] = value;
        return;
    }
    if (addr < 0xC000) {
        /* External RAM / RTC Write */
        if (!ctx->ram_enabled) {
            static int sram_blocked = 0;
            if (sram_blocked++ < 20) {
                fprintf(stderr, "[SRAM] BLOCKED write to 0x%04X = 0x%02X (RAM not enabled, bank=%d)\n", addr, value, ctx->ram_bank);
            }
            return;
        }
        
        /* MBC3 RTC mode */
        if (ctx->rtc_mode) {
            /* RTC Register Write */
            switch (ctx->rtc_reg) {
                case 0x08: ctx->rtc.s = value % 60; break;
                case 0x09: ctx->rtc.m = value % 60; break;
                case 0x0A: ctx->rtc.h = value % 24; break;
                case 0x0B: ctx->rtc.dl = value; break;
                case 0x0C: 
                    ctx->rtc.dh = value; 
                    ctx->rtc.active = !(value & 0x40); /* Bit 6 is Halt */
                    break;
            }
            return;
        }
        
        /* MBC2: 512x4 bit internal RAM (only lower 4 bits stored) */
        if (ctx->mbc_type >= 0x05 && ctx->mbc_type <= 0x06) {
            if (ctx->eram) {
                ctx->eram[(addr - 0xA000) & 0x1FF] = value & 0x0F;
            }
            return;
        }
        
        /* Standard external RAM */
        if (ctx->eram) {
            uint32_t eram_addr = ((uint32_t)ctx->ram_bank * 0x2000) + (addr - 0xA000);
            if (eram_addr < ctx->eram_size) {
                ctx->eram[eram_addr] = value;
                /* SRAM write logging disabled */
            }
        }
        return;
    }
    if (addr < 0xD000) { ctx->wram[addr - 0xC000] = value; return; }
    if (addr < 0xE000) {
        uint32_t wram_off = (ctx->wram_bank * WRAM_BANK_SIZE) + (addr - 0xD000);
        /* Watch game state variables */
        if (addr == 0xDB95 && value != ctx->wram[wram_off]) {
            fprintf(stderr, "[STATE] wGameplayType %02X→%02X PC=0x%04X bank=%d SP=%04X\n",
                    ctx->wram[wram_off], value, ctx->pc, ctx->rom_bank, ctx->sp);
            /* Dump stack for call trace */
            fprintf(stderr, "[STATE]   Stack: ");
            for (int si = 0; si < 8; si++) {
                uint16_t ra = gb_read8(ctx, ctx->sp + si*2) | (gb_read8(ctx, ctx->sp + si*2 + 1) << 8);
                fprintf(stderr, "%04X ", ra);
            }
            fprintf(stderr, "\n");
            /* Also dump D6FE/D6FF */
            fprintf(stderr, "[STATE]   D6FE=%02X D6FF=%02X DB96=%02X FFF2=%02X\n",
                    gb_read8(ctx, 0xD6FE), gb_read8(ctx, 0xD6FF),
                    gb_read8(ctx, 0xDB96), gb_read8(ctx, 0xFFF2));
        }
        ctx->wram[wram_off] = value;
        return;
    }
    if (addr < 0xFE00) { gb_write8(ctx, addr - 0x2000, value); return; }
    if (addr < 0xFEA0) { 
        /* OAM Write - Check STAT mode 2 or 3 */
        uint8_t stat = ctx->io[0x41] & 3;
        // if (stat == 2 || stat == 3) return;
        
        ctx->oam[addr - 0xFE00] = value; 
        return; 
    }
    if (addr < 0xFF00) return;
    if (addr < 0xFF80) {
        if (addr >= 0xFF40 && addr <= 0xFF4B) { ppu_write_register((GBPPU*)ctx->ppu, ctx, addr, value); return; }
        if (addr >= 0xFF68 && addr <= 0xFF6B) { ppu_write_register((GBPPU*)ctx->ppu, ctx, addr, value); return; }
        if (addr == 0xFF4F) { ctx->vram_bank = value & 0x01; return; } /* VBK: VRAM bank select */
        if (addr == 0xFF70) { /* SVBK: WRAM bank select */
            ctx->wram_bank = value & 0x07;
            if (ctx->wram_bank == 0) ctx->wram_bank = 1;
            return;
        }
        if (addr >= 0xFF51 && addr <= 0xFF55) { /* HDMA registers */
            GBPPU* ppu = (GBPPU*)ctx->ppu;
            switch (addr) {
                case 0xFF51: ppu->hdma_src = (ppu->hdma_src & 0x00FF) | (value << 8); return;
                case 0xFF52: ppu->hdma_src = (ppu->hdma_src & 0xFF00) | (value & 0xF0); return;
                case 0xFF53: ppu->hdma_dst = (ppu->hdma_dst & 0x00FF) | ((value & 0x1F) << 8); return;
                case 0xFF54: ppu->hdma_dst = (ppu->hdma_dst & 0xFF00) | (value & 0xF0); return;
                case 0xFF55: {
                    if (ppu->hdma_active && !(value & 0x80)) {
                        /* Cancel active HDMA */
                        ppu->hdma_active = false;
                        return;
                    }
                    uint8_t len = (value & 0x7F) + 1; /* Number of 16-byte blocks */
                    if (value & 0x80) {
                        /* HBlank DMA (HDMA) - transfer 16 bytes per HBlank */
                        ppu->hdma_len = value & 0x7F;
                        ppu->hdma_active = true;
                    } else {
                        /* General Purpose DMA (GDMA) - transfer all at once */
                        uint16_t src = ppu->hdma_src;
                        uint16_t dst = 0x8000 | ppu->hdma_dst;
                        for (int i = 0; i < len * 16; i++) {
                            uint8_t byte = gb_read8(ctx, src + i);
                            ctx->vram[(ctx->vram_bank * 0x2000) + ((dst + i) & 0x1FFF)] = byte;
                        }
                        ppu->hdma_src += len * 16;
                        ppu->hdma_dst = (ppu->hdma_dst + len * 16) & 0x1FFF;
                        ppu->hdma_active = false;
                        ppu->hdma_len = 0xFF;
                        /* GDMA takes len * 8 M-cycles in normal speed */
                        gb_add_cycles(ctx, len * 32);
                    }
                    return;
                }
            }
            return;
        }
        if (addr >= 0xFF10 && addr <= 0xFF3F) { gb_audio_write(ctx, addr, value); return; }
        if (addr == 0xFF04) { 
            uint16_t old_div = ctx->div_counter;
            ctx->div_counter = 0; 
            ctx->io[0x04] = 0; /* Update register view immediately */
            if (ctx->apu) gb_audio_div_reset(ctx->apu);
            
            /* DIV Reset Glitch: 
             * If the selected bit for TIMA is 1 in old_div and becomes 0 (it does, since div is 0),
             * this counts as a falling edge and increments TIMA.
             */
             uint8_t tac = ctx->io[0x07];
             if (tac & 0x04) { /* Timer Enabled */
                uint16_t mask;
                switch (tac & 0x03) {
                    case 0: mask = 1 << 9; break; /* 1024 cycles */
                    case 1: mask = 1 << 3; break; /* 16 cycles */
                    case 2: mask = 1 << 5; break; /* 64 cycles */
                    case 3: mask = 1 << 7; break; /* 256 cycles */
                    default: mask = 0; break;
                }
                if (old_div & mask) {
                    /* Glitch triggered: Increment TIMA */
                    if (ctx->io[0x05] == 0xFF) { 
                        ctx->io[0x05] = ctx->io[0x06]; 
                        ctx->io[0x0F] |= 0x04; 
                    } else {
                        ctx->io[0x05]++;
                    }
                }
             }
            return; 
        }
        if (addr == 0xFF46) {
             /* OAM DMA: Start transfer (takes 160 M-cycles = 640 T-cycles) */
             /* Prevent DMA from invalid regions if needed, but hardware allows it (reads FF/garbage) */
             ctx->dma.source_high = value;
             ctx->dma.progress = 0;
             ctx->dma.cycles_remaining = 640;
             ctx->dma.active = 1;
             return;
        }
        if (addr == 0xFF02 && (value & 0x80)) {
            printf("%c", ctx->io[0x01]); fflush(stdout);
            ctx->io[0x0F] |= 0x08;
        }
        ctx->io[addr - 0xFF00] = value;
        return;
    }
    if (addr < 0xFFFF) { 
        // if (addr >= 0xFF80 && addr <= 0xFF8F) {
        //      DBG_GENERAL("Writing to HRAM[%04X]: %02X", addr, value);
        // }
        ctx->hram[addr - 0xFF80] = value; return; 
    }
    if (addr == 0xFFFF) { ctx->io[0x80] = value; return; }
}

uint16_t gb_read16(GBContext* ctx, uint16_t addr) {
    return (uint16_t)gb_read8(ctx, addr) | ((uint16_t)gb_read8(ctx, addr + 1) << 8);
}

void gb_write16(GBContext* ctx, uint16_t addr, uint16_t value) {
    gb_write8(ctx, addr, value & 0xFF);
    gb_write8(ctx, addr + 1, value >> 8);
}

void gb_push16(GBContext* ctx, uint16_t value) {
    ctx->sp -= 2;
    gb_write16(ctx, ctx->sp, value);
}

uint16_t gb_pop16(GBContext* ctx) {
    uint16_t val = gb_read16(ctx, ctx->sp);
    ctx->sp += 2;
    return val;
}

/* ============================================================================
 * ALU
 * ========================================================================== */

void gb_add8(GBContext* ctx, uint8_t value) {
    uint32_t res = (uint32_t)ctx->a + value;
    ctx->f_z = (res & 0xFF) == 0;
    ctx->f_n = 0;
    ctx->f_h = ((ctx->a & 0x0F) + (value & 0x0F)) > 0x0F;
    ctx->f_c = res > 0xFF;
    ctx->a = (uint8_t)res;
}
void gb_adc8(GBContext* ctx, uint8_t value) {
    uint8_t carry = ctx->f_c ? 1 : 0;
    uint32_t res = (uint32_t)ctx->a + value + carry;
    ctx->f_z = (res & 0xFF) == 0;
    ctx->f_n = 0;
    ctx->f_h = ((ctx->a & 0x0F) + (value & 0x0F) + carry) > 0x0F;
    ctx->f_c = res > 0xFF;
    ctx->a = (uint8_t)res;
}
void gb_sub8(GBContext* ctx, uint8_t value) {
    ctx->f_z = ctx->a == value;
    ctx->f_n = 1;
    ctx->f_h = (ctx->a & 0x0F) < (value & 0x0F);
    ctx->f_c = ctx->a < value;
    ctx->a -= value;
}
void gb_sbc8(GBContext* ctx, uint8_t value) {
    uint8_t carry = ctx->f_c ? 1 : 0;
    int res = (int)ctx->a - (int)value - carry;
    ctx->f_z = (res & 0xFF) == 0;
    ctx->f_n = 1;
    ctx->f_h = ((int)(ctx->a & 0x0F) - (int)(value & 0x0F) - (int)carry) < 0;
    ctx->f_c = res < 0;
    ctx->a = (uint8_t)res;
}
void gb_and8(GBContext* ctx, uint8_t value) { ctx->a &= value; ctx->f_z = ctx->a == 0; ctx->f_n = 0; ctx->f_h = 1; ctx->f_c = 0; }
void gb_or8(GBContext* ctx, uint8_t value) { ctx->a |= value; ctx->f_z = ctx->a == 0; ctx->f_n = 0; ctx->f_h = 0; ctx->f_c = 0; }
void gb_xor8(GBContext* ctx, uint8_t value) { ctx->a ^= value; ctx->f_z = ctx->a == 0; ctx->f_n = 0; ctx->f_h = 0; ctx->f_c = 0; }
void gb_cp8(GBContext* ctx, uint8_t value) {
    ctx->f_z = ctx->a == value;
    ctx->f_n = 1;
    ctx->f_h = (ctx->a & 0x0F) < (value & 0x0F);
    ctx->f_c = ctx->a < value;
}
uint8_t gb_inc8(GBContext* ctx, uint8_t val) {
    ctx->f_h = (val & 0x0F) == 0x0F;
    val++;
    ctx->f_z = val == 0;
    ctx->f_n = 0;
    return val;
}
uint8_t gb_dec8(GBContext* ctx, uint8_t val) {
    ctx->f_h = (val & 0x0F) == 0;
    val--;
    ctx->f_z = val == 0;
    ctx->f_n = 1;
    return val;
}
void gb_add16(GBContext* ctx, uint16_t val) {
    uint32_t res = (uint32_t)ctx->hl + val;
    ctx->f_n = 0;
    ctx->f_h = ((ctx->hl & 0x0FFF) + (val & 0x0FFF)) > 0x0FFF;
    ctx->f_c = res > 0xFFFF;
    ctx->hl = (uint16_t)res;
}
void gb_add_sp(GBContext* ctx, int8_t off) {
    ctx->f_z = 0; ctx->f_n = 0;
    ctx->f_h = ((ctx->sp & 0x0F) + (off & 0x0F)) > 0x0F;
    ctx->f_c = ((ctx->sp & 0xFF) + (off & 0xFF)) > 0xFF;
    ctx->sp += off;
}
void gb_ld_hl_sp_n(GBContext* ctx, int8_t off) {
    ctx->f_z = 0; ctx->f_n = 0;
    ctx->f_h = ((ctx->sp & 0x0F) + (off & 0x0F)) > 0x0F;
    ctx->f_c = ((ctx->sp & 0xFF) + (off & 0xFF)) > 0xFF;
    ctx->hl = ctx->sp + off;
}

uint8_t gb_rlc(GBContext* ctx, uint8_t v) { ctx->f_c = v >> 7; v = (v << 1) | ctx->f_c; ctx->f_z = v == 0; ctx->f_n = 0; ctx->f_h = 0; return v; }
uint8_t gb_rrc(GBContext* ctx, uint8_t v) { ctx->f_c = v & 1; v = (v >> 1) | (ctx->f_c << 7); ctx->f_z = v == 0; ctx->f_n = 0; ctx->f_h = 0; return v; }
uint8_t gb_rl(GBContext* ctx, uint8_t v) { uint8_t c = ctx->f_c; ctx->f_c = v >> 7; v = (v << 1) | c; ctx->f_z = v == 0; ctx->f_n = 0; ctx->f_h = 0; return v; }
uint8_t gb_rr(GBContext* ctx, uint8_t v) { uint8_t c = ctx->f_c; ctx->f_c = v & 1; v = (v >> 1) | (c << 7); ctx->f_z = v == 0; ctx->f_n = 0; ctx->f_h = 0; return v; }
uint8_t gb_sla(GBContext* ctx, uint8_t v) { ctx->f_c = v >> 7; v <<= 1; ctx->f_z = v == 0; ctx->f_n = 0; ctx->f_h = 0; return v; }
uint8_t gb_sra(GBContext* ctx, uint8_t v) { ctx->f_c = v & 1; v = (uint8_t)((int8_t)v >> 1); ctx->f_z = v == 0; ctx->f_n = 0; ctx->f_h = 0; return v; }
uint8_t gb_swap(GBContext* ctx, uint8_t v) { v = (uint8_t)((v << 4) | (v >> 4)); ctx->f_z = v == 0; ctx->f_n = 0; ctx->f_h = 0; ctx->f_c = 0; return v; }
uint8_t gb_srl(GBContext* ctx, uint8_t v) { ctx->f_c = v & 1; v >>= 1; ctx->f_z = v == 0; ctx->f_n = 0; ctx->f_h = 0; return v; }
void gb_bit(GBContext* ctx, uint8_t bit, uint8_t v) { ctx->f_z = !(v & (1 << bit)); ctx->f_n = 0; ctx->f_h = 1; }

void gb_rlca(GBContext* ctx) { ctx->a = gb_rlc(ctx, ctx->a); ctx->f_z = 0; }
void gb_rrca(GBContext* ctx) { ctx->a = gb_rrc(ctx, ctx->a); ctx->f_z = 0; }
void gb_rla(GBContext* ctx) { ctx->a = gb_rl(ctx, ctx->a); ctx->f_z = 0; }
void gb_rra(GBContext* ctx) { ctx->a = gb_rr(ctx, ctx->a); ctx->f_z = 0; }

void gb_daa(GBContext* ctx) {
   int a = ctx->a;
   if (!ctx->f_n) {
       if (ctx->f_h || (a & 0xF) > 9) a += 0x06;
       if (ctx->f_c || a > 0x9F) a += 0x60;
   } else {
       if (ctx->f_h) a = (a - 6) & 0xFF;
       if (ctx->f_c) a -= 0x60;
   }
   
   ctx->f_h = 0;
   if ((a & 0x100) == 0x100) ctx->f_c = 1;
   
   a &= 0xFF;
   ctx->f_z = (a == 0);
   ctx->a = (uint8_t)a;
}

/* ============================================================================
 * Control Flow helpers
 * ========================================================================== */

void gb_ret(GBContext* ctx) { ctx->pc = gb_pop16(ctx); }
void gbrt_jump_hl(GBContext* ctx) { ctx->pc = ctx->hl; }
void gb_rst(GBContext* ctx, uint8_t vec) { gb_push16(ctx, ctx->pc); ctx->pc = vec; }

void gbrt_set_trace_file(const char* filename) {
    if (gbrt_trace_filename) free(gbrt_trace_filename);
    if (filename) gbrt_trace_filename = strdup(filename);
    else gbrt_trace_filename = NULL;
}

void gbrt_log_trace(GBContext* ctx, uint16_t bank, uint16_t addr) {
    if (ctx->trace_entries_enabled && ctx->trace_file) {
        fprintf((FILE*)ctx->trace_file, "%d:%04x\n", (int)bank, (int)addr);
    }
}

/* Default dispatch implementations.
 * On GCC/Clang these are weak symbols that can be overridden.
 * On MSVC they are regular functions (pokemon uses its own dispatch_call). */
#if !defined(_MSC_VER)
__attribute__((weak))
#endif
void gb_dispatch(GBContext* ctx, uint16_t addr) {
    gbrt_log_trace(ctx, (addr < 0x4000) ? 0 : ctx->rom_bank, addr);
    ctx->pc = addr;
    gb_interpret(ctx, addr);
}

#if !defined(_MSC_VER)
__attribute__((weak))
#endif
void gb_dispatch_call(GBContext* ctx, uint16_t addr) {
    gbrt_log_trace(ctx, (addr < 0x4000) ? 0 : ctx->rom_bank, addr);
    ctx->pc = addr;
}

/* ============================================================================
 * Timing & Hardware Sync
 * ========================================================================== */

static inline void gb_sync(GBContext* ctx) {
    uint32_t current = ctx->cycles;
    uint32_t delta = current - ctx->last_sync_cycles;
    if (delta > 0) {
        ctx->last_sync_cycles = current;
        if (ctx->ppu) ppu_tick((GBPPU*)ctx->ppu, ctx, delta);
    }
}

void gb_add_cycles(GBContext* ctx, uint32_t cycles) {
    ctx->cycles += cycles;
    ctx->frame_cycles += cycles;
}



static void gb_rtc_tick(GBContext* ctx, uint32_t cycles) {
    if (!ctx->rtc.active) return;
    
    /* Update RTC time */
    ctx->rtc.last_time += cycles;
    while (ctx->rtc.last_time >= 4194304) { /* 1 second at 4.194304 MHz */
        ctx->rtc.last_time -= 4194304;
        
        ctx->rtc.s++;
        if (ctx->rtc.s >= 60) {
            ctx->rtc.s = 0;
            ctx->rtc.m++;
            if (ctx->rtc.m >= 60) {
                ctx->rtc.m = 0;
                ctx->rtc.h++;
                if (ctx->rtc.h >= 24) {
                    ctx->rtc.h = 0;
                    uint16_t d = ctx->rtc.dl | ((ctx->rtc.dh & 1) << 8);
                    d++;
                    ctx->rtc.dl = d & 0xFF;
                    if (d > 0x1FF) {
                        ctx->rtc.dh |= 0x80; /* Overflow */
                        ctx->rtc.dh &= 0xFE; /* Clear 9th bit */
                    } else {
                        ctx->rtc.dh = (ctx->rtc.dh & 0xFE) | ((d >> 8) & 1);
                    }
                }
            }
        }
    }
}

/**
 * Process OAM DMA transfer
 * DMA takes 160 M-cycles (640 T-cycles), copying 1 byte per M-cycle
 */
static void gb_dma_tick(GBContext* ctx, uint32_t cycles) {
    if (!ctx->dma.active) return;
    
    /* Process DMA cycles */
    while (cycles > 0 && ctx->dma.active) {
        /* Each byte takes 4 T-cycles (1 M-cycle) */
        uint32_t byte_cycles = (cycles >= 4) ? 4 : cycles;
        cycles -= byte_cycles;
        ctx->dma.cycles_remaining -= byte_cycles;
        
        /* Copy one byte every 4 T-cycles */
        if (ctx->dma.progress < 160 && (ctx->dma.cycles_remaining % 4) == 0) {
            uint16_t src_addr = ((uint16_t)ctx->dma.source_high << 8) | ctx->dma.progress;
            /* Directly access ROM/RAM without triggering normal restrictions */
            uint8_t byte;
            if (src_addr < 0x8000) {
                /* ROM */
                if (src_addr < 0x4000) {
                    byte = ctx->rom[src_addr];
                } else {
                    byte = ctx->rom[(ctx->rom_bank * 0x4000) + (src_addr - 0x4000)];
                }
            } else if (src_addr < 0xA000) {
                /* VRAM */
                byte = ctx->vram[src_addr - 0x8000];
            } else if (src_addr < 0xC000) {
                /* External RAM */
                byte = ctx->eram ? ctx->eram[(ctx->ram_bank * 0x2000) + (src_addr - 0xA000)] : 0xFF;
            } else if (src_addr < 0xE000) {
                /* WRAM */
                if (src_addr < 0xD000) {
                    byte = ctx->wram[src_addr - 0xC000];
                } else {
                    byte = ctx->wram[(ctx->wram_bank * 0x1000) + (src_addr - 0xD000)];
                }
            } else {
                byte = 0xFF;
            }
            ctx->oam[ctx->dma.progress] = byte;
            ctx->dma.progress++;
        }
        
        /* Check if DMA is complete */
        if (ctx->dma.progress >= 160 || ctx->dma.cycles_remaining == 0) {
            ctx->dma.active = 0;
        }
    }
}

void gb_tick(GBContext* ctx, uint32_t cycles) {
    static uint32_t last_log = 0;
    
    // Check limit
    if (gbrt_instruction_limit > 0) {
        gbrt_instruction_count++;
        if (gbrt_instruction_count >= gbrt_instruction_limit) {
            printf("Instruction limit reached (%llu)\n", (unsigned long long)gbrt_instruction_limit);
            exit(0);
        }
    }

    if (gbrt_trace_enabled && ctx->cycles - last_log >= 10000) {
        last_log = ctx->cycles;
        fprintf(stderr, "[TICK] Cycles: %u, PC: 0x%04X, IME: %d, IF: 0x%02X, IE: 0x%02X\n", 
                ctx->cycles, ctx->pc, ctx->ime, ctx->io[0x0F], ctx->io[0x80]);
    }
    gb_add_cycles(ctx, cycles);
    
    /* RTC Tick */
    gb_rtc_tick(ctx, cycles);
    
    /* OAM DMA Tick */
    gb_dma_tick(ctx, cycles);

    /* Update DIV and TIMA */
    uint16_t old_div = ctx->div_counter;
    ctx->div_counter += (uint16_t)cycles;
    ctx->io[0x04] = (uint8_t)(ctx->div_counter >> 8);
    
    uint8_t tac = ctx->io[0x07];
    if (tac & 0x04) { /* Timer Enabled */
        uint16_t mask;
        switch (tac & 0x03) {
            case 0: mask = 1 << 9; break; /* 4096 Hz (1024 cycles) -> bit 9 */
            case 1: mask = 1 << 3; break; /* 262144 Hz (16 cycles) -> bit 3 */
            case 2: mask = 1 << 5; break; /* 65536 Hz (64 cycles) -> bit 5 */
            case 3: mask = 1 << 7; break; /* 16384 Hz (256 cycles) -> bit 7 */
            default: mask = 0; break;
        }
        
        /* Check for falling edges.
           We detect how many times the bit flipped from 1 to 0.
           The bit flips every 'mask' cycles (period is 2*mask).
           We iterate to find all falling edges in the range. 
        */
        uint16_t current = old_div;
        uint32_t cycles_left = cycles;
        
        /* Optimization: if cycles are small (common case), doing a loop is fine. */
        while (cycles_left > 0) {
            /* Next falling edge is at next multiple of (2*mask) */
            uint16_t next_fall = (current | (mask * 2 - 1)) + 1;
            
            /* Distance to next fall */
            uint32_t dist = (uint16_t)(next_fall - current);
            if (dist == 0) dist = mask * 2; /* Should happen if current is exactly on edge? */
            
            /* Check if we reach the fall */
            if (cycles_left >= dist) {
                /* Validate it is a falling edge for the selected bit?
                   next_fall is the transition 11...1 -> 00...0 for bits < bit+1.
                   Bit 'mask' definitely transitions. 
                   Wait, next multiple of 2*mask means mask bit becomes 0.
                   So yes, next_fall is a falling edge point.
                */
                if (ctx->io[0x05] == 0xFF) { 
                    ctx->io[0x05] = ctx->io[0x06]; /* Reload TMA */
                    ctx->io[0x0F] |= 0x04;         /* Request Timer Interrupt */
                } else {
                    ctx->io[0x05]++;
                }
                current += (uint16_t)dist;
                cycles_left -= dist;
            } else {
                break;
            }
        }
    }
    
    /* Sync PPU every tick for accurate frame boundaries.
     * This is slower than the ~256-cycle batch sync but ensures
     * recompiled functions don't overshoot VBlank by thousands of cycles. */
    gb_sync(ctx);

    /* Speed switch halt countdown */
    if (ctx->speed_switch_halt > 0) {
        ctx->speed_switch_halt -= (int32_t)cycles;
        if (ctx->speed_switch_halt <= 0) {
            ctx->speed_switch_halt = 0;
            ctx->halted = 0;
            fprintf(stderr, "[GBRT] Speed switch halt complete, resuming CPU\n");
        }
    }

    if (ctx->frame_done || (ctx->ime && (ctx->io[0x0F] & ctx->io[0x80] & 0x1F))) ctx->stopped = 1;
    if (ctx->apu) gb_audio_step(ctx, cycles);
    if (ctx->ime_pending) { ctx->ime = 1; ctx->ime_pending = 0; }
}

void gb_handle_interrupts(GBContext* ctx) {
    if (!ctx->ime) return;
    uint8_t if_reg = ctx->io[0x0F];
    uint8_t ie_reg = ctx->io[0x80];
    uint8_t pending = if_reg & ie_reg & 0x1F;
    if (pending) {
        ctx->ime = 0; ctx->halted = 0;
        uint16_t vec = 0; uint8_t bit = 0;
        if (pending & 0x01) { vec = 0x0040; bit = 0x01; }
        else if (pending & 0x02) { vec = 0x0048; bit = 0x02; }
        else if (pending & 0x04) { vec = 0x0050; bit = 0x04; }
        else if (pending & 0x08) { vec = 0x0058; bit = 0x08; }
        else if (pending & 0x10) { vec = 0x0060; bit = 0x10; }
        if (vec) {
            ctx->io[0x0F] &= ~bit;
            
            /* ISR takes 5 M-cycles (20 T-cycles) as per Pan Docs:
             * - 2 M-cycles: Wait states (NOPs)
             * - 2 M-cycles: Push PC to stack (SP decremented twice, PC written)
             * - 1 M-cycle: Set PC to interrupt vector
             */
            gb_tick(ctx, 8);  /* 2 wait M-cycles */
            gb_push16(ctx, ctx->pc);
            gb_tick(ctx, 8);  /* 2 push M-cycles */
            ctx->pc = vec;
            gb_tick(ctx, 4);  /* 1 jump M-cycle */
            ctx->stopped = 1;
        }
    }
}

/* ============================================================================
 * Execution
 * ========================================================================== */

uint32_t gb_run_frame(GBContext* ctx) {
    gb_reset_frame(ctx);
    uint32_t start = ctx->cycles;
    
    static int fcount = 0;
    fcount++;
    if (fcount % 60 == 0) {
        /* DB95 is at WRAM offset 0x1B95 (0xDB95 - 0xC000) */
        uint8_t gpt = ctx->wram[0x1B95];
        uint8_t gps = ctx->wram[0x1B96];
        fprintf(stderr, "[FRAME] Frame %d, Cycles: %u, GameplayType=%02X/%02X\n",
                fcount, ctx->cycles, gpt, gps);
    }

    while (!ctx->frame_done) {
        gb_handle_interrupts(ctx);
        
        /* Check for HALT exit condition (even if IME=0).
         * Don't exit halt during speed switch (countdown handles that). */
        if (ctx->halted && ctx->speed_switch_halt <= 0) {
             if (ctx->io[0x0F] & ctx->io[0x80] & 0x1F) {
                 ctx->halted = 0;
             }
        }
        
        ctx->stopped = 0;
        if (ctx->halted) gb_tick(ctx, 4);
        else gb_step(ctx);
        gb_sync(ctx);
    }
    return ctx->cycles - start;
}

/* Dispatch trace ring buffer for crash debugging */
#define DTRACE_SIZE 32
static struct { uint16_t pc; uint8_t bank; uint8_t a; uint16_t sp; uint16_t hl; } g_dtrace[DTRACE_SIZE];
static int g_dtrace_idx = 0;

void gb_dump_dtrace(void) {
    fprintf(stderr, "[DTRACE] Last %d dispatch entries:\n", DTRACE_SIZE);
    for (int i = 0; i < DTRACE_SIZE; i++) {
        int idx = (g_dtrace_idx + i) % DTRACE_SIZE;
        fprintf(stderr, "  [%2d] PC=%04X bank=%d A=%02X SP=%04X HL=%04X\n",
                i, g_dtrace[idx].pc, g_dtrace[idx].bank, g_dtrace[idx].a,
                g_dtrace[idx].sp, g_dtrace[idx].hl);
    }
}

uint32_t gb_step(GBContext* ctx) {
    if (gbrt_instruction_limit > 0 && ++gbrt_instruction_count >= gbrt_instruction_limit) {
        printf("Instruction limit reached (%llu)\n", (unsigned long long)gbrt_instruction_limit);
        exit(0);
    }

    /* Record dispatch trace */
    g_dtrace[g_dtrace_idx].pc = ctx->pc;
    g_dtrace[g_dtrace_idx].bank = ctx->rom_bank;
    g_dtrace[g_dtrace_idx].a = ctx->a;
    g_dtrace[g_dtrace_idx].sp = ctx->sp;
    g_dtrace[g_dtrace_idx].hl = ctx->hl;
    g_dtrace_idx = (g_dtrace_idx + 1) % DTRACE_SIZE;

    /* Handle HALT bug by falling back to interpreter for the next instruction */
    if (ctx->halt_bug) {
        gb_interpret(ctx, ctx->pc);
        return 0; /* Cycle counting handled by interpreter */
    }

    uint32_t start = ctx->cycles;
    gb_dispatch(ctx, ctx->pc);
    return ctx->cycles - start;
}

void gb_reset_frame(GBContext* ctx) {
    ctx->frame_done = 0;
    ctx->frame_cycles = 0;
    if (ctx->ppu) ppu_clear_frame_ready((GBPPU*)ctx->ppu);
}

const uint32_t* gb_get_framebuffer(GBContext* ctx) {
    if (ctx->ppu) return ppu_get_framebuffer((GBPPU*)ctx->ppu);
    return NULL;
}

void gb_halt(GBContext* ctx) { ctx->halted = 1; }

void gb_stop(GBContext* ctx) {
    /* CGB speed switch: if KEY1 bit 0 is set, toggle speed mode */
    uint8_t key1 = ctx->io[0x4D];
    if (key1 & 0x01) {
        /* Toggle bit 7 (current speed indicator), clear bit 0 (switch request) */
        ctx->io[0x4D] = (key1 ^ 0x80) & 0xFE;

        /* DIV counter resets immediately at start of speed switch */
        ctx->div_counter = 0;
        ctx->io[0x04] = 0;

        /* CPU halts for the speed switch duration. SameBoy uses 0x20008 M-cycles.
         * The speed toggle happens at M-cycle 6 (single→double). After that,
         * PPU gets 1:1 M-cycle mapping (double speed). So:
         * - First 6 M-cycles (single speed, 5 frozen): ~2 PPU T-cycles
         * - Remaining 131074 M-cycles (double speed): 131074 PPU T-cycles
         * Total: ~131076 PPU T-cycles ≈ 1.87 frames */
        ctx->speed_switch_halt = 131076;
        ctx->halted = 1;
        ctx->stopped = 1;  /* Force return from recompiled function */

        fprintf(stderr, "[GBRT] CGB speed switch: KEY1=0x%02X (double_speed=%d), halt=%d T-cycles\n",
                ctx->io[0x4D], (ctx->io[0x4D] >> 7) & 1, ctx->speed_switch_halt);
    } else {
        /* Normal STOP (not speed switch) */
        ctx->stopped = 1;
    }
}
bool gb_frame_complete(GBContext* ctx) { return ctx->frame_done != 0; }

void gb_set_platform_callbacks(GBContext* ctx, const GBPlatformCallbacks* c) {
    if (ctx && c) {
        ctx->callbacks = *c;
    }
}

void gb_audio_callback(GBContext* ctx, int16_t l, int16_t r) {
    if (ctx && ctx->callbacks.on_audio_sample) {
        ctx->callbacks.on_audio_sample(ctx, l, r);
    }
}
