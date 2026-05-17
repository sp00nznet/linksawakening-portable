/**
 * @file mp_worldsync.cpp
 * @brief World state synchronization implementation
 */

#include "mp_worldsync.h"
#include "mp_protocol.h"
#include <string.h>
#include <stdio.h>

extern "C" {
#include "gbrt.h"
}

/* ============================================================================
 * Complete Sync Address Table
 *
 * Every WRAM address that represents shared world state.
 * ========================================================================== */

const MPSyncAddr g_world_sync_table[] = {
    /* ---- Dungeon Items (current dungeon) ---- */
    { 0xDC74, "HasDungeonMap",         SYNC_CAT_DUNGEON_ITEMS, MERGE_OR },
    { 0xDC75, "HasDungeonCompass",     SYNC_CAT_DUNGEON_ITEMS, MERGE_OR },
    { 0xDC76, "HasDungeonStoneSlab",   SYNC_CAT_DUNGEON_ITEMS, MERGE_OR },
    { 0xDC77, "HasDungeonBossKey",     SYNC_CAT_DUNGEON_ITEMS, MERGE_OR },
    { 0xDC78, "SmallKeysCount",        SYNC_CAT_DUNGEON_ITEMS, MERGE_MAX },
    { 0xDBC2, "DungeonItemFlags",      SYNC_CAT_DUNGEON_ITEMS, MERGE_OR },

    /* ---- Nightmare Keys (per-dungeon boss keys) ---- */
    { 0xDBBD, "HasTailKey",            SYNC_CAT_DUNGEON_ITEMS, MERGE_OR },
    { 0xDBBE, "HasAnglerKey",          SYNC_CAT_DUNGEON_ITEMS, MERGE_OR },
    { 0xDBBF, "HasFaceKey",            SYNC_CAT_DUNGEON_ITEMS, MERGE_OR },
    { 0xDBC0, "HasBirdKey",            SYNC_CAT_DUNGEON_ITEMS, MERGE_OR },

    /* ---- Instruments (Wind Fish songs) ---- */
    { 0xDC0D, "HasInstrument1",        SYNC_CAT_INSTRUMENTS,   MERGE_OR },
    { 0xDC0E, "HasInstrument2",        SYNC_CAT_INSTRUMENTS,   MERGE_OR },
    { 0xDC0F, "HasInstrument3",        SYNC_CAT_INSTRUMENTS,   MERGE_OR },
    { 0xDC10, "HasInstrument4",        SYNC_CAT_INSTRUMENTS,   MERGE_OR },
    { 0xDC11, "HasInstrument5",        SYNC_CAT_INSTRUMENTS,   MERGE_OR },
    { 0xDC12, "HasInstrument6",        SYNC_CAT_INSTRUMENTS,   MERGE_OR },
    { 0xDC13, "HasInstrument7",        SYNC_CAT_INSTRUMENTS,   MERGE_OR },
    { 0xDC14, "HasInstrument8",        SYNC_CAT_INSTRUMENTS,   MERGE_OR },

    /* ---- Weapons & Upgrades ---- */
    { 0xDBFA, "SwordLevel",            SYNC_CAT_INVENTORY,     MERGE_MAX },
    { 0xDBF0, "ShieldLevel",           SYNC_CAT_INVENTORY,     MERGE_MAX },
    { 0xDBEF, "PowerBraceletLevel",    SYNC_CAT_INVENTORY,     MERGE_MAX },
    { 0xDBB8, "HasFlippers",           SYNC_CAT_INVENTORY,     MERGE_OR },
    { 0xDBB9, "HasMedicine",           SYNC_CAT_INVENTORY,     MERGE_OR },

    /* ---- Special Items & NPC Events ---- */
    { 0xDBF7, "HasToadstool",          SYNC_CAT_EVENTS,        MERGE_OR },
    { 0xDBF4, "TarinFlag",             SYNC_CAT_EVENTS,        MERGE_OR },
    { 0xDBFD, "RichardSpokenFlag",     SYNC_CAT_EVENTS,        MERGE_OR },
    { 0xDBBA, "TradeSequenceItem",     SYNC_CAT_EVENTS,        MERGE_MAX },
    { 0xDC25, "BoomerangTradedItem",   SYNC_CAT_EVENTS,        MERGE_OR },
    { 0xDBF5, "OcarinaSongFlags",      SYNC_CAT_EVENTS,        MERGE_OR },

    /* ---- Followers ---- */
    { 0xDBFE, "IsBowWowFollowing",     SYNC_CAT_EVENTS,        MERGE_HOST_WINS },
    { 0xDC1B, "IsMarinFollowing",      SYNC_CAT_EVENTS,        MERGE_HOST_WINS },
    { 0xDC1C, "IsMarinInAnimalVillage",SYNC_CAT_EVENTS,        MERGE_HOST_WINS },
    { 0xDC21, "IsGhostFollowing",      SYNC_CAT_EVENTS,        MERGE_HOST_WINS },
    { 0xDC23, "IsRoosterFollowing",    SYNC_CAT_EVENTS,        MERGE_HOST_WINS },

    /* ---- Collectibles ---- */
    { 0xDBBB, "SeashellsCount",        SYNC_CAT_COLLECTIBLES,  MERGE_MAX },
    { 0xDBC1, "GoldenLeavesCount",     SYNC_CAT_COLLECTIBLES,  MERGE_MAX },
    { 0xDC04, "HeartPiecesCount",      SYNC_CAT_COLLECTIBLES,  MERGE_MAX },

    /* ---- Boss Defeats ---- */
    { 0xD46C, "BossDefeated",          SYNC_CAT_BOSSES,        MERGE_OR },

    /* ---- Shop ---- */
    { 0xDBF2, "HasStolenFromShop",     SYNC_CAT_EVENTS,        MERGE_OR },
    { 0xDC16, "IsThief",               SYNC_CAT_EVENTS,        MERGE_OR },

    /* ---- Color Dungeon ---- */
    { 0xDE81, "ColorDungeonTombstones",SYNC_CAT_EVENTS,        MERGE_OR },
    { 0xDE82, "ColorDungeonItemFlags", SYNC_CAT_EVENTS,        MERGE_OR },

    /* ---- Dungeon-specific ---- */
    { 0xDC1A, "D7PillarsDestroyed",   SYNC_CAT_DUNGEON_ITEMS, MERGE_MAX },
    { 0xDC24, "EggMazeProgress",       SYNC_CAT_EVENTS,        MERGE_MAX },
};

