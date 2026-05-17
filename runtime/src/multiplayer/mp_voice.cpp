/**
 * @file mp_voice.cpp
 * @brief Voice chat implementation using SDL2 audio capture + mu-law codec
 */

#include "mp_voice.h"
#include <SDL.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ============================================================================
 * Mu-law codec (ITU-T G.711)
 *
 * Compresses 16-bit PCM to 8-bit mu-law. ~48dB dynamic range.
 * Simple, fast, good enough for voice over a game.
 * ========================================================================== */

static const int16_t g_mulaw_decode_table[256] = {
    -32124,-31100,-30076,-29052,-28028,-27004,-25980,-24956,
    -23932,-22908,-21884,-20860,-19836,-18812,-17788,-16764,
    -15996,-15484,-14972,-14460,-13948,-13436,-12924,-12412,
    -11900,-11388,-10876,-10364, -9852, -9340, -8828, -8316,
     -7932, -7676, -7420, -7164, -6908, -6652, -6396, -6140,
     -5884, -5628, -5372, -5116, -4860, -4604, -4348, -4092,
     -3900, -3772, -3644, -3516, -3388, -3260, -3132, -3004,
     -2876, -2748, -2620, -2492, -2364, -2236, -2108, -1980,
     -1884, -1820, -1756, -1692, -1628, -1564, -1500, -1436,
     -1372, -1308, -1244, -1180, -1116, -1052,  -988,  -924,
      -876,  -844,  -812,  -780,  -748,  -716,  -684,  -652,
      -620,  -588,  -556,  -524,  -492,  -460,  -428,  -396,
      -372,  -356,  -340,  -324,  -308,  -292,  -276,  -260,
      -244,  -228,  -212,  -196,  -180,  -164,  -148,  -132,
      -120,  -112,  -104,   -96,   -88,   -80,   -72,   -64,
       -56,   -48,   -40,   -32,   -24,   -16,    -8,     0,
     32124, 31100, 30076, 29052, 28028, 27004, 25980, 24956,
     23932, 22908, 21884, 20860, 19836, 18812, 17788, 16764,
     15996, 15484, 14972, 14460, 13948, 13436, 12924, 12412,
     11900, 11388, 10876, 10364,  9852,  9340,  8828,  8316,
      7932,  7676,  7420,  7164,  6908,  6652,  6396,  6140,
      5884,  5628,  5372,  5116,  4860,  4604,  4348,  4092,
      3900,  3772,  3644,  3516,  3388,  3260,  3132,  3004,
      2876,  2748,  2620,  2492,  2364,  2236,  2108,  1980,
      1884,  1820,  1756,  1692,  1628,  1564,  1500,  1436,
      1372,  1308,  1244,  1180,  1116,  1052,   988,   924,
       876,   844,   812,   780,   748,   716,   684,   652,
       620,   588,   556,   524,   492,   460,   428,   396,
       372,   356,   340,   324,   308,   292,   276,   260,
       244,   228,   212,   196,   180,   164,   148,   132,
       120,   112,   104,    96,    88,    80,    72,    64,
        56,    48,    40,    32,    24,    16,     8,     0,
};

static uint8_t pcm_to_mulaw(int16_t pcm) {
    int sign = (pcm >> 8) & 0x80;
    if (sign) pcm = -pcm;
    if (pcm > 32635) pcm = 32635;
    pcm += 0x84;

    int exp = 7;
    uint16_t mask = 0x4000;
    while (exp > 0 && !(pcm & mask)) {
        exp--;
        mask >>= 1;
    }

    int mantissa = (pcm >> (exp + 3)) & 0x0F;
    uint8_t mulaw = ~(sign | (exp << 4) | mantissa);
    return mulaw;
}

static int16_t mulaw_to_pcm(uint8_t mulaw) {
    return g_mulaw_decode_table[mulaw];
}

/* ============================================================================
 * Voice State
 * ========================================================================== */

#define CAPTURE_BUFFER_SIZE 4096
#define PLAYBACK_BUFFER_SIZE 8192

static struct {
    bool initialized;
    MPVoiceMode mode;

    /* Capture */
    SDL_AudioDeviceID capture_device;
    int16_t capture_buffer[CAPTURE_BUFFER_SIZE];
    int capture_write_pos;
    bool ptt_active;
    float vox_threshold;
    float tx_volume;

    /* Playback */
    SDL_AudioDeviceID playback_device;
    int16_t playback_buffer[PLAYBACK_BUFFER_SIZE];
    int playback_write_pos;
    int playback_read_pos;
    float rx_volume;

