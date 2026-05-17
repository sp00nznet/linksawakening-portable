/**
 * @file mp_delta.cpp
 * @brief Framebuffer delta encoding implementation
 */

#include "mp_delta.h"
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * Encoder
 * ========================================================================== */

void mp_delta_encoder_init(MPDeltaEncoder* enc, uint32_t keyframe_interval) {
    if (!enc) return;
    memset(enc, 0, sizeof(*enc));
    enc->keyframe_interval = keyframe_interval > 0 ? keyframe_interval : 60;
}

void mp_delta_encoder_reset(MPDeltaEncoder* enc) {
    if (!enc) return;
    enc->has_prev = false;
    enc->frames_since_keyframe = 0;
}

uint32_t mp_delta_encode(MPDeltaEncoder* enc,
                          const uint32_t* current_frame,
                          uint8_t* out_buf, uint32_t out_capacity,
                          uint8_t* out_type)
{
    if (!enc || !current_frame || !out_buf || !out_type) return 0;

    bool force_keyframe = !enc->has_prev ||
                          enc->frames_since_keyframe >= enc->keyframe_interval;

    if (force_keyframe) {
        /* ---- Keyframe: RLE compress the full frame ---- */
        *out_type = MP_FRAME_KEYFRAME;

        uint32_t comp_size = mp_rle_compress(current_frame, MP_FRAME_PIXELS,
                                              out_buf, out_capacity);

        /* Save as reference for future deltas */
        memcpy(enc->prev_frame, current_frame, MP_FRAME_BYTES);
        enc->has_prev = true;
        enc->frames_since_keyframe = 0;

        return comp_size;

    } else {
        /* ---- Delta: XOR against previous, then RLE compress ---- */
        *out_type = MP_FRAME_DELTA;

        /* Compute XOR delta into a temp buffer on the stack.
         * Most pixels will be 0 (unchanged), which RLE compresses very well. */
        static uint32_t delta[MP_FRAME_PIXELS]; /* ~360KB - static to avoid stack overflow */

        uint32_t changed_pixels = 0;
        for (uint32_t i = 0; i < MP_FRAME_PIXELS; i++) {
            delta[i] = current_frame[i] ^ enc->prev_frame[i];
            if (delta[i] != 0) changed_pixels++;
        }

        /* If too many pixels changed (>50%), send keyframe instead */
        if (changed_pixels > MP_FRAME_PIXELS / 2) {
            *out_type = MP_FRAME_KEYFRAME;
            uint32_t comp_size = mp_rle_compress(current_frame, MP_FRAME_PIXELS,
                                                  out_buf, out_capacity);
            memcpy(enc->prev_frame, current_frame, MP_FRAME_BYTES);
            enc->frames_since_keyframe = 0;
            return comp_size;
        }

        uint32_t comp_size = mp_rle_compress(delta, MP_FRAME_PIXELS,
                                              out_buf, out_capacity);

        /* Update reference */
        memcpy(enc->prev_frame, current_frame, MP_FRAME_BYTES);
        enc->frames_since_keyframe++;

        return comp_size;
    }
}

/* ============================================================================
 * Decoder
 * ========================================================================== */

void mp_delta_decoder_init(MPDeltaDecoder* dec) {
    if (!dec) return;
    memset(dec, 0, sizeof(*dec));
}

void mp_delta_decoder_reset(MPDeltaDecoder* dec) {
    if (!dec) return;
    dec->has_prev = false;
}

bool mp_delta_decode(MPDeltaDecoder* dec,
                      const uint8_t* compressed, uint32_t comp_size,
                      uint8_t frame_type,
                      uint32_t* out_frame)
{
    if (!dec || !compressed || !out_frame) return false;

    if (frame_type == MP_FRAME_KEYFRAME) {
        /* Decompress directly into output */
        int pixels = mp_rle_decompress(compressed, comp_size,
                                        out_frame, MP_FRAME_PIXELS);
        if (pixels != MP_FRAME_PIXELS) return false;

        /* Save as reference */
        memcpy(dec->prev_frame, out_frame, MP_FRAME_BYTES);
        dec->has_prev = true;
        return true;

    } else if (frame_type == MP_FRAME_DELTA) {
        if (!dec->has_prev) {
            /* Can't apply delta without a reference frame */
            return false;
        }

        /* Decompress XOR delta */
        static uint32_t delta[MP_FRAME_PIXELS];
        int pixels = mp_rle_decompress(compressed, comp_size,
                                        delta, MP_FRAME_PIXELS);
        if (pixels != MP_FRAME_PIXELS) return false;

        /* Apply XOR to reconstruct frame */
        for (uint32_t i = 0; i < MP_FRAME_PIXELS; i++) {
            out_frame[i] = dec->prev_frame[i] ^ delta[i];
        }

        /* Update reference */
        memcpy(dec->prev_frame, out_frame, MP_FRAME_BYTES);
        return true;
    }

    return false;
}

bool mp_delta_needs_keyframe(const MPDeltaDecoder* dec) {
    return dec && !dec->has_prev;
}
