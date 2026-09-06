#include "timing/timing.h"

void timing_init(timing_t *t)
{
    if (!t) return;
    t->now = 0;
    for (int i = 0; i < TIMING_MAX_EVENTS; i++) {
        t->ev[i].deadline = 0;
        t->ev[i].cb = NULL;
        t->ev[i].ctx = NULL;
        t->ev[i].armed = 0;
    }
}

void timing_arm(timing_t *t, int slot, uint64_t delay,
                timing_cb cb, void *ctx)
{
    if (!t || slot < 0 || slot >= TIMING_MAX_EVENTS) return;
    t->ev[slot].deadline = t->now + delay;
    t->ev[slot].cb = cb;
    t->ev[slot].ctx = ctx;
    t->ev[slot].armed = 1;
}

void timing_disarm(timing_t *t, int slot)
{
    if (!t || slot < 0 || slot >= TIMING_MAX_EVENTS) return;
    t->ev[slot].armed = 0;
    t->ev[slot].cb = NULL;
    t->ev[slot].ctx = NULL;
}

uint64_t timing_next(const timing_t *t)
{
    if (!t) return UINT64_MAX;
    uint64_t next = UINT64_MAX;
    for (int i = 0; i < TIMING_MAX_EVENTS; i++) {
        if (t->ev[i].armed && t->ev[i].deadline < next)
            next = t->ev[i].deadline;
    }
    return next;
}

void timing_advance(timing_t *t, uint64_t target)
{
    if (!t || target <= t->now) return;
    t->now = target;
    /* 到期事件按槽位顺序触发；回调可重新武装自身 */
    for (int i = 0; i < TIMING_MAX_EVENTS; i++) {
        if (t->ev[i].armed && t->ev[i].deadline <= t->now) {
            timing_cb cb = t->ev[i].cb;
            void *ctx = t->ev[i].ctx;
            t->ev[i].armed = 0;
            if (cb) cb(ctx);
        }
    }
}