    /* Per-player mute */
    bool muted[MP_MAX_PLAYERS];

    /* Metering */
    float input_level;
    bool transmitting;

} g_voice = {
    .mode = VOICE_MODE_OFF,
    .vox_threshold = 0.05f,
    .tx_volume = 1.0f,
    .rx_volume = 1.0f,
};

/* ============================================================================
 * SDL Audio Callbacks
 * ========================================================================== */

static void capture_callback(void* userdata, Uint8* stream, int len) {
    (void)userdata;
    int16_t* samples = (int16_t*)stream;
    int sample_count = len / sizeof(int16_t);

    for (int i = 0; i < sample_count; i++) {
        int next = (g_voice.capture_write_pos + 1) % CAPTURE_BUFFER_SIZE;
        g_voice.capture_buffer[g_voice.capture_write_pos] = samples[i];
        g_voice.capture_write_pos = next;
    }
}

static void playback_callback(void* userdata, Uint8* stream, int len) {
    (void)userdata;
    int16_t* output = (int16_t*)stream;
    int sample_count = len / sizeof(int16_t);

    for (int i = 0; i < sample_count; i++) {
        if (g_voice.playback_read_pos != g_voice.playback_write_pos) {
            int32_t sample = g_voice.playback_buffer[g_voice.playback_read_pos];
            sample = (int32_t)(sample * g_voice.rx_volume);
            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;
            output[i] = (int16_t)sample;
            g_voice.playback_read_pos = (g_voice.playback_read_pos + 1) % PLAYBACK_BUFFER_SIZE;
        } else {
            output[i] = 0;
        }
    }
}

/* ============================================================================
 * Public API
 * ========================================================================== */

bool mp_voice_init(void) {
    if (g_voice.initialized) return true;

    /* Open capture device */
    SDL_AudioSpec want_cap, have_cap;
    memset(&want_cap, 0, sizeof(want_cap));
    want_cap.freq = VOICE_SAMPLE_RATE;
    want_cap.format = AUDIO_S16SYS;
    want_cap.channels = 1;
    want_cap.samples = VOICE_FRAME_SAMPLES;
    want_cap.callback = capture_callback;

    g_voice.capture_device = SDL_OpenAudioDevice(NULL, 1, &want_cap, &have_cap, 0);
    if (g_voice.capture_device == 0) {
        fprintf(stderr, "[VOICE] Failed to open capture device: %s\n", SDL_GetError());
        /* Voice chat won't work but that's OK */
        return false;
    }

    /* Open playback device (separate from game audio, also at 8kHz mono) */
    SDL_AudioSpec want_play, have_play;
    memset(&want_play, 0, sizeof(want_play));
    want_play.freq = VOICE_SAMPLE_RATE;
    want_play.format = AUDIO_S16SYS;
    want_play.channels = 1;
    want_play.samples = 256;
    want_play.callback = playback_callback;

    g_voice.playback_device = SDL_OpenAudioDevice(NULL, 0, &want_play, &have_play, 0);
    if (g_voice.playback_device == 0) {
        fprintf(stderr, "[VOICE] Failed to open playback device: %s\n", SDL_GetError());
        SDL_CloseAudioDevice(g_voice.capture_device);
        g_voice.capture_device = 0;
        return false;
    }

    /* Start playback (capture starts when mode != OFF) */
    SDL_PauseAudioDevice(g_voice.playback_device, 0);

    g_voice.initialized = true;
    fprintf(stderr, "[VOICE] Voice chat initialized (capture=%u, playback=%u)\n",
            g_voice.capture_device, g_voice.playback_device);
    return true;
}

void mp_voice_shutdown(void) {
    if (!g_voice.initialized) return;

    if (g_voice.capture_device) {
        SDL_CloseAudioDevice(g_voice.capture_device);
        g_voice.capture_device = 0;
    }
    if (g_voice.playback_device) {
        SDL_CloseAudioDevice(g_voice.playback_device);
        g_voice.playback_device = 0;
    }

    g_voice.initialized = false;
    fprintf(stderr, "[VOICE] Voice chat shut down\n");
}

void mp_voice_set_mode(MPVoiceMode mode) {
    g_voice.mode = mode;

    if (g_voice.capture_device) {
        /* Start/stop capture based on mode */
        SDL_PauseAudioDevice(g_voice.capture_device, mode == VOICE_MODE_OFF ? 1 : 0);
    }
}

MPVoiceMode mp_voice_get_mode(void) { return g_voice.mode; }

