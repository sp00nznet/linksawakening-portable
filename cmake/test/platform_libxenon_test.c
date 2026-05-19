/*
 * platform_libxenon_test.c — exercises platform_libxenon.c without the
 * full recompiled game. Proves the backend's init / render / input / vsync
 * path works on real Xenon hardware and in Xenia.
 *
 * Built by cmake/test/build_platform_test_xenon.sh into a XEX.
 *
 * Behaviour: brings up the platform, then each frame draws a moving
 * gradient + a cursor block the gamepad d-pad moves around, and polls
 * the joypad. Exits when poll_events returns false (Guide button).
 */

#include "platform_sdl.h"
#include <stdint.h>
#include <xenon_uart/xenon_uart.h>

#define GB_W 160
#define GB_H 144

static uint32_t fb[GB_W * GB_H];

int main(void)
{
    if (!gb_platform_init(4)) {
        uart_puts((unsigned char*)"la360-test: gb_platform_init failed\n");
        return 1;
    }
    uart_puts((unsigned char*)"la360-test: platform up, entering frame loop\n");

    int cursor_x = GB_W / 2;
    int cursor_y = GB_H / 2;
    uint32_t frame = 0;

    for (;;) {
        if (!gb_platform_poll_events(0)) break;   /* Guide button quits */

        /* Move a cursor with the d-pad. g_joypad_dpad is active-low:
         * 0x04=Up 0x08=Down 0x02=Left 0x01=Right. */
        if (!(g_joypad_dpad & 0x04) && cursor_y > 4)        cursor_y -= 2;
        if (!(g_joypad_dpad & 0x08) && cursor_y < GB_H - 6) cursor_y += 2;
        if (!(g_joypad_dpad & 0x02) && cursor_x > 4)        cursor_x -= 2;
        if (!(g_joypad_dpad & 0x01) && cursor_x < GB_W - 6) cursor_x += 2;

        /* Draw an animated gradient background */
        for (int y = 0; y < GB_H; ++y) {
            for (int x = 0; x < GB_W; ++x) {
                uint8_t r = (uint8_t)(x + frame);
                uint8_t g = (uint8_t)(y + frame);
                uint8_t b = (uint8_t)(frame);
                fb[y * GB_W + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
        }

        /* A button tints the cursor: white normally, red while A held */
        uint32_t cursor_color =
            (g_joypad_buttons & 0x01) ? 0xFFFFFFFFu : 0xFFFF2020u;

        /* Draw an 8x8 cursor block */
        for (int dy = -4; dy < 4; ++dy) {
            for (int dx = -4; dx < 4; ++dx) {
                int px = cursor_x + dx;
                int py = cursor_y + dy;
                if (px >= 0 && px < GB_W && py >= 0 && py < GB_H)
                    fb[py * GB_W + px] = cursor_color;
            }
        }

        gb_platform_render_frame(fb);
        gb_platform_vsync();

        if ((frame % 60) == 0)
            uart_puts((unsigned char*)"la360-test: frame tick\n");
        frame++;
    }

    gb_platform_shutdown();
    uart_puts((unsigned char*)"la360-test: clean exit\n");
    return 0;
}
