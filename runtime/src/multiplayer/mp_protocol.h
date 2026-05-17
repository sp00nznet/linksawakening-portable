/**
 * @file mp_protocol.h
 * @brief Multiplayer wire protocol - packet types and serialization
 *
 * All packets are sent via ENet reliable or unreliable channels.
 * Channel 0: Reliable (join/leave/sync)
 * Channel 1: Unreliable (input/frame data)
 */

#ifndef MP_PROTOCOL_H
#define MP_PROTOCOL_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ========================================================================== */

#define MP_DEFAULT_PORT     21384   /* "LA DX 4P" */
#define MP_MAX_PLAYERS      4
#define MP_MAX_NAME_LEN     32
#define MP_PROTOCOL_VERSION 1

#define MP_CHANNEL_RELIABLE   0
#define MP_CHANNEL_UNRELIABLE 1
#define MP_CHANNEL_COUNT      2

/* GB screen dimensions */
#define MP_SCREEN_W 160
#define MP_SCREEN_H 144
#define MP_FRAME_PIXELS (MP_SCREEN_W * MP_SCREEN_H)
#define MP_FRAME_BYTES  (MP_FRAME_PIXELS * sizeof(uint32_t))  /* ARGB8888 */

/* World sync: number of shared WRAM addresses to synchronize */
#define MP_SYNC_ADDR_COUNT 64

/* ============================================================================
 * Packet Types
 * ========================================================================== */

typedef enum {
    MP_PKT_JOIN_REQ      = 0x01,  /* Client -> Host: request to join */
    MP_PKT_JOIN_ACK      = 0x02,  /* Host -> Client: accepted, here's your slot */
    MP_PKT_JOIN_DENY     = 0x03,  /* Host -> Client: rejected (full, version, etc.) */
    MP_PKT_INPUT         = 0x10,  /* Client -> Host: joypad state */
    MP_PKT_FRAME         = 0x20,  /* Host -> Client: rendered framebuffer */
    MP_PKT_WORLD_SYNC    = 0x30,  /* Host -> All: shared world state */
    MP_PKT_PLAYER_INFO   = 0x40,  /* Host -> All: all player positions/status */
    MP_PKT_CHAT          = 0x50,  /* Any -> All: text chat */
    MP_PKT_DISCONNECT    = 0xFF,  /* Any: graceful disconnect */
} MPPacketType;

/* ============================================================================
 * Packet Header (common to all packets)
 * ========================================================================== */

#pragma pack(push, 1)

typedef struct {
    uint8_t  type;           /* MPPacketType */
    uint8_t  version;        /* MP_PROTOCOL_VERSION */
    uint8_t  player_id;      /* Sender's player slot (0-3, or 0xFF for unassigned) */
    uint8_t  _pad;
    uint32_t sequence;       /* Packet sequence number */
} MPPacketHeader;

/* ============================================================================
 * Join Request (Client -> Host)
 * ========================================================================== */

typedef struct {
    MPPacketHeader header;
    char     name[MP_MAX_NAME_LEN];  /* Player screen name */
    float    color_h;                /* Link color hue (0-360) */
    float    color_s;                /* Link color saturation (0-1) */
    float    color_v;                /* Link color value/brightness (0-1) */
    uint32_t save_hash;              /* Hash of player's save data */
} MPJoinReq;

/* ============================================================================
 * Join Acknowledge (Host -> Client)
 * ========================================================================== */

typedef struct {
    MPPacketHeader header;
    uint8_t  assigned_slot;   /* Player slot 0-3 */
    uint8_t  total_players;   /* How many players are currently in */
    uint8_t  _pad[2];
} MPJoinAck;

/* ============================================================================
 * Join Deny (Host -> Client)
 * ========================================================================== */

typedef struct {
    MPPacketHeader header;
    uint8_t  reason;          /* 0=full, 1=version mismatch, 2=banned */
    char     message[64];     /* Human-readable reason */
} MPJoinDeny;

/* ============================================================================
 * Input Packet (Client -> Host, every frame)
 * ========================================================================== */

typedef struct {
    MPPacketHeader header;
    uint8_t  joypad_dpad;     /* D-pad state (active low) */
    uint8_t  joypad_buttons;  /* Button state (active low) */
    uint8_t  _pad[2];
    uint32_t frame_num;       /* Client's frame counter for lag compensation */
} MPInputPacket;