const int g_world_sync_table_size =
    sizeof(g_world_sync_table) / sizeof(g_world_sync_table[0]);

/* ============================================================================
 * Snapshot State (for dirty tracking)
 * ========================================================================== */

/* Snapshot of individual sync addresses */
static uint8_t g_snapshot_values[sizeof(g_world_sync_table) / sizeof(g_world_sync_table[0])];

/* Snapshot of room status blocks */
static uint8_t g_snapshot_ow_rooms[SYNC_OW_ROOM_COUNT];
static uint8_t g_snapshot_indoor_a[SYNC_INDOOR_A_COUNT];
static uint8_t g_snapshot_indoor_b[SYNC_INDOOR_B_COUNT];

static bool g_has_snapshot = false;

/* ============================================================================
 * WRAM Helpers
 * ========================================================================== */

static uint8_t read_wram(GBContext* ctx, uint16_t addr) {
    if (!ctx || !ctx->wram) return 0;
    if (addr >= 0xC000 && addr < 0xE000)
        return ctx->wram[addr - 0xC000];
    return 0;
}

static void write_wram(GBContext* ctx, uint16_t addr, uint8_t value) {
    if (!ctx || !ctx->wram) return;
    if (addr >= 0xC000 && addr < 0xE000)
        ctx->wram[addr - 0xC000] = value;
}

