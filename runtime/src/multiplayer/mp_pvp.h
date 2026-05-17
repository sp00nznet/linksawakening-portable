/**
 * @file mp_pvp.h
 * @brief PvP Arena Mode
 *
 * A special multiplayer mode where players can damage each other.
 * The host selects a room as the arena, all players are teleported there,
 * and combat begins. Last Link standing wins.
 *
 * Rules:
 * - All players start with equal hearts
 * - Sword/item attacks damage other Links when in the same room
 * - Damage is calculated by detecting Link's attack state in one context
 *   and proximity to another player's Link position
 * - Knockback is simulated by manipulating WRAM position values
 * - Round ends when only one player has HP remaining
 * - Score tracked across rounds
 *
 * Arena rooms (suitable for PvP):
 * - Overworld open areas (Mabe Village square, beach, etc.)
 * - Dungeon boss rooms (large, open)
 * - Color Dungeon rooms
 */

#ifndef MP_PVP_H
#define MP_PVP_H

#include <stdint.h>
#include <stdbool.h>
#include "mp_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GBContext GBContext;

/* ============================================================================
 * PvP Configuration
 * ========================================================================== */

typedef struct {
    uint8_t  arena_room;       /* Room ID for the arena */
    uint8_t  arena_indoor;     /* 0=overworld, 1=indoor */
    uint8_t  starting_hearts;  /* Hearts per player (1-20, default 3) */
    uint8_t  rounds_to_win;    /* Rounds needed to win match (default 3) */
    bool     items_enabled;    /* Allow items (bombs, arrows, etc.) */
    bool     friendly_fire;    /* false = sword only, true = all damage */
    uint16_t respawn_delay;    /* Frames before respawn after death */
} MPPvPConfig;

/* Default arenas */
typedef struct {
    const char* name;
    uint8_t     room;
    uint8_t     indoor;
} MPArenaPreset;

/* ============================================================================
 * PvP State
 * ========================================================================== */

typedef enum {
    PVP_STATE_OFF,          /* PvP not active */
    PVP_STATE_LOBBY,        /* In lobby, waiting for players */
    PVP_STATE_COUNTDOWN,    /* 3-2-1 countdown before round */
    PVP_STATE_FIGHTING,     /* Round in progress */
    PVP_STATE_ROUND_END,    /* Round ended, showing results */
    PVP_STATE_MATCH_END,    /* Match ended, showing final results */
} MPPvPState;

typedef struct {
    uint8_t  kills;
    uint8_t  deaths;
    uint8_t  rounds_won;
    uint8_t  health;         /* Current health in arena */
    bool     alive;          /* Still alive this round */
    uint16_t respawn_timer;  /* Countdown to respawn */
} MPPvPPlayerStats;

/* WRAM addresses for Link's attack state */
#define LADX_LINK_ANIM_STATE   0xD103  /* Link animation state */
#define LADX_LINK_DIRECTION    0xD104  /* Link facing direction */
#define LADX_LINK_SWORD_STATE  0xD105  /* Sword swing state */

/* Attack detection: Link is attacking if sword state is active */
#define SWORD_STATE_IDLE    0x00
#define SWORD_STATE_SWING   0x01
#define SWORD_STATE_SPIN    0x02

/* Damage values (in 1/8 hearts) */
#define PVP_SWORD_DAMAGE    8   /* 1 heart */
#define PVP_SPIN_DAMAGE     16  /* 2 hearts */
#define PVP_BOMB_DAMAGE     16  /* 2 hearts */
#define PVP_ARROW_DAMAGE    8   /* 1 heart */

/* Hit detection radius (pixels) */
#define PVP_SWORD_RANGE     20
#define PVP_BOMB_RANGE      24
#define PVP_ARROW_RANGE     12

/* ============================================================================
 * PvP Packets (extend MPPacketType)
 * ========================================================================== */

#define MP_PKT_PVP_START   0x80
#define MP_PKT_PVP_HIT     0x81
#define MP_PKT_PVP_DEATH   0x82
#define MP_PKT_PVP_ROUND   0x83
#define MP_PKT_PVP_END     0x84

#pragma pack(push, 1)

typedef struct {
    MPPacketHeader header;
    MPPvPConfig config;
} MPPvPStartPacket;

typedef struct {
    MPPacketHeader header;
    uint8_t attacker_slot;
    uint8_t victim_slot;
    uint8_t damage;
    uint8_t attack_type;  /* 0=sword, 1=spin, 2=bomb, 3=arrow */
} MPPvPHitPacket;

typedef struct {
    MPPacketHeader header;
    uint8_t dead_slot;
    uint8_t killer_slot;
} MPPvPDeathPacket;

typedef struct {
    MPPacketHeader header;
    uint8_t round_number;
    uint8_t winner_slot;
    MPPvPPlayerStats stats[MP_MAX_PLAYERS];
} MPPvPRoundPacket;

#pragma pack(pop)

/* ============================================================================
 * PvP API
 * ========================================================================== */

/** Initialize PvP system */
void mp_pvp_init(void);

/** Get current PvP state */
MPPvPState mp_pvp_get_state(void);

/** Get PvP config */
const MPPvPConfig* mp_pvp_get_config(void);

/** Start a PvP match (host only) */
bool mp_pvp_start(const MPPvPConfig* config);

/** Stop the current PvP match */
void mp_pvp_stop(void);

/**
 * Per-frame PvP update. Must be called after all player frames are run.
 * Handles hit detection, damage, death, round transitions.
 * @param contexts  Array of GBContext pointers (one per player, NULL if empty)
 * @param count     Number of contexts
 */
void mp_pvp_update(GBContext** contexts, int count);

/** Get stats for a player */
const MPPvPPlayerStats* mp_pvp_get_stats(int slot);

/** Get the current round number */
int mp_pvp_get_round(void);

/** Get available arena presets */
const MPArenaPreset* mp_pvp_get_arenas(int* count);

/** Handle a received PvP packet */
void mp_pvp_handle_packet(const void* data, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif /* MP_PVP_H */
