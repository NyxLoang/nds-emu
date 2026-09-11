#ifndef NDS_EMU_IO_TIMER_H
#define NDS_EMU_IO_TIMER_H

#include <stdint.h>

/* 定时器 0-3 寄存器地址：每个定时器占 4 字节（CNT_L 16 位 + CNT_H 16 位） */
#define IO_TIMER0_BASE   0x04000100u
#define IO_TIMER_STRIDE  4u
#define IO_TIMER_COUNT   4u
#define IO_TIMER_END     0x04000110u   /* 上界（不含） */

/* TMxCNT_H 控制位（bit7 使能；bit0-1 分频；bit2 级联阶段 6 忽略） */
#define TIMER_CNT_ENABLE       0x80u
#define TIMER_CNT_IRQ          0x40u   /* bit6：溢出置 IF（21-B9h） */
#define TIMER_CNT_PRESCALER_MASK 0x3u

/* 单个定时器。简化模型：
   - cnt_l 是计数值，软件可读写，硬件按分频递增（16 位自然回绕）；
   - cnt_h 是控制，bit7=1 才计数，bit0-1 选分频（1/64/256/1024）；
   - acc 是内部累加器：每条指令 +1，攒够分频数才给 cnt_l 加 1。
     这样 1:64 分频时，cnt_l 约每 64 条指令涨 1。 */
typedef struct nds_timer {
    uint16_t cnt_l;
    uint16_t cnt_h;
    uint16_t reload;  /* 21-B9j：写 CNT_L 的重载值；CNT_H 使能时从它起跳 */
    uint32_t acc;
} nds_timer_t;

uint8_t timer_read8(const nds_timer_t *t, uint32_t addr);
void timer_write8(nds_timer_t *t, uint32_t addr, uint8_t val);

/* 经过 cycles 个系统周期：使能的定时器按分频累计（支持一次多周期跳跃） */
int timer_advance(nds_timer_t *t, uint32_t cycles);

#endif /* NDS_EMU_IO_TIMER_H */
