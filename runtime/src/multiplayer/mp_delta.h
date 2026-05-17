/**
 * @file mp_delta.h
 * @brief Framebuffer delta encoding for bandwidth reduction
 *
 * Instead of sending full RLE-compressed frames every tick, we XOR the
 * current frame against the previous frame and RLE-compress the delta.
 * Game screens change very little frame-to-frame (mostly sprite movement),
 * so deltas are dramatically smaller.
 *
 * Protocol:
 * - Every N frames (or on scene change): send a full keyframe
 * - Between keyframes: send XOR deltas
 * - Client XORs delta against their last frame to reconstruct
 * - If client misses a delta, they request a keyframe
 *
 * Typical bandwidth savings: ~70-80% vs full RLE every frame.
 */

#ifndef MP_DELTA_H
#define MP_DELTA_H

#include <stdint.h>
#include <stdbool.h>
#include "mp_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Frame types */
#define MP_FRAME_KEYFRAME  0   /* Full frame (RLE compressed) */
#define MP_FRAME_DELTA     1   /* XOR delta (RLE compressed) */

/* Extended frame packet header */
typedef struct {
    MPPacketHeader header;
    uint32_t frame_num;
    uint32_t compressed_size;
    uint8_t  frame_type;       /* MP_FRAME_KEYFRAME or MP_FRAME_DELTA */
    uint8_t  _pad[3];
} MPDeltaFramePacket;

/* Delta encoder state (one per client on host) */
typedef struct {
    uint32_t prev_frame[MP_FRAME_PIXELS];  /* Last sent frame */
    bool     has_prev;                      /* false = must send keyframe */
    uint32_t frames_since_keyframe;
    uint32_t keyframe_interval;            /* Send keyframe every N frames */
} MPDeltaEncoder;

/* Delta decoder state (one per client) */
typedef struct {
    uint32_t prev_frame[MP_FRAME_PIXELS];  /* Last received frame */
    bool     has_prev;
    uint32_t last_frame_num;
} MPDeltaDecoder;

/* ============================================================================
 * Encoder API (Host side)
 * ========================================================================== */

/** Initialize an encoder. keyframe_interval = frames between forced keyframes. */
void mp_delta_encoder_init(MPDeltaEncoder* enc, uint32_t keyframe_interval);

/** Reset encoder (force next frame to be a keyframe) */
void mp_delta_encoder_reset(MPDeltaEncoder* enc);

/**
 * Encode a frame. Automatically chooses keyframe vs delta.
 *
 * @param enc           Encoder state
 * @param current_frame Current 160x144 ARGB framebuffer
 * @param out_buf       Output buffer (must be at least MP_FRAME_BYTES * 2)
 * @param out_capacity  Size of output buffer
 * @param out_type      Set to MP_FRAME_KEYFRAME or MP_FRAME_DELTA
 * @return              Compressed size in bytes (0 on failure)
 */
uint32_t mp_delta_encode(MPDeltaEncoder* enc,
                          const uint32_t* current_frame,
                          uint8_t* out_buf, uint32_t out_capacity,
                          uint8_t* out_type);

/* ============================================================================
 * Decoder API (Client side)
 * ========================================================================== */

/** Initialize a decoder */
void mp_delta_decoder_init(MPDeltaDecoder* dec);

/** Reset decoder (next frame must be a keyframe) */
void mp_delta_decoder_reset(MPDeltaDecoder* dec);

/**
 * Decode a received frame.
 *
 * @param dec           Decoder state
 * @param compressed    Compressed data
 * @param comp_size     Size of compressed data
 * @param frame_type    MP_FRAME_KEYFRAME or MP_FRAME_DELTA
 * @param out_frame     Output 160x144 ARGB framebuffer
 * @return              true on success
 */
bool mp_delta_decode(MPDeltaDecoder* dec,
                      const uint8_t* compressed, uint32_t comp_size,
                      uint8_t frame_type,
                      uint32_t* out_frame);

/** Check if decoder needs a keyframe (missed frames, no prev) */
bool mp_delta_needs_keyframe(const MPDeltaDecoder* dec);

#ifdef __cplusplus
}
#endif

#endif /* MP_DELTA_H */