/* ============================================================================
 * Implementation
 * ========================================================================== */

void mp_worldsync_init(void) {
    memset(g_snapshot_values, 0, sizeof(g_snapshot_values));
    memset(g_snapshot_ow_rooms, 0, sizeof(g_snapshot_ow_rooms));
    memset(g_snapshot_indoor_a, 0, sizeof(g_snapshot_indoor_a));
    memset(g_snapshot_indoor_b, 0, sizeof(g_snapshot_indoor_b));
    g_has_snapshot = false;
}

void mp_worldsync_snapshot(GBContext* ctx) {
    if (!ctx) return;

    /* Snapshot individual addresses */
    for (int i = 0; i < g_world_sync_table_size; i++) {
        g_snapshot_values[i] = read_wram(ctx, g_world_sync_table[i].addr);
    }

    /* Snapshot room status blocks */
    for (int i = 0; i < SYNC_OW_ROOM_COUNT; i++)
        g_snapshot_ow_rooms[i] = read_wram(ctx, SYNC_OW_ROOM_BASE + i);

    for (int i = 0; i < SYNC_INDOOR_A_COUNT; i++)
        g_snapshot_indoor_a[i] = read_wram(ctx, SYNC_INDOOR_A_BASE + i);

    for (int i = 0; i < SYNC_INDOOR_B_COUNT; i++)
        g_snapshot_indoor_b[i] = read_wram(ctx, SYNC_INDOOR_B_BASE + i);

    g_has_snapshot = true;
}

uint32_t mp_worldsync_build_delta(GBContext* ctx, uint8_t* out_buf,
                                   uint32_t buf_size, uint16_t* out_count)
{
    if (!ctx || !out_buf || !out_count) return 0;

    uint32_t written = 0;
    uint16_t count = 0;
    MPSyncEntry* entries = (MPSyncEntry*)out_buf;
    uint32_t max_entries = buf_size / sizeof(MPSyncEntry);

    /* Check individual addresses */
    for (int i = 0; i < g_world_sync_table_size && count < max_entries; i++) {
        uint8_t current = read_wram(ctx, g_world_sync_table[i].addr);

        if (!g_has_snapshot || current != g_snapshot_values[i]) {
            entries[count].addr = g_world_sync_table[i].addr;
            entries[count].value = current;
            entries[count]._pad = 0;
            g_snapshot_values[i] = current;
            count++;
        }
    }

    /* Check overworld room status */
    for (int i = 0; i < SYNC_OW_ROOM_COUNT && count < max_entries; i++) {
        uint8_t current = read_wram(ctx, SYNC_OW_ROOM_BASE + i);
        if (!g_has_snapshot || current != g_snapshot_ow_rooms[i]) {
            entries[count].addr = SYNC_OW_ROOM_BASE + i;
            entries[count].value = current;
            entries[count]._pad = 0;
            g_snapshot_ow_rooms[i] = current;
            count++;
        }
    }

    /* Check indoor A rooms */
    for (int i = 0; i < SYNC_INDOOR_A_COUNT && count < max_entries; i++) {
        uint8_t current = read_wram(ctx, SYNC_INDOOR_A_BASE + i);
        if (!g_has_snapshot || current != g_snapshot_indoor_a[i]) {
            entries[count].addr = SYNC_INDOOR_A_BASE + i;
            entries[count].value = current;
            entries[count]._pad = 0;
            g_snapshot_indoor_a[i] = current;
            count++;
        }
    }

    /* Check indoor B rooms */
    for (int i = 0; i < SYNC_INDOOR_B_COUNT && count < max_entries; i++) {
        uint8_t current = read_wram(ctx, SYNC_INDOOR_B_BASE + i);
        if (!g_has_snapshot || current != g_snapshot_indoor_b[i]) {
            entries[count].addr = SYNC_INDOOR_B_BASE + i;
            entries[count].value = current;
            entries[count]._pad = 0;
            g_snapshot_indoor_b[i] = current;
            count++;
        }
    }

    g_has_snapshot = true;
    *out_count = count;
    return count * sizeof(MPSyncEntry);
}

