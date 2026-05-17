#ifndef HWTRACE_H
#define HWTRACE_H

#include "gbrt.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void hwtrace_init(const char *filename);
void hwtrace_close(void);
void hwtrace_scanline(GBContext *ctx, uint8_t line);
void hwtrace_vblank(GBContext *ctx);
bool hwtrace_active(void);
uint32_t hwtrace_frame_count(void);

#ifdef __cplusplus
}
#endif

#endif /* HWTRACE_H */
