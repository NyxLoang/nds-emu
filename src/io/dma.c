#include "dma.h"
#include "bus/bus.h"

/* 通道内字节偏移（相对本通道基址）：0-3=SAD、4-7=DAD、8-9=CNT_L、10-11=CNT_H */
static uint32_t ch_off(uint32_t addr)
{
    return addr - IO_DMA0_BASE;
}

int dma_is_addr(uint32_t addr)
{
    /* 本阶段只实现 DMA0 一个通道；其余通道地址保留给后续（读 0 写忽略） */
    return addr >= IO_DMA0_BASE && addr < IO_DMA0_BASE + IO_DMA_STRIDE;
}

uint8_t dma_read8(const dma_channel_t *dma, uint32_t addr)
{
    uint32_t off = ch_off(addr);
    if (off < 4)      return (uint8_t)(dma->sad >> (off * 8));
    if (off < 8)      return (uint8_t)(dma->dad >> ((off - 4) * 8));
    if (off < 10)     return (uint8_t)(dma->cnt_l >> ((off - 8) * 8));
    return (uint8_t)(dma->cnt_h >> ((off - 10) * 8));
}

/* 立即模式：把 N 个字/半字从源搬到目的。
   源/目的默认递增；SRC_FIX 源不动（填色）、DST_FIX 目的不动。
   搬运走 bus_read/write，源/目的可落在 Main RAM / VRAM / IO 任意区间。 */
static void dma_transfer(dma_channel_t *dma, struct bus *bus)
{
    uint32_t n = dma->cnt_l != 0 ? dma->cnt_l : 0x4000u; /* 0 按 GBA/NDS 惯例=0x4000 */
    int is32 = (dma->cnt_h & DMA_CNT_32BIT) != 0;
    uint32_t step = is32 ? 4u : 2u;
    uint32_t src = dma->sad;
    uint32_t dst = dma->dad;
    int src_fix = (dma->cnt_h & DMA_CNT_SRC_FIX) != 0;
    int dst_fix = (dma->cnt_h & DMA_CNT_DST_FIX) != 0;

    for (uint32_t i = 0; i < n; i++) {
        if (is32) {
            uint32_t v = bus_read32(bus, src);
            bus_write32(bus, dst, v);
        } else {
            uint16_t v = bus_read16(bus, src);
            bus_write16(bus, dst, v);
        }
        if (!src_fix) src += step;
        if (!dst_fix) dst += step;
    }
    /* 非重复搬运搬完自动清使能（真机同款行为） */
    dma->cnt_h &= (uint16_t)~DMA_CNT_ENABLE;
}

void dma_write8(dma_channel_t *dma, uint32_t addr, uint8_t val, struct bus *bus)
{
    uint32_t off = ch_off(addr);
    if (off < 4) {
        uint32_t shift = off * 8;
        dma->sad = (dma->sad & ~(0xFFu << shift)) | ((uint32_t)val << shift);
    } else if (off < 8) {
        uint32_t shift = (off - 4) * 8;
        dma->dad = (dma->dad & ~(0xFFu << shift)) | ((uint32_t)val << shift);
    } else if (off < 10) {
        uint32_t shift = (off - 8) * 8;
        dma->cnt_l = (uint16_t)((dma->cnt_l & ~(0xFFu << shift)) | ((uint32_t)val << shift));
    } else {
        uint32_t shift = (off - 10) * 8;
        dma->cnt_h = (uint16_t)((dma->cnt_h & ~(0xFFu << shift)) | ((uint32_t)val << shift));
        /* 触发条件：使能位置位 且 模式=立即（bit27-29=0）。
           模式非立即（VBlank/HBlank 等）留到后续阶段，这里不搬。 */
        if ((dma->cnt_h & DMA_CNT_ENABLE) != 0 &&
            (dma->cnt_h & DMA_CNT_MODE_MASK) == 0)
            dma_transfer(dma, bus);
    }
}
