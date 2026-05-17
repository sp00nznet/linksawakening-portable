/**
 * @file mp_session.h
 * @brief Multiplayer session manager
 *
 * Manages the multiplayer game session - creates/destroys GBContexts for
 * each player, routes input, runs frames, and handles world state sync.
 *
 * The host runs up to 4 GBContext instances (one per player).
 * Each context has its own WRAM (player state) but can share ERAM (save data).
 */

#ifndef MP_SESSION_H
#define MP_SESSION_H

#include <stdint.h>
#include <stdbool.h>
#include "mp_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct GBContext GBContext;

/* ============================================================================
 * Session State
 * ========================================================================== */

typedef enum {
    MP_STATE_IDLE,          /* Not in a multiplayer session */
    MP_STATE_HOSTING,       /* Hosting a game, waiting for / playing with clients */
    MP_STATE_CONNECTING,    /* Client: attempting to connect */
    MP_STATE_CONNECTED,     /* Client: connected and playing */
    MP_STATE_DISCONNECTED,  /* Was connected, now disconnected */
} MPSessionState;

/* Per-player info tracked by the session */
typedef struct {
    bool        active;
    char        name[MP_MAX_NAME_LEN];
    float       color_h, color_s, color_v;
    uint8_t     map_room;
    uint8_t     is_indoor;
    uint8_t     dungeon_idx;
    uint8_t     health;
    uint8_t     max_health;
    uint16_t    link_x, link_y;
    uint32_t    ping_ms;

    /* Host only: per-player game context */
    GBContext*  ctx;

    /* Host only: per-player input state */
    uint8_t     input_dpad;
    uint8_t     input_buttons;
} MPPlayer;

/* ============================================================================
 * Session API
 * ========================================================================== */

/** Initialize the session system */
void mp_session_init(void);

/** Shutdown and clean up */
void mp_session_shutdown(void);

/** Get current session state */
MPSessionState mp_session_get_state(void);

/**
 * Host a multiplayer game.
 * @param host_ctx The host's existing GBContext (player 0)
 * @param port     Port to listen on (0 = default)
 * @return true on success
 */
bool mp_session_host(GBContext* host_ctx, uint16_t port);

/**
 * Join a multiplayer game.
 * @param address  Host IP/hostname
 * @param port     Port (0 = default)
 * @param name     Player screen name
 * @param h,s,v    Link color in HSV
 * @return true if connection initiated
 */
bool mp_session_join(const char* address, uint16_t port,
                     const char* name, float h, float s, float v);

/** Stop hosting or disconnect from host */
void mp_session_leave(void);

/**
 * Per-frame update. Call once per frame from the main loop.
 * - Host: services network, runs frames for all player contexts
 * - Client: services network, sends input, receives frames
 */
void mp_session_update(void);

/** Get player info for slot (0-3). Returns NULL if slot is empty. */
const MPPlayer* mp_session_get_player(int slot);

/** Get the local player's slot index */
int mp_session_get_local_slot(void);

/** Get total number of active players */
int mp_session_get_player_count(void);

/**
 * Get the framebuffer for the local player.
 * - Host: returns host context framebuffer
 * - Client: returns received framebuffer from host
 */
const uint32_t* mp_session_get_framebuffer(void);

/** Check if we're currently in a multiplayer session */
bool mp_session_is_active(void);

/** Check if we're a connected client (should skip local game loop) */
bool mp_session_is_client_connected(void);

/* ============================================================================
 * Settings (persisted in config)
 * ========================================================================== */

/** Get/set the player's screen name */
const char* mp_session_get_name(void);
void mp_session_set_name(const char* name);

/** Get/set Link color (HSV) */
void mp_session_get_color(float* h, float* s, float* v);
void mp_session_set_color(float h, float s, float v);

/** Get/set debug overlay visibility */
bool mp_session_get_debug_overlay(void);
void mp_session_set_debug_overlay(bool visible);

/** Get/set minimum hearts for joining players (0 = use save data) */
int mp_session_get_min_hearts(void);
void mp_session_set_min_hearts(int hearts);

/** Starting equipment for joining players */
void mp_session_set_start_sword(int level);
void mp_session_set_start_shield(int level);
void mp_session_set_start_bracelet(int level);
void mp_session_set_start_boots(bool v);
void mp_session_set_start_flippers(bool v);
void mp_session_set_start_feather(bool v);
void mp_session_set_start_bow(bool v);
void mp_session_set_start_hookshot(bool v);
void mp_session_set_start_rod(bool v);
void mp_session_set_start_ocarina(bool v);
void mp_session_set_start_shovel(bool v);
void mp_session_set_start_rupees(int v);
void mp_session_set_start_bombs(int v);
void mp_session_set_start_arrows(int v);
void mp_session_set_start_powder(int v);
int  mp_session_get_start_sword(void);
int  mp_session_get_start_shield(void);
int  mp_session_get_start_bracelet(void);
bool mp_session_get_start_boots(void);
bool mp_session_get_start_flippers(void);
bool mp_session_get_start_feather(void);
bool mp_session_get_start_bow(void);
bool mp_session_get_start_hookshot(void);
bool mp_session_get_start_rod(void);
bool mp_session_get_start_ocarina(void);
bool mp_session_get_start_shovel(void);
int  mp_session_get_start_rupees(void);
int  mp_session_get_start_bombs(void);
int  mp_session_get_start_arrows(void);
int  mp_session_get_start_powder(void);

#ifdef __cplusplus
}
#endif

#endif /* MP_SESSION_H */
