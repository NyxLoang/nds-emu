#ifndef NDS_EMU_IO_H
#define NDS_EMU_IO_H

#include <stdint.h>
#include "irq.h"
#include "timer.h"
#include "key.h"

/* 寄存器区（0x04000000 起）的完整状态：中断控制器 + 4 个定时器 + 按键。
   按模块结构规则拆功能文件：irq.h/.c（中断）、timer.h/.c（定时器）、
   key.h/.c（按键）；本文件是对外接口，bus 在 IO 区间调用 io_read8/io_write8。
   对外信号：io_set_vblank（硬件产生 VBlank）、io_set_keyinput（按键状态）、
   io_advance_timers（每条指令推进定时器）、io_irq_pending（是否真发生中断）。 */
typedef struct io {
    irq_t irq;                   /* 中断：IME / IE / IF */
    nds_timer_t timer[IO_TIMER_COUNT]; /* 定时器 0-3 */
    keypad_t keypad;             /* KEYINPUT */
} io_t;

/* 创建 / 销毁寄存器区（calloc 清零，未配置位读 0） */
io_t *io_create(void);
void io_destroy(io_t *io);

/* 按字节访问整个 IO 区（bus 在 0x04000000 区间转发到这里） */
uint8_t io_read8(const io_t *io, uint32_t addr);
void io_write8(io_t *io, uint32_t addr, uint8_t val);

/* ---- 硬件侧信号（主循环 / 测试驱动调用） ---- */

/* 一帧结束：把 VBlank 位挂起（6.3 用指令计数每帧调用） */
void io_set_vblank(io_t *io);

/* 是否真会发生中断：IF&IE!=0 且 IME 打开（6.4） */
int io_irq_pending(const io_t *io);

/* 按键状态：pressed 位=1 表示按下（6.6，由窗口键事件驱动） */
void io_set_keyinput(io_t *io, uint16_t pressed);

/* 一个周期（一条指令）过去：推进所有使能定时器（6.5，cpu_step 调用） */
void io_advance_timers(io_t *io);

#endif /* NDS_EMU_IO_H */
