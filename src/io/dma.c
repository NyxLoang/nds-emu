#include "dma.h"
#include "bus/bus.h"
#include "io/io.h"
#include <stdio.h>

/* 通道内字节偏移（相对本通道基址）：0-3=SAD、4-7=DAD、8-9=CNT_L、10-11=CNT_H */
static uint32_t ch_off(uint32_t addr)
{
    return (addr - IO_DMA0_BASE) % IO_DMA_STRIDE;
}

/* 按地址选通道（0..3） */
static uint32_t ch_index(uint32_t addr)
{
    return (addr - IO_DMA0_BASE) / IO_DMA_STRIDE;
}

int dma_is_addr(uint32_t addr)
{
    return addr >= IO_DMA0_BASE && addr < IO_DMA_END;
}

uint8_t dma_read8(const dma_t *dma, uint32_t addr)
{
    const dma_channel_t *d = &dma->ch[ch_index(addr)];
    uint32_t off = ch_off(addr);
    if (off < 4)      return (uint8_t)(d->sad >> (off * 8));
    if (off < 8)      return (uint8_t)(d->dad >> ((off - 4) * 8));
    if (off < 10)     return (uint8_t)(d->cnt_l >> ((off - 8) * 8));
    return (uint8_t)(d->cnt_h >> ((off - 10) * 8));
}

/* 地址推进：按地址控制位增/减/固定（3=增/重载，repeat 触发时靠外部重置回 sad/dad）。 */
static uint32_t dma_advance(uint32_t a, int mode, uint32_t step)
{
    switch (mode) {
    case 1:  return a - step;  /* 递减 */
    case 2:  return a;         /* 固定 */
    default: return a + step;  /* 0=增 / 3=增/重载 */
    }
}

/* DMA 完成中断（21-B9p）：搬完且 CNT bit14(IRQ) 置位时，把当前核 IF 的
   bit8+通道 置 1（真机 DMA0-3 完成中断 = IF bit8-11）。 */
static void dma_irq_done(const dma_channel_t *dma, struct bus *bus, int ch,
                         int is_arm7)
{
    if (bus == NULL || bus->io == NULL)
        return;
    if ((dma->cnt_h & DMA_CNT_IRQ) == 0)
        return;
    bus->io->irq[is_arm7 ? 1 : 0].ifl |= (uint32_t)(1u << (8 + ch));
}

/* 执行一次拷贝：把 N 个字/半字从源搬到目的。
   源/目的地址控制按增/减/固定处理；搬运走 bus_read/write，源可落在卡带 CARD_DATA。
   非重复搬运搬完自动清使能（真机同款行为）；重复模式保持使能，每次触发都重搬同一块。 */
/* 21-B9yi：诊断用——DMA 搬运期间置 1，供总线侧区分「CPU 读 / DMA 读」。 */
int g_dma_active;

static void dma_transfer(dma_channel_t *dma, struct bus *bus, int ch, int is_arm7)
{
    uint32_t n = dma->cnt_l != 0 ? dma->cnt_l : 0x4000u; /* 0 按 GBA/NDS 惯例=0x4000 */
    int is32 = (dma->cnt_h & DMA_CNT_32BIT) != 0;
    uint32_t step = is32 ? 4u : 2u;
    uint32_t src = dma->sad;
    uint32_t dst = dma->dad;
    int src_mode = (dma->cnt_h >> 7) & 3u; /* CNT bit23-24 */
    int dst_mode = (dma->cnt_h >> 5) & 3u; /* CNT bit21-22 */

    /* 21-B9xq 诊断：前 40 次 DMA 搬运的通道/属主/源/目的/长度/单位（bring-up 定位用） */
    {
        static int dma_log = 0;
        if (dma_log < 40) {
            printf("dma: arm%d ch=%d sad=%08X dad=%08X n=%u unit=%d"
                   " srcmode=%d dstmode=%d cnt=%04X\n",
                   is_arm7 ? 7 : 9, ch, dma->sad, dma->dad, n, is32 ? 32 : 16,
                   src_mode, dst_mode, dma->cnt_h);
            dma_log++;
        }
    }

    /* 21-B9wu：搬运期间把「当前访问者」切到 DMA 属主核。总线按 active_is_arm7
       分流 IO：0x04000400 在 ARM9 视角是 GX 命令 FIFO、ARM7 视角是音频寄存器；
       IME/IE/IF 也是两套。不切换的话 ARM9 的显示列表 DMA 会被当成 ARM7 的音频写，
       GX 收不到几何命令（3D 画面全黑），DMA 完成中断也会挂到错的核心。 */
    int prev_arm7 = bus->active_is_arm7;
    bus->active_is_arm7 = is_arm7;
    int prev_dma = g_dma_active;
    g_dma_active = 1;
    for (uint32_t i = 0; i < n; i++) {
        /* 21-B9zb: DMA 从 CARD_DATA 取数时按卡带就绪时钟等待（保留：让 DMA
           读到的都是真实 ROM 数据，而不是 FIFO 残留值）。 */
        if (src == BUS_CARD_DATA && bus != NULL && bus->io != NULL)
            cartbus_advance(&bus->io->cartbus, 100000u);
        if (is32)
            bus_write32(bus, dst, bus_read32(bus, src));
        else
            bus_write16(bus, dst, bus_read16(bus, src));
        src = dma_advance(src, src_mode, step);
        dst = dma_advance(dst, dst_mode, step);
    }

    bus->active_is_arm7 = prev_arm7;
    g_dma_active = prev_dma;
    /* 21-B9yi：把推进后的地址写回通道——重复模式（CNT bit25）下卡带 DMA 会
       反复触发，**地址必须跨轮次保持前进**，否则每个字都写到同一个地址
       （实测：游戏用 `AF000001`（模式 5 + 重复 + 源固定/目的递增）逐字搬 512B，
       本地因地址不前进把整块写到了同一个 dword，游戏数据对不上后走进死循环）。 */
    dma->sad = src;
    dma->dad = dst;
    if ((dma->cnt_h & DMA_CNT_REPEAT) == 0)
        dma->cnt_h &= (uint16_t)~DMA_CNT_ENABLE;
    dma_irq_done(dma, bus, ch, is_arm7);
}

