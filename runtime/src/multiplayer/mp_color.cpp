/**
 * @file mp_color.cpp
 * @brief Link palette recoloring implementation
 *
 * CGB Link's Awakening uses specific palette entries for Link's tunic.
 * We detect these colors in the framebuffer and shift them to the
 * player's chosen color.
 */

#include "mp_color.h"
#include <math.h>
#include <string.h>

/* ============================================================================
 * Link's Default CGB Palette Colors
 *
 * Link's tunic in LADX CGB mode uses these approximate colors:
 * - Light green (tunic highlight): ~#58A858 or similar
 * - Medium green (tunic body):     ~#386838
 * - Dark green (tunic shadow):     ~#183818
 *
 * These values are derived from the CGB OBJ palette 0 entries.
 * We match with tolerance to handle slight PPU rendering variations.
 * ========================================================================== */

#define MAX_REF_COLORS 6

typedef struct {
    uint8_t r, g, b;
    float   rel_lightness;  /* 0.0 = darkest, 1.0 = lightest */
} RefColor;

static RefColor g_ref_colors[MAX_REF_COLORS];
static int g_ref_count = 0;
static bool g_refs_set = false;

/* Color matching tolerance (per channel) */
#define COLOR_TOLERANCE 24

/* ============================================================================
 * Color Space Helpers
 * ========================================================================== */

static void rgb_to_hsl(uint8_t r, uint8_t g, uint8_t b,
                        float* h, float* s, float* l)
{
    float rf = r / 255.0f;
    float gf = g / 255.0f;
    float bf = b / 255.0f;

    float mx = rf > gf ? (rf > bf ? rf : bf) : (gf > bf ? gf : bf);
    float mn = rf < gf ? (rf < bf ? rf : bf) : (gf < bf ? gf : bf);
    float d = mx - mn;

    *l = (mx + mn) * 0.5f;

    if (d < 0.001f) {
        *h = 0; *s = 0;
        return;
    }

    *s = (*l > 0.5f) ? d / (2.0f - mx - mn) : d / (mx + mn);

    if (mx == rf) {
        *h = (gf - bf) / d + (gf < bf ? 6.0f : 0.0f);
    } else if (mx == gf) {
        *h = (bf - rf) / d + 2.0f;
    } else {
        *h = (rf - gf) / d + 4.0f;
    }
    *h *= 60.0f;
}

static void hsl_to_rgb(float h, float s, float l,
                        uint8_t* r, uint8_t* g, uint8_t* b)
{
    if (s < 0.001f) {
        uint8_t v = (uint8_t)(l * 255.0f);
        *r = *g = *b = v;
        return;
    }

    float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
    float p = 2.0f * l - q;

    auto hue2rgb = [](float p, float q, float t) -> float {
        if (t < 0) t += 1.0f;
        if (t > 1) t -= 1.0f;
        if (t < 1.0f/6.0f) return p + (q - p) * 6.0f * t;
        if (t < 0.5f)      return q;
        if (t < 2.0f/3.0f) return p + (q - p) * (2.0f/3.0f - t) * 6.0f;
        return p;
    };

    float hn = h / 360.0f;
    float rf = hue2rgb(p, q, hn + 1.0f/3.0f);
    float gf = hue2rgb(p, q, hn);
    float bf = hue2rgb(p, q, hn - 1.0f/3.0f);

    *r = (uint8_t)(rf * 255.0f + 0.5f);
    *g = (uint8_t)(gf * 255.0f + 0.5f);
    *b = (uint8_t)(bf * 255.0f + 0.5f);
}

/* ============================================================================
 * Reference Palette Setup
 * ========================================================================== */

void mp_color_set_reference_palette(void) {
    /* Link's default CGB tunic colors (approximate, from LADX CGB palettes) */
    /* These are the green tones used for Link's tunic sprite */
    g_ref_colors[0] = { 0xB8, 0xF8, 0x18, 1.0f };  /* Lightest green */
    g_ref_colors[1] = { 0x58, 0xA8, 0x58, 0.7f };   /* Medium-light green */
    g_ref_colors[2] = { 0x38, 0x68, 0x38, 0.4f };   /* Medium green */
    g_ref_colors[3] = { 0x18, 0x38, 0x18, 0.15f };  /* Dark green */
    g_ref_count = 4;
    g_refs_set = true;
}

/* ============================================================================
 * Framebuffer Recoloring
 * ========================================================================== */

static bool color_matches_ref(uint8_t r, uint8_t g, uint8_t b,
                               int* ref_idx)
{
    for (int i = 0; i < g_ref_count; i++) {
        int dr = (int)r - (int)g_ref_colors[i].r;
        int dg = (int)g - (int)g_ref_colors[i].g;
        int db = (int)b - (int)g_ref_colors[i].b;

        if (dr < 0) dr = -dr;
        if (dg < 0) dg = -dg;
        if (db < 0) db = -db;

        if (dr <= COLOR_TOLERANCE && dg <= COLOR_TOLERANCE && db <= COLOR_TOLERANCE) {
            *ref_idx = i;
            return true;
        }
    }
    return false;
}

void mp_color_recolor_link(uint32_t* framebuffer,
                           float target_h, float target_s, float target_v)
{
    if (!framebuffer || !g_refs_set) return;

    /* Default green hue ~120 - if target is close to default, skip */
    if (target_h > 110.0f && target_h < 130.0f &&
        target_s > 0.7f && target_s < 0.9f)
        return;

    for (int i = 0; i < 160 * 144; i++) {
        uint32_t pixel = framebuffer[i];
        uint8_t r = (pixel >> 16) & 0xFF;
        uint8_t g = (pixel >>  8) & 0xFF;
        uint8_t b = (pixel >>  0) & 0xFF;
        uint8_t a = (pixel >> 24) & 0xFF;

        int ref_idx;
        if (color_matches_ref(r, g, b, &ref_idx)) {
            /* Get the relative lightness from the reference */
            float lightness = g_ref_colors[ref_idx].rel_lightness;

            /* Generate new color at target hue with preserved lightness */
            float new_l = lightness * target_v;
            uint8_t nr, ng, nb;
            hsl_to_rgb(target_h, target_s, new_l, &nr, &ng, &nb);

            framebuffer[i] = ((uint32_t)a << 24) | ((uint32_t)nr << 16) |
                             ((uint32_t)ng << 8) | (uint32_t)nb;
        }
    }
}
