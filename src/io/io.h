#ifndef NDS_EMU_IO_H
#define NDS_EMU_IO_H

#include <stdint.h>
#include "irq.h"
#include "timer.h"
#include "key.h"
#include "dma.h"
#include "fifo.h"
#include "disp.h"

struct bus; /* 前向声明：io 需要 bus 反指，供 DMA 搬运访存 */

/* 寄存器区（0x04000000 起）的完整状态：中断 + 定时器 + 按键 + DMA + IPC FIFO + 显示。
   按模块结构规则拆功能文件：irq（中断）、timer（定时器）、key（按键）、
   dma（DMA）、fifo（IPC FIFO）、disp（2D 显示控制）；本文件是对外接口，bus 在
   IO 区间调用 io_read8/io_write8，FIFO 的 32 位收发经 io_recv32/io_send32。
   中断寄存器按 CPU 分流：irq[0]=ARM9、irq[1]=ARM7（同址、按访问者身份选择）。
   对外信号：io_set_vblank、io_set_keyinput、io_advance_timers、io_irq_pending。 */
typedef struct io {
    irq_t irq[2];                     /* 中断：IME/IE/IF 各一套（ARM9/ARM7） */
    nds_timer_t timer[IO_TIMER_COUNT];/* 定时器 0-3 */
    keypad_t keypad;                  /* KEYINPUT */
    dma_channel_t dma;                /* DMA（阶段 7：只实现 DMA0 一条通道） */
    ipc_fifo_t fifo;                  /* IPC FIFO（阶段 8：双核通信） */
    disp_t disp;                      /* 2D 显示控制（阶段 9：DISPCNT/BGxCNT/滚动） */
    struct bus *bus;                  /* bus 反指：DMA 搬运需经 bus 访存 */
} io_t;

/* 创建 / 销毁寄存器区（calloc 清零，未配置位读 0） */
io_t *io_create(void);
void io_destroy(io_t *io);

/* 按字节访问整个 IO 区。is_arm7：当前访问者身份（中断/FIFO CNT 按此分流） */
uint8_t io_read8(const io_t *io, uint32_t addr, int is_arm7);
void io_write8(io_t *io, uint32_t addr, uint8_t val, int is_arm7);

/* FIFO 32 位收发（bus 对 SEND/RECV 地址整体转发，避免拆字节破坏队列） */
uint32_t io_recv32(io_t *io, int is_arm7);
void io_send32(io_t *io, int is_arm7, uint32_t val);

/* ---- 硬件侧信号（主循环 / 测试驱动调用） ---- */

/* 一帧结束：把 VBlank 位挂起（6.3；VBlank 是 ARM9 显示事件，置 ARM9 的 IF） */
void io_set_vblank(io_t *io);

/* 是否真会发生中断（默认 ARM9 视角，6.4 主循环用） */
int io_irq_pending(const io_t *io);

/* 按键状态：pressed 位=1 表示按下（6.6，由窗口键事件驱动） */
void io_set_keyinput(io_t *io, uint16_t pressed);

/* 一个周期（一条指令）过去：推进所有使能定时器（6.5，cpu_step 调用） */
void io_advance_timers(io_t *io);

#endif /* NDS_EMU_IO_H */
