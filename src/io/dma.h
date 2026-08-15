#ifndef NDS_EMU_IO_DMA_H
#define NDS_EMU_IO_DMA_H

#include <stdint.h>

struct bus; /* 前向声明：必须在参数列表外声明，否则参数里的 struct bus 是另一个类型 */

/* DMA 通道寄存器（NDS9 与 NDS7 同址；本模拟器简化为一套共享通道）。
   每通道 0x0C：SAD/DAD 各 4 字节 + CNT_L/CNT_H 各 2 字节，共 4 通道。 */
#define IO_DMA0_BASE   0x040000B0u
#define IO_DMA_STRIDE  0x0Cu
#define IO_DMA_COUNT   4u
#define IO_DMA_END     0x040000E0u   /* 上界（不含），之后是 DMA0FILL 等 */

/* CNT_H 控制位（16 位寄存器，对应真机 32 位 CNT 的高 16 位，
   因此 bit15=CR bit31、bit10=CR bit26 …） */
#define DMA_CNT_ENABLE   (1u << 15)   /* 写 1 启动，非重复搬完自动清 0（=CR bit31） */
#define DMA_CNT_IRQ      (1u << 14)   /* 搬完置 IF（本阶段暂不接线）（=CR bit30） */
#define DMA_CNT_REPEAT   (1u << 9)    /* 重复：每个触发周期重搬同一块（=CR bit25） */
#define DMA_CNT_32BIT    (1u << 10)   /* 0=半字(16 位)  1=字(32 位)（=CR bit26） */
#define DMA_CNT_SRC_FIX  (1u << 8)    /* 源地址控制=2：固定（填色用）（=CR bit24） */
#define DMA_CNT_SRC_DEC  (1u << 7)    /* 源地址控制=1：递减（=CR bit23） */
#define DMA_CNT_DST_FIX  (1u << 6)    /* 目的地址控制=2：固定（=CR bit22） */
#define DMA_CNT_DST_DEC  (1u << 5)    /* 目的地址控制=1：递减（=CR bit21） */
#define DMA_CNT_DST_RELOAD (3u << 5)  /* 目的地址控制=3：增/重载（repeat 用）（=CR bit21-22） */

#define DMA_CNT_MODE_MASK  (0x7u << 11) /* 触发模式（=CR bit27-29） */
#define DMA_CNT_MODE_SHIFT 11

/* 触发模式值（DMA_CNT_MODE 字段） */
enum {
    DMA_START_IMMED  = 0,  /* 立即 */
    DMA_START_VBLANK = 1,  /* VBlank */
    DMA_START_HBLANK = 2,  /* HBlank（阶段 15 不触发，预留） */
    DMA_START_CARD   = 5,  /* 卡带 DRQ（真游戏读 ROM 的典型路径） */
};

/* 单个 DMA 通道。SAD/DAD 是源/目的地址；CNT_L 是字数；CNT_H 是控制。 */
typedef struct dma_channel {
    uint32_t sad;
    uint32_t dad;
    uint16_t cnt_l;
    uint16_t cnt_h;
} dma_channel_t;

/* 4 条通道（阶段 7 只实现 DMA0 一条；阶段 15 补齐全部 4 条）。 */
typedef struct dma {
    dma_channel_t ch[IO_DMA_COUNT];
} dma_t;

int dma_is_addr(uint32_t addr);
uint8_t dma_read8(const dma_t *dma, uint32_t addr);

/* 写寄存器；若写到某通道 CNT_H 高字节且使能位置位、模式=立即，则同步执行拷贝。
   bus 用于搬运（源/目的可能落在 Main RAM / VRAM / IO / 卡带 CARD_DATA 任意区间）。 */
void dma_write8(dma_t *dma, uint32_t addr, uint8_t val, struct bus *bus);

/* 触发指定触发模式（VBlank/卡带等）的所有已使能通道。立即模式不在此触发。 */
void dma_fire(dma_t *dma, struct bus *bus, int start_mode);

#endif /* NDS_EMU_IO_DMA_H */