void mp_voice_set_ptt(bool active) { g_voice.ptt_active = active; }
void mp_voice_set_vox_threshold(float t) { g_voice.vox_threshold = t; }
void mp_voice_set_tx_volume(float v) { g_voice.tx_volume = v; }
void mp_voice_set_rx_volume(float v) { g_voice.rx_volume = v; }

void mp_voice_update(uint8_t* out_packet, uint32_t* out_size) {
    if (!out_packet || !out_size) return;
    *out_size = 0;

    if (!g_voice.initialized || g_voice.mode == VOICE_MODE_OFF) return;

    /* Count available captured samples */
    int available = 0;
    static int read_pos = 0;
    int wp = g_voice.capture_write_pos;

    if (wp >= read_pos)
        available = wp - read_pos;
    else
        available = CAPTURE_BUFFER_SIZE - read_pos + wp;

    if (available < VOICE_FRAME_SAMPLES) return; /* Not enough for a frame */

    /* Read a frame of samples */
    int16_t frame[VOICE_FRAME_SAMPLES];
    float rms = 0;

    for (int i = 0; i < VOICE_FRAME_SAMPLES; i++) {
        frame[i] = (int16_t)(g_voice.capture_buffer[read_pos] * g_voice.tx_volume);
        rms += (float)frame[i] * frame[i];
        read_pos = (read_pos + 1) % CAPTURE_BUFFER_SIZE;
    }

    rms = sqrtf(rms / VOICE_FRAME_SAMPLES) / 32768.0f;
    g_voice.input_level = rms;

    /* Determine if we should transmit */
    bool should_tx = false;
    switch (g_voice.mode) {
    case VOICE_MODE_PTT:
        should_tx = g_voice.ptt_active;
        break;
    case VOICE_MODE_ACTIVE:
        should_tx = (rms > g_voice.vox_threshold);
        break;
    case VOICE_MODE_ALWAYS:
        should_tx = true;
        break;
    default:
        break;
    }

    g_voice.transmitting = should_tx;
    if (!should_tx) return;

    /* Encode to mu-law */
    MPVoicePacket* pkt = (MPVoicePacket*)out_packet;
    memset(pkt, 0, sizeof(MPVoicePacket));
    pkt->header.type = MP_PKT_VOICE;
    pkt->header.version = MP_PROTOCOL_VERSION;
    pkt->sample_count = VOICE_FRAME_SAMPLES;

    uint8_t* mulaw_data = out_packet + sizeof(MPVoicePacket);
    for (int i = 0; i < VOICE_FRAME_SAMPLES; i++) {
        mulaw_data[i] = pcm_to_mulaw(frame[i]);
    }

    *out_size = sizeof(MPVoicePacket) + VOICE_FRAME_SAMPLES;
}

void mp_voice_receive(int from_slot, const void* data, uint32_t size) {
    if (!g_voice.initialized) return;
    if (from_slot < 0 || from_slot >= MP_MAX_PLAYERS) return;
    if (g_voice.muted[from_slot]) return;
    if (size < sizeof(MPVoicePacket)) return;

    const MPVoicePacket* pkt = (const MPVoicePacket*)data;
    const uint8_t* mulaw_data = (const uint8_t*)data + sizeof(MPVoicePacket);

    if (sizeof(MPVoicePacket) + pkt->sample_count > size) return;

    /* Decode mu-law and mix into playback buffer */
    SDL_LockAudioDevice(g_voice.playback_device);

    for (int i = 0; i < pkt->sample_count; i++) {
        int16_t sample = mulaw_to_pcm(mulaw_data[i]);

        /* Mix with existing (additive) */
        int next = (g_voice.playback_write_pos + 1) % PLAYBACK_BUFFER_SIZE;
        int32_t mixed = g_voice.playback_buffer[g_voice.playback_write_pos] + sample;
        if (mixed > 32767) mixed = 32767;
        if (mixed < -32768) mixed = -32768;
        g_voice.playback_buffer[g_voice.playback_write_pos] = (int16_t)mixed;
        g_voice.playback_write_pos = next;
    }

    SDL_UnlockAudioDevice(g_voice.playback_device);
}

bool mp_voice_is_transmitting(void) { return g_voice.transmitting; }
float mp_voice_get_input_level(void) { return g_voice.input_level; }

void mp_voice_mute_player(int slot, bool muted) {
    if (slot >= 0 && slot < MP_MAX_PLAYERS)
        g_voice.muted[slot] = muted;
}

bool mp_voice_is_player_muted(int slot) {
    if (slot >= 0 && slot < MP_MAX_PLAYERS)
        return g_voice.muted[slot];
    return false;
}