void mp_worldsync_apply(GBContext* ctx, const void* entries_data, uint16_t count) {
    if (!ctx || !entries_data || count == 0) return;

    const MPSyncEntry* entries = (const MPSyncEntry*)entries_data;

    for (uint16_t i = 0; i < count; i++) {
        uint16_t addr = entries[i].addr;
        uint8_t  new_val = entries[i].value;

        /* Determine merge mode */
        uint8_t merge = MERGE_HOST_WINS;

        /* Look up in sync table for merge mode */
        for (int t = 0; t < g_world_sync_table_size; t++) {
            if (g_world_sync_table[t].addr == addr) {
                merge = g_world_sync_table[t].merge_mode;
                break;
            }
        }

        /* Room status ranges always use OR merge */
        if ((addr >= SYNC_OW_ROOM_BASE && addr < SYNC_OW_ROOM_BASE + SYNC_OW_ROOM_COUNT) ||
            (addr >= SYNC_INDOOR_A_BASE && addr < SYNC_INDOOR_A_BASE + SYNC_INDOOR_A_COUNT) ||
            (addr >= SYNC_INDOOR_B_BASE && addr < SYNC_INDOOR_B_BASE + SYNC_INDOOR_B_COUNT)) {
            merge = MERGE_OR;
        }

        uint8_t old_val = read_wram(ctx, addr);
        uint8_t final_val;

        switch (merge) {
        case MERGE_OR:
            final_val = old_val | new_val;
            break;
        case MERGE_MAX:
            final_val = (new_val > old_val) ? new_val : old_val;
            break;
        case MERGE_HOST_WINS:
        default:
            final_val = new_val;
            break;
        }

        write_wram(ctx, addr, final_val);
    }
}

void mp_worldsync_full_copy(GBContext* src, GBContext* dst) {
    if (!src || !dst) return;

    /* Copy all sync table addresses */
    for (int i = 0; i < g_world_sync_table_size; i++) {
        write_wram(dst, g_world_sync_table[i].addr,
                   read_wram(src, g_world_sync_table[i].addr));
    }

    /* Copy room status blocks */
    for (int i = 0; i < SYNC_OW_ROOM_COUNT; i++)
        write_wram(dst, SYNC_OW_ROOM_BASE + i,
                   read_wram(src, SYNC_OW_ROOM_BASE + i));

    for (int i = 0; i < SYNC_INDOOR_A_COUNT; i++)
        write_wram(dst, SYNC_INDOOR_A_BASE + i,
                   read_wram(src, SYNC_INDOOR_A_BASE + i));

    for (int i = 0; i < SYNC_INDOOR_B_COUNT; i++)
        write_wram(dst, SYNC_INDOOR_B_BASE + i,
                   read_wram(src, SYNC_INDOOR_B_BASE + i));

    fprintf(stderr, "[WORLDSYNC] Full copy: %d addresses + %d room bytes\n",
            g_world_sync_table_size,
            SYNC_OW_ROOM_COUNT + SYNC_INDOOR_A_COUNT + SYNC_INDOOR_B_COUNT);
}

/* ============================================================================
 * Deferred OR-Merge After All Frames
 *
 * Called once per frame AFTER all gb_run_frame() calls return.
 * No recompiled code is executing, so WRAM is safe to modify.
 * ========================================================================== */

/* Gameplay type values for transition safety */
#define GAMEPLAY_OVERWORLD   0x01
#define GAMEPLAY_INDOOR      0x02
#define GAMEPLAY_SIDESCROLL  0x0B
#define GAMEPLAY_TRANSITION  0x07
#define GAMEPLAY_SEQUENCE    0x08

/* LADX WRAM address for gameplay type */
#define ADDR_GAMEPLAY_TYPE   0xDB95

