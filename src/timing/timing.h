#ifndef NDS_EMU_TIMING_H
#define NDS_EMU_TIMING_H

#include <stdint.h>

/* 事件目标调度：NDS 双核与硬件事件共用一个系统时间戳。
   本阶段只提供最小事件表，后续 VBlank/扫描线/FIFO/卡带都挂到这里。 */
#define TIMING_MAX_EVENTS 8

typedef void (*timing_cb)(void *ctx);

typedef struct timing_event {
    uint64_t deadline;    /* 系统周期（ARM7 时钟单位） */
    timing_cb cb;
    void *ctx;
    int armed;
} timing_event_t;

typedef struct timing {
    uint64_t now;         /* 当前系统时间 */
    timing_event_t ev[TIMING_MAX_EVENTS];
} timing_t;

void timing_init(timing_t *t);
void timing_arm(timing_t *t, int slot, uint64_t delay, timing_cb cb, void *ctx);
void timing_disarm(timing_t *t, int slot);
uint64_t timing_next(const timing_t *t);
void timing_advance(timing_t *t, uint64_t target);

#endif
