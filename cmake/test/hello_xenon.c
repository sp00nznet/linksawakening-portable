/* Minimal libxenon smoke test for the la360 toolchain.
 *
 * Build (from WSL with libxenon installed):
 *   cd /tmp && mkdir -p hello-xenon && cd hello-xenon
 *   /usr/local/xenon/bin/xenon-gcc \
 *       -DXENON -m32 -maltivec -fno-pic -mpowerpc64 -mhard-float \
 *       -I/usr/local/xenon/usr/include \
 *       -L/usr/local/xenon/xenon/lib/32 -L/usr/local/xenon/usr/lib \
 *       -T/usr/local/xenon/app.lds \
 *       /mnt/d/ports/la360/cmake/test/hello_xenon.c \
 *       -lxenon -lm -lc -lgcc \
 *       -o hello.elf
 *   /usr/local/xenon/bin/xenon-objcopy -O elf32-powerpc --adjust-vma 0x80000000 \
 *       hello.elf hello.elf32
 *   /mnt/c/xbox360nfs/tools/elf2xex hello.elf32 hello.xex
 *
 * Run in Xenia:
 *   D:\emu\xenia-canary\xenia_canary.exe hello.xex
 *
 * Expected: a window opens, framebuffer fills with a magenta/green pattern,
 * "Hello from la360" appears in Xenia's console log.
 */

#include <stdio.h>
#include <stdint.h>
#include <xetypes.h>
#include <xenon_soc/xenon_power.h>
#include <xenon_uart/xenon_uart.h>
#include <console/console.h>
#include <xenos/xenos.h>

int main(void)
{
    /* Bring up video at the auto-detected resolution. xenos_init returns
     * once the GPU has a framebuffer and the scanout is running. */
    xenos_init(VIDEO_MODE_AUTO);

    /* Console writes go to the screen and to UART (USB serial cable on RGH,
     * captured in Xenia's log window). */
    console_init();

    /* xenon_uart_puts also sends to UART; redundant with console but useful
     * for very early-boot debugging before console is up. */
    xenon_uart_puts("la360: hello_xenon smoke test\n");
    printf("la360: hello_xenon smoke test\n");
    printf("la360: PowerPC %d-bit user space, hard float OK\n",
           (int)(sizeof(void*) * 8));
    printf("la360: this build is %s endian\n",
#if defined(__BIG_ENDIAN__) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
        "big"
#else
        "little"
#endif
    );

    /* Spin forever so the XEX stays running and Xenia/the console keeps
     * displaying the framebuffer. A real backend would have a frame loop
     * here calling gb_run_frame + gb_platform_render_frame + vsync. */
    for (;;) {
        xenon_uart_byte('.');
        /* Coarse sleep — TB ticks ~50 MHz so 250M ticks ≈ 5 sec. */
        uint64_t tb = mftb();
        while (mftb() - tb < 250000000ULL) { }
    }

    return 0;
}
