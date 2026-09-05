#include "timer.h"

/* 分频表：TMxCNT_H bit0-1 = 0→1、1→64、2→256、3→1024 */
static const unsigned s_prescalers[4] = { 1u, 64u, 256u, 1024u };

/* 定时器内偏移（相对本定时器基址）：0/1=CNT_L 低/高字节，2/3=CNT_H 低/高字节。
   用 % IO_TIMER_STRIDE 去掉前面定时器占的地址，避免 TM1 起偏移超过 4。 */
uint8_t timer_read8(const nds_timer_t *t, uint32_t addr)
{
    if (addr < IO_TIMER0_BASE || addr >= IO_TIMER_END)
        return 0;
    uint32_t off = (addr - IO_TIMER0_BASE) % IO_TIMER_STRIDE;
    if (off < 2) /* CNT_L */
        return (uint8_t)(t->cnt_l >> (off * 8));
    return (uint8_t)(t->cnt_h >> ((off - 2) * 8));
}

void timer_write8(nds_timer_t *t, uint32_t addr, uint8_t val)
{
    if (addr < IO_TIMER0_BASE || addr >= IO_TIMER_END)
        return;
    uint32_t off = (addr - IO_TIMER0_BASE) % IO_TIMER_STRIDE;
    if (off < 2) {
        /* 写 CNT_L：只改对应字节，其余位保留 */
        uint32_t shift = off * 8;
        t->cnt_l = (uint16_t)((t->cnt_l & ~(0xFFu << shift)) | ((uint32_t)val << shift));
    } else {
        /* 写 CNT_H：改控制字节；真机写控制寄存器会重启计数器 */
        uint32_t shift = (off - 2) * 8;
        t->cnt_h = (uint16_t)((t->cnt_h & ~(0xFFu << shift)) | ((uint32_t)val << shift));
        t->cnt_l = 0;
        t->acc = 0;
    }
}

int timer_advance(nds_timer_t *t)
{
    if (!(t->cnt_h & TIMER_CNT_ENABLE))
        return 0; /* 未使能：不计数 */
    unsigned div = s_prescalers[t->cnt_h & TIMER_CNT_PRESCALER_MASK];
    if (++t->acc >= div) {
        t->acc = 0;
        if (t->cnt_l == 0xFFFFu) {
            t->cnt_l = 0;
            return 1; /* 21-B9h：溢出，由上层置对应 IF 位 */
        }
        t->cnt_l++;
    }
    return 0;
}
