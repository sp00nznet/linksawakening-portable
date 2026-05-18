/* Minimal libxenon smoke test for the la360 toolchain.
 *
 * Build via cmake/test/build_hello.sh from WSL, then run in Xenia:
 *   D:\emu\xenia-canary\xenia_canary.exe <out>/hello.xex
 *
 * Expected: a window opens, console text appears, "Hello from la360"
 * shows up in Xenia's stdout log.
 */

#include <stdio.h>
#include <stdint.h>
#include <xetypes.h>
#include <xenon_uart/xenon_uart.h>
#include <console/console.h>
#include <xenos/xenos.h>
#include <time/time.h>

int main(void)
{
    /* Bring up video at auto-detected resolution. Returns once the GPU has
     * a framebuffer and scanout is running. */
    xenos_init(VIDEO_MODE_AUTO);

    /* Console: writes via printf go to the framebuffer text overlay
     * (and to UART when a serial cable is attached on real hardware;
     * Xenia surfaces these in its log window). */
    console_init();

    uart_puts((unsigned char*)"la360: hello_xenon smoke test\n");
    printf("la360: hello_xenon smoke test\n");
    printf("la360: pointer width = %d bits\n", (int)(sizeof(void*) * 8));
    printf("la360: build endianness = %s\n",
#if defined(__BIG_ENDIAN__) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
        "big (PPC native)"
#else
        "little (unexpected on Xenon!)"
#endif
    );
    printf("la360: if you can read this, the toolchain round-trip works.\n");

    /* Spin forever so Xenia keeps showing the framebuffer. A real backend
     * would have a frame loop here calling gb_run_frame +
     * gb_platform_render_frame + vsync. */
    for (;;) {
        putch('.');
        udelay(5 * 1000 * 1000);  /* 5s */
    }

    return 0;
}
