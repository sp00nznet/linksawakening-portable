/**
 * @file mp_voice.h
 * @brief Voice chat for multiplayer
 *
 * Simple voice chat using SDL2 audio capture. Audio is captured from the
 * microphone, compressed with a simple codec (mu-law), and sent to other
 * players via the network.
 *
 * Design:
 * - Push-to-talk (hold V key) or voice activation
 * - SDL2 audio capture device for microphone input
 * - mu-law compression (8:1 ratio, good enough for voice)
 * - Mono 8kHz for minimal bandwidth (~8 KB/s per speaker)
 * - Mixing: sum all incoming voice streams, clamp
 * - Playback through a separate SDL audio stream (not the game audio)
 */

#ifndef MP_VOICE_H
#define MP_VOICE_H

#include <stdint.h>
#include <stdbool.h>
#include "mp_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Voice chat constants */
#define VOICE_SAMPLE_RATE   8000    /* 8kHz mono */
#define VOICE_FRAME_MS      20      /* 20ms per voice frame */
#define VOICE_FRAME_SAMPLES (VOICE_SAMPLE_RATE * VOICE_FRAME_MS / 1000)  /* 160 samples */
#define VOICE_FRAME_BYTES   VOICE_FRAME_SAMPLES  /* mu-law = 1 byte per sample */

/* Voice packet (extend MPPacketType) */
#define MP_PKT_VOICE 0x70

#pragma pack(push, 1)
typedef struct {
    MPPacketHeader header;
    uint16_t sample_count;    /* Number of mu-law samples */
    uint8_t  _pad[2];
    /* Followed by sample_count bytes of mu-law audio */
} MPVoicePacket;
#pragma pack(pop)

/* ============================================================================
 * Voice Mode
 * ========================================================================== */

typedef enum {
    VOICE_MODE_OFF,           /* Voice chat disabled */
    VOICE_MODE_PTT,           /* Push-to-talk (hold key) */
    VOICE_MODE_ACTIVE,        /* Voice activation (auto-detect speech) */
    VOICE_MODE_ALWAYS,        /* Always transmitting */
} MPVoiceMode;

/* ============================================================================
 * Voice API
 * ========================================================================== */

/** Initialize voice chat (opens capture device) */
bool mp_voice_init(void);

/** Shutdown voice chat */
void mp_voice_shutdown(void);

/** Set voice mode */
void mp_voice_set_mode(MPVoiceMode mode);
MPVoiceMode mp_voice_get_mode(void);

/** Set PTT key state (call when key is held/released) */
void mp_voice_set_ptt(bool active);

/** Set voice activation threshold (0.0 - 1.0) */
void mp_voice_set_vox_threshold(float threshold);

/** Set transmit/receive volume (0.0 - 1.0) */
void mp_voice_set_tx_volume(float vol);
void mp_voice_set_rx_volume(float vol);

/**
 * Per-frame update. Captures audio, builds packets.
 * @param out_packet  Buffer for outgoing voice packet (at least 256 bytes)
 * @param out_size    Set to size of packet to send (0 if nothing to send)
 */
void mp_voice_update(uint8_t* out_packet, uint32_t* out_size);

/**
 * Receive a voice packet from another player.
 * Decoded audio is mixed into the playback buffer.
 */
void mp_voice_receive(int from_slot, const void* data, uint32_t size);

/** Check if currently transmitting */
bool mp_voice_is_transmitting(void);

/** Get input level (0.0 - 1.0) for UI meter */
float mp_voice_get_input_level(void);

/** Mute/unmute a specific player's voice */
void mp_voice_mute_player(int slot, bool muted);
bool mp_voice_is_player_muted(int slot);

#ifdef __cplusplus
}
#endif

#endif /* MP_VOICE_H */