/* ============================================================================
 * Frame Packet (Host -> Client, every frame)
 *
 * The framebuffer is 160x144 ARGB8888 = 92,160 bytes uncompressed.
 * We use simple RLE compression to reduce bandwidth.
 * ========================================================================== */

typedef struct {
    MPPacketHeader header;
    uint32_t frame_num;           /* Host frame counter */
    uint32_t compressed_size;     /* Size of compressed framebuffer data */
    /* Followed by `compressed_size` bytes of RLE-compressed framebuffer */
} MPFramePacket;

/* ============================================================================
 * World Sync (Host -> All clients, periodic)
 *
 * Synchronizes shared game state: chests opened, keys collected,
 * dungeon progress, NPC states, etc.
 * ========================================================================== */

typedef struct {
    uint16_t addr;    /* WRAM address */
    uint8_t  value;   /* Current value */
    uint8_t  _pad;
} MPSyncEntry;

typedef struct {
    MPPacketHeader header;
    uint32_t frame_num;
    uint16_t entry_count;
    uint8_t  _pad[2];
    /* Followed by entry_count MPSyncEntry structs */
} MPWorldSync;

/* ============================================================================
 * Player Info (Host -> All, periodic)
 *
 * Broadcasts all player positions and states so clients can render
 * indicators of where other players are.
 * ========================================================================== */

typedef struct {
    uint8_t  slot;                    /* Player slot 0-3 */
    uint8_t  connected;               /* 1 if connected */
    char     name[MP_MAX_NAME_LEN];   /* Screen name */
    float    color_h, color_s, color_v; /* Link color */
    uint8_t  map_room;                /* Current room ID */
    uint8_t  is_indoor;               /* Indoor/overworld flag */
    uint8_t  dungeon_idx;             /* Current dungeon */
    uint8_t  health;                  /* Current health */
    uint8_t  max_health;              /* Max health */
    uint16_t link_x;                  /* Link X position (subpixel) */
    uint16_t link_y;                  /* Link Y position (subpixel) */
    uint32_t ping_ms;                 /* Round-trip latency */
} MPPlayerState;

typedef struct {
    MPPacketHeader header;
    uint32_t frame_num;
    MPPlayerState players[MP_MAX_PLAYERS];
} MPPlayerInfo;

/* ============================================================================
 * Chat (Any -> All)
 * ========================================================================== */

typedef struct {
    MPPacketHeader header;
    char message[128];
} MPChatPacket;

#pragma pack(pop)

/* ============================================================================
 * Framebuffer RLE Compression
 *
 * Simple run-length encoding for ARGB8888 framebuffers.
 * Format: [count:uint16][pixel:uint32] repeating.
 * Worst case: 1.5x original size (all unique pixels).
 * Best case: ~1 KB (solid color screen).
 * Typical game frame: ~15-30 KB compressed.
 * ========================================================================== */

static inline uint32_t mp_rle_compress(const uint32_t* src, uint32_t pixel_count,
                                        uint8_t* dst, uint32_t dst_capacity)
{
    uint32_t out = 0;
    uint32_t i = 0;

    while (i < pixel_count) {
        uint32_t pixel = src[i];
        uint16_t run = 1;
        while (i + run < pixel_count && run < 65535 && src[i + run] == pixel)
            run++;

        if (out + 6 > dst_capacity) return 0; /* overflow */

        memcpy(dst + out, &run, 2);    out += 2;
        memcpy(dst + out, &pixel, 4);  out += 4;
        i += run;
    }
    return out;
}

static inline int mp_rle_decompress(const uint8_t* src, uint32_t src_size,
                                     uint32_t* dst, uint32_t pixel_capacity)
{
    uint32_t in = 0;
    uint32_t out = 0;

    while (in + 6 <= src_size) {
        uint16_t run;
        uint32_t pixel;
        memcpy(&run, src + in, 2);    in += 2;
        memcpy(&pixel, src + in, 4);  in += 4;

        if (out + run > pixel_capacity) return -1; /* overflow */

        for (uint16_t j = 0; j < run; j++)
            dst[out++] = pixel;
    }
    return (int)out;
}

#ifdef __cplusplus
}
#endif

#endif /* MP_PROTOCOL_H */
