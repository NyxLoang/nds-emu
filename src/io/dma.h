#ifndef NDS_EMU_IO_DMA_H
#define NDS_EMU_IO_DMA_H

#include <stdint.h>

struct bus; /* 前向声明：必须在参数列表外声明，否则参数里的 struct bus 是另一个类型 */

/* DMA 通道寄存器（NDS9；本阶段只实现 DMA0 一条通道）。
   通道间距 0x0C：SAD/DAD 各 4 字节 + CNT_L/CNT_H 各 2 字节。 */
#define IO_DMA0_BASE   0x040000B0u
#define IO_DMA_STRIDE  0x0Cu
#define IO_DMA_COUNT   4u
#define IO_DMA_END     0x040000E0u   /* 上界（不含），之后是 DMA0FILL 等 */

/* DMA0CNT_H 控制位（CNT_H 是 16 位寄存器，对应真机 32 位 CR 的高 16 位，
   因此 bit15=CR bit31、bit10=CR bit26 …） */
#define DMA_CNT_ENABLE   (1u << 15)   /* 写 1 启动，搬完自动清 0（=CR bit31） */
#define DMA_CNT_32BIT    (1u << 10)   /* 0=半字(16 位)  1=字(32 位)（=CR bit26） */
#define DMA_CNT_SRC_FIX  (1u << 8)    /* 源地址固定（填色用）（=CR bit24） */
#define DMA_CNT_DST_FIX  (1u << 6)    /* 目的地址固定（=CR bit22） */
#define DMA_CNT_MODE_MASK (0x7u << 11)/* 触发模式：只支持 0=立即（=CR bit27-29） */

/* 单个 DMA 通道。SAD/DAD 是源/目的地址；CNT_L 是字数；CNT_H 是控制。 */
typedef struct dma_channel {
    uint32_t sad;
    uint32_t dad;
    uint16_t cnt_l;
    uint16_t cnt_h;
} dma_channel_t;

int dma_is_addr(uint32_t addr);
uint8_t dma_read8(const dma_channel_t *dma, uint32_t addr);

/* 写寄存器；若写到了 CNT_H 且使能位置位、模式=立即，则同步执行拷贝。
   bus 用于搬运（源/目的可能落在 Main RAM / VRAM / IO 任意区间）。 */
void dma_write8(dma_channel_t *dma, uint32_t addr, uint8_t val, struct bus *bus);

#endif /* NDS_EMU_IO_DMA_H */
