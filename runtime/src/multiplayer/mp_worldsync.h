/**
 * @file mp_worldsync.h
 * @brief Comprehensive world state synchronization for LADX multiplayer
 *
 * Defines all WRAM addresses that represent shared world state and need
 * to be synchronized across all player contexts. When any player opens
 * a chest, gets a key, defeats a boss, or triggers an event, all other
 * players see the change reflected in their game world.
 *
 * Sync strategy:
 * - Host is authoritative for world state
 * - Every N frames, host diffs current state against last-sent snapshot
 * - Only changed entries are sent (dirty flag optimization)
 * - Clients apply received state to their local view
 * - Per-player state (health, position) is NOT synced here (see mp_session)
 */

#ifndef MP_WORLDSYNC_H
#define MP_WORLDSYNC_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GBContext GBContext;

/* ============================================================================
 * Sync Categories
 * ========================================================================== */

typedef enum {
    SYNC_CAT_DUNGEON_ITEMS,   /* Maps, compasses, keys, boss keys */
    SYNC_CAT_INSTRUMENTS,     /* 8 wind fish instruments */
    SYNC_CAT_INVENTORY,       /* Swords, shields, items */
    SYNC_CAT_OVERWORLD,       /* Overworld room flags (128 bytes) */
    SYNC_CAT_DUNGEON_A,       /* Indoor A room flags (256 bytes) */
    SYNC_CAT_DUNGEON_B,       /* Indoor B room flags (256 bytes) */
    SYNC_CAT_EVENTS,          /* NPC flags, trade sequence, followers */
    SYNC_CAT_BOSSES,          /* Boss defeated flags */
    SYNC_CAT_COLLECTIBLES,    /* Seashells, golden leaves, heart pieces */
    SYNC_CAT_COUNT
} MPSyncCategory;

/* ============================================================================
 * Sync Address Entry
 * ========================================================================== */

typedef struct {
    uint16_t       addr;       /* WRAM address (0xC000-0xDFFF) */
    const char*    name;       /* Human-readable name (for debug) */
    MPSyncCategory category;
    uint8_t        merge_mode; /* 0=host_wins, 1=OR_merge, 2=MAX_merge */
} MPSyncAddr;

/* Merge modes:
 * - HOST_WINS:  Host value always overwrites client
 * - OR_MERGE:   Bitwise OR (used for flags - once set, stays set)
 * - MAX_MERGE:  Take max value (used for counts that only go up)
 */
#define MERGE_HOST_WINS 0
#define MERGE_OR        1
#define MERGE_MAX       2

/* ============================================================================
 * Complete Sync Address Table
 * ========================================================================== */

/* Defined in mp_worldsync.cpp */
extern const MPSyncAddr g_world_sync_table[];
extern const int         g_world_sync_table_size;

/* Overworld/dungeon room status ranges (synced as blocks) */
#define SYNC_OW_ROOM_BASE    0xD8B5   /* 128 bytes: overworld room flags */
#define SYNC_OW_ROOM_COUNT   128
#define SYNC_INDOOR_A_BASE   0xD9B5   /* 256 bytes: indoor A room flags */
#define SYNC_INDOOR_A_COUNT  256
#define SYNC_INDOOR_B_BASE   0xDAB5   /* 256 bytes: indoor B room flags */
#define SYNC_INDOOR_B_COUNT  256

/* ============================================================================
 * World Sync Engine
 * ========================================================================== */

/** Initialize the world sync engine */
void mp_worldsync_init(void);

/** Take a snapshot of current world state from a context.
 *  Used to detect changes (dirty tracking). */
void mp_worldsync_snapshot(GBContext* ctx);

/**
 * Build a sync packet with only changed entries since last snapshot.
 * @param ctx       Source context to read current state from
 * @param out_buf   Buffer to write sync entries into
 * @param buf_size  Size of output buffer
 * @param out_count Number of changed entries written
 * @return          Total bytes written to out_buf
 */
uint32_t mp_worldsync_build_delta(GBContext* ctx, uint8_t* out_buf,
                                   uint32_t buf_size, uint16_t* out_count);

/**
 * Apply received sync entries to a context.
 * Uses merge_mode to resolve conflicts.
 * @param ctx       Target context to write into
 * @param entries   Array of sync entries from network
 * @param count     Number of entries
 */
void mp_worldsync_apply(GBContext* ctx, const void* entries, uint16_t count);

/**
 * Full sync: copy ALL world state from src to dst context.
 * Used when a new player joins to get them up to speed.
 */
void mp_worldsync_full_copy(GBContext* src, GBContext* dst);

/**
 * Merge world state from a player context back into the host.
 * Called when a player does something that affects the world
 * (opens chest, defeats boss, etc.) - host picks up the change.
 */
void mp_worldsync_merge_to_host(GBContext* host, GBContext* player);

/**
 * Deferred OR/MAX merge across ALL active player contexts.
 * Must be called AFTER all gb_run_frame() calls have returned
 * (no recompiled code executing). Reads all sync addresses from
 * every context, merges using OR/MAX/HOST_WINS, then writes
 * merged values back to any context that's behind.
 * Skips writing to contexts mid-room-transition for safety.
 *
 * @param contexts  Array of GBContext pointers (index 0 = host)
 * @param count     Number of contexts in the array
 */
void mp_worldsync_merge_all(GBContext** contexts, int count);

#ifdef __cplusplus
}
#endif

#endif /* MP_WORLDSYNC_H */
