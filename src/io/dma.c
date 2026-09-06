#include "dma.h"
#include "bus/bus.h"
#include "io/io.h"

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
static void dma_irq_done(const dma_channel_t *dma, struct bus *bus, int ch)
{
    if (bus == NULL || bus->io == NULL)
        return;
    if ((dma->cnt_h & DMA_CNT_IRQ) == 0)
        return;
    int idx = bus->active_is_arm7 ? 1 : 0;
    bus->io->irq[idx].ifl |= (uint32_t)(1u << (8 + ch));
}

/* 执行一次拷贝：把 N 个字/半字从源搬到目的。
   源/目的地址控制按增/减/固定处理；搬运走 bus_read/write，源可落在卡带 CARD_DATA。
   非重复搬运搬完自动清使能（真机同款行为）；重复模式保持使能，每次触发都重搬同一块。 */
static void dma_transfer(dma_channel_t *dma, struct bus *bus, int ch)
{
    uint32_t n = dma->cnt_l != 0 ? dma->cnt_l : 0x4000u; /* 0 按 GBA/NDS 惯例=0x4000 */
    int is32 = (dma->cnt_h & DMA_CNT_32BIT) != 0;
    uint32_t step = is32 ? 4u : 2u;
    uint32_t src = dma->sad;
    uint32_t dst = dma->dad;
    int src_mode = (dma->cnt_h >> 7) & 3u; /* CNT bit23-24 */
    int dst_mode = (dma->cnt_h >> 5) & 3u; /* CNT bit21-22 */

    for (uint32_t i = 0; i < n; i++) {
        /* 21-B9zb: DMA 从 CARD_DATA 取数时按卡带就绪时钟等待 */
        if (src == BUS_CARD_DATA && bus != NULL && bus->io != NULL)
            cartbus_advance(&bus->io->cartbus, 100000u);
        if (is32)
            bus_write32(bus, dst, bus_read32(bus, src));
        else
            bus_write16(bus, dst, bus_read16(bus, src));
        src = dma_advance(src, src_mode, step);
        dst = dma_advance(dst, dst_mode, step);
    }

    if ((dma->cnt_h & DMA_CNT_REPEAT) == 0)
        dma->cnt_h &= (uint16_t)~DMA_CNT_ENABLE;
    dma_irq_done(dma, bus, ch);
}

void dma_write8(dma_t *dma, uint32_t addr, uint8_t val, struct bus *bus)
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
        /* 触发条件：写 CNT_H 高字节（含使能位）且使能位置位、模式=立即 */
        if (off == 11 && (d->cnt_h & DMA_CNT_ENABLE) != 0 &&
            ((d->cnt_h & DMA_CNT_MODE_MASK) >> DMA_CNT_MODE_SHIFT) == DMA_START_IMMED)
            dma_transfer(d, bus, (int)ch_index(addr));
    }
}

void dma_fire(dma_t *dma, struct bus *bus, int start_mode)
{
    for (int c = 0; c < IO_DMA_COUNT; c++) {
        dma_channel_t *d = &dma->ch[c];
        if ((d->cnt_h & DMA_CNT_ENABLE) == 0)
            continue;
        if (((d->cnt_h & DMA_CNT_MODE_MASK) >> DMA_CNT_MODE_SHIFT) != start_mode)
            continue;
        dma_transfer(d, bus, c);
    }
}
