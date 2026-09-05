#ifndef NDS_EMU_IO_H
#define NDS_EMU_IO_H

#include <stdint.h>
#include "irq.h"
#include "timer.h"
#include "key.h"
#include "dma.h"
#include "fifo.h"
#include "disp.h"
#include "touch.h"
#include "memctl.h"
#include "power.h"
#include "math.h"
#include "snd/snd.h"
#include "gx/gx.h"
#include "cart/cartbus.h"

struct bus; /* 前向声明：io 需要 bus 反指，供 DMA 搬运访存 */

/* 寄存器区（0x04000000 起）的完整状态：中断 + 定时器 + 按键 + DMA + IPC FIFO + 显示 + 卡带总线。
   按模块结构规则拆功能文件：irq（中断）、timer（定时器）、key（按键）、
   dma（DMA）、fifo（IPC FIFO）、disp（2D 显示控制）、cartbus（卡带总线，阶段 15）；
   本文件是对外接口，bus 在 IO 区间调用 io_read8/io_write8，FIFO 的 32 位收发经
   io_recv32/io_send32，卡带数据端口经 io_card_data_read32/write32。
   中断寄存器按 CPU 分流：irq[0]=ARM9、irq[1]=ARM7（同址、按访问者身份选择）。
   对外信号：io_set_vblank、io_set_keyinput、io_advance_timers、io_irq_pending、io_attach_cart。 */
typedef struct io {
    irq_t irq[2];                     /* 中断：IME/IE/IF 各一套（ARM9/ARM7） */
    nds_timer_t timer[IO_TIMER_COUNT];/* 定时器 0-3 */
    keypad_t keypad;                  /* KEYINPUT */
    dma_t dma;                        /* DMA：阶段 15 补齐 4 通道 */
    ipc_fifo_t fifo;                  /* IPC FIFO（阶段 8：双核通信） */
    ipc_sync_t sync;                  /* IPCSYNC（阶段 21-B2：双核同步寄存器） */
    disp_t disp;                      /* 2D 显示控制（阶段 9：DISPCNT/BGxCNT/滚动） */
    touch_t touch;                    /* 触摸屏 SPI（阶段 17：SPICNT/SPIDATA + TSC） */
    memctl_t memctl;                  /* 内存控制（阶段 21-B7：EXMEMCNT/WRAMCNT） */
    power_t power;                    /* 电源/调试（阶段 21-B9n：POWCNT/POSTFLG） */
    math_t math;                      /* 硬件除法/开方（阶段 21-B9k：DIV/SQRT） */
    snd_t snd;                        /* 音频（阶段 18：16 通道 + SOUNDCNT/SOUNDBIAS） */
    gx_t gx;                          /* 3D 几何引擎（阶段 19：DISP3DCNT/GXSTAT/GXFIFO） */
    cartbus_t cartbus;                /* 卡带总线（阶段 15：ROMCTRL/命令/数据端口） */
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

/* 几何命令区（0x04000400..0x040005FF）32 位整体写：GXFIFO 命令字 / 命令端口参数。
   由 bus 在 ARM9 视角整体转发（拆字节会破坏 40 位命令语义）。 */
void io_gx_write32(io_t *io, uint32_t addr, uint32_t val);

/* 卡带数据端口 CARD_DATA（0x04100010）32 位读写（bus 在 IO 区外整体转发）。
   读会推进卡带内部读地址；写本阶段仅占位（读 ROM 用不到）。 */
uint32_t io_card_data_read32(io_t *io);
void io_card_data_write32(io_t *io, uint32_t val);

/* 把已装载 ROM 借给卡带总线（只借指针，不拷贝、不释放）。 */
void io_attach_cart(io_t *io, const uint8_t *rom, size_t rom_size);

/* 阶段 16：配置存档芯片类型（分配缓冲，填 0xFF 擦除态）。 */
void io_attach_save(io_t *io, save_type_t type);

/* 拿到存档芯片指针（main.c 用于 .sav 文件持久化）。 */
save_t *io_get_save(io_t *io);

/* ---- 硬件侧信号（主循环 / 测试驱动调用） ---- */

/* 一帧结束：把 VBlank 位挂起（6.3；VBlank 是 ARM9 显示事件，置 ARM9 的 IF） */
void io_set_vblank(io_t *io);

/* 是否真会发生中断（默认 ARM9 视角，6.4 主循环用） */
int io_irq_pending(const io_t *io);

/* 按键状态：pressed 位=1 表示按下（6.6，由窗口键事件驱动） */
void io_set_keyinput(io_t *io, uint16_t pressed);

/* 触摸位置：12 位 ADC 值，down=1 表示笔按下（阶段 17，由窗口鼠标/触摸事件驱动） */
void io_set_touch(io_t *io, uint16_t adc_x, uint16_t adc_y, int down);

/* 一个周期（一条指令）过去：推进所有使能定时器（6.5，cpu_step 调用） */
void io_advance_timers(io_t *io);

#endif /* NDS_EMU_IO_H */