void dma_write8(dma_t *dma, uint32_t addr, uint8_t val, struct bus *bus,
                int is_arm7)
{
    dma_channel_t *d = &dma->ch[ch_index(addr)];
    uint32_t off = ch_off(addr);
    if (off < 4) {
        uint32_t shift = off * 8;
        d->sad = (d->sad & ~(0xFFu << shift)) | ((uint32_t)val << shift);
    } else if (off < 8) {
        uint32_t shift = (off - 4) * 8;
        d->dad = (d->dad & ~(0xFFu << shift)) | ((uint32_t)val << shift);
    } else if (off < 10) {
        uint32_t shift = (off - 8) * 8;
        d->cnt_l = (uint16_t)((d->cnt_l & ~(0xFFu << shift)) | ((uint32_t)val << shift));
    } else {
        uint32_t shift = (off - 10) * 8;
        d->cnt_h = (uint16_t)((d->cnt_h & ~(0xFFu << shift)) | ((uint32_t)val << shift));
        /* 触发条件：写 CNT_H 高字节（含使能位）且使能位置位；
           模式 0=立即、模式 7=GX FIFO（仅 ARM9）都在这里同步搬运。 */
        if (off == 11 && (d->cnt_h & DMA_CNT_ENABLE) != 0) {
            unsigned mode = ((d->cnt_h & DMA_CNT_MODE_MASK) >> DMA_CNT_MODE_SHIFT);
            if (mode == DMA_START_IMMED ||
                (!is_arm7 && mode == DMA_START_GXFIFO))
                dma_transfer(d, bus, (int)ch_index(addr), is_arm7);
        }
    }
}

void dma_fire(dma_t *dma, struct bus *bus, int start_mode, int is_arm7)
{
    for (int c = 0; c < IO_DMA_COUNT; c++) {
        dma_channel_t *d = &dma->ch[c];
        if ((d->cnt_h & DMA_CNT_ENABLE) == 0)
            continue;
        if (((d->cnt_h & DMA_CNT_MODE_MASK) >> DMA_CNT_MODE_SHIFT) != start_mode)
            continue;
        dma_transfer(d, bus, c, is_arm7);
    }
}

/* 21-B9ws：卡带 DMA 触发。ARM9 用 3 位 start mode（5=DS cart）；
   ARM7 的 DS cart 模式是 CNT 高半字 bits13-12 | 0x10（melonDS: 0x12）。 */
void dma_fire_card(dma_t *dma, struct bus *bus, int is_arm7)
{
    /* 21-B9yi：melonDS 的 `NDSCartSlot::Interface::CheckDMA()` 第一行就是
       `if (!(ROMCnt & (1<<23))) return;` ——DRQ 未置位时不触发卡带 DMA。
       本地旧实现无条件触发，于是「武装了卡带 DMA 但数据还没就绪」时会读到
       FIFO 里的残留值（实测游戏缓冲被写进 0xFFFFFFFF，随后逻辑走飞）。 */
    if (bus == NULL || bus->io == NULL)
        return;
    if ((bus->io->cartbus.romctrl & CART_ROMCTRL_DRQ) == 0)
        return;
    for (int c = 0; c < IO_DMA_COUNT; c++) {
        dma_channel_t *d = &dma->ch[c];
        if ((d->cnt_h & DMA_CNT_ENABLE) == 0)
            continue;
        unsigned mode = is_arm7
            ? ((((unsigned)d->cnt_h >> 12) & 3u) | 0x10u)
            : (((unsigned)d->cnt_h & DMA_CNT_MODE_MASK) >> DMA_CNT_MODE_SHIFT);
        if ((is_arm7 && mode == DMA_START_CARD7) ||
            (!is_arm7 && mode == DMA_START_CARD))
            dma_transfer(d, bus, c, is_arm7);
    }
}
