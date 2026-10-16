#ifndef _TICK_PCM_H
#define _TICK_PCM_H

#include <stdint.h>

/* 每个滴答样本长度（160ms @ 24kHz），由 tools/gen_tick_pcm.py 生成 */
#define TICK_PCM_N 3840

extern const int16_t tick_pcm_a[TICK_PCM_N];
extern const int16_t tick_pcm_b[TICK_PCM_N];

#endif