static bool is_safe_to_write(GBContext* ctx) {
    uint8_t gtype = read_wram(ctx, ADDR_GAMEPLAY_TYPE);
    return gtype == GAMEPLAY_OVERWORLD ||
           gtype == GAMEPLAY_INDOOR ||
           gtype == GAMEPLAY_SIDESCROLL;
}

void mp_worldsync_merge_all(GBContext** contexts, int count) {
    if (!contexts || count < 2) return;

    /* ---- Merge individual sync table addresses only ----
     * Room flag blocks (0xD8B5-0xDCB4) are NOT synced here because
     * the game uses those ranges as working scratch during room
     * transitions/loads. Writing to them mid-game corrupts transient
     * state and causes VRAM jumps. The discrete addresses in the
     * sync table (boss flags, instruments, items, etc.) are stable
     * and safe to merge at any time. */
    for (int t = 0; t < g_world_sync_table_size; t++) {
        uint16_t addr = g_world_sync_table[t].addr;
        uint8_t  mode = g_world_sync_table[t].merge_mode;

        /* Read from all contexts and compute merged value */
        uint8_t merged = read_wram(contexts[0], addr);

        for (int c = 1; c < count; c++) {
            if (!contexts[c]) continue;
            uint8_t val = read_wram(contexts[c], addr);

            switch (mode) {
            case MERGE_OR:
                merged |= val;
                break;
            case MERGE_MAX:
                if (val > merged) merged = val;
                break;
            case MERGE_HOST_WINS:
                /* Host value (contexts[0]) always wins — don't merge */
                break;
            }
        }

        /* Write merged value back to any context that's behind */
        for (int c = 0; c < count; c++) {
            if (!contexts[c]) continue;
            uint8_t cur = read_wram(contexts[c], addr);
            if (cur == merged) continue;

            /* For HOST_WINS: only write host value to non-host contexts */
            if (mode == MERGE_HOST_WINS && c == 0) continue;

            /* Skip contexts mid-transition */
            if (!is_safe_to_write(contexts[c])) continue;

            write_wram(contexts[c], addr, merged);
        }
    }
}

void mp_worldsync_merge_to_host(GBContext* host, GBContext* player) {
    if (!host || !player) return;

    for (int i = 0; i < g_world_sync_table_size; i++) {
        uint8_t host_val = read_wram(host, g_world_sync_table[i].addr);
        uint8_t player_val = read_wram(player, g_world_sync_table[i].addr);

        if (host_val == player_val) continue;

        uint8_t merged;
        switch (g_world_sync_table[i].merge_mode) {
        case MERGE_OR:
            merged = host_val | player_val;
            break;
        case MERGE_MAX:
            merged = (player_val > host_val) ? player_val : host_val;
            break;
        case MERGE_HOST_WINS:
        default:
            continue; /* Host wins = don't overwrite host */
        }

        if (merged != host_val) {
            write_wram(host, g_world_sync_table[i].addr, merged);
        }
    }

    /* Merge room flags (OR merge - once opened, stays opened) */
    for (int i = 0; i < SYNC_OW_ROOM_COUNT; i++) {
        uint16_t addr = SYNC_OW_ROOM_BASE + i;
        uint8_t h = read_wram(host, addr);
        uint8_t p = read_wram(player, addr);
        if (p & ~h) write_wram(host, addr, h | p);
    }

    for (int i = 0; i < SYNC_INDOOR_A_COUNT; i++) {
        uint16_t addr = SYNC_INDOOR_A_BASE + i;
        uint8_t h = read_wram(host, addr);
        uint8_t p = read_wram(player, addr);
        if (p & ~h) write_wram(host, addr, h | p);
    }

    for (int i = 0; i < SYNC_INDOOR_B_COUNT; i++) {
        uint16_t addr = SYNC_INDOOR_B_BASE + i;
        uint8_t h = read_wram(host, addr);
        uint8_t p = read_wram(player, addr);
        if (p & ~h) write_wram(host, addr, h | p);
    }
}
