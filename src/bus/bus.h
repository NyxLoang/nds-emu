#ifndef NDS_EMU_BUS_H
#define NDS_EMU_BUS_H

#include <stdint.h>
#include <stddef.h>

/* Main RAM：NDS 主内存，ARM9 镜像装载处，共 4MB。 */
#define BUS_MAIN_RAM_SIZE (4 * 1024 * 1024)

/* VRAM：显存，程序往这里写颜色字；本阶段先分配 656KB 区间。 */
#define BUS_VRAM_SIZE (656 * 1024)

/* ARM7 专属 WRAM：64KB（阶段 8，ARM7 镜像装载于此）。 */
#define BUS_ARM7_WRAM_SIZE (64 * 1024)

/* 地址区间基址（内存地图见 docs/03-memory-map.md） */
#define BUS_MAIN_RAM_BASE 0x02000000u
#define BUS_VRAM_BASE     0x06000000u
#define BUS_IO_BASE       0x04000000u
#define BUS_IO_SIZE       0x00010000u   /* IO 区间 64KB，具体寄存器由 io 模块实现 */
#define BUS_ARM7_WRAM_BASE 0x03800000u  /* ARM7 WRAM 基址 */

/* IPC FIFO RECV 的独占地址（不在 0x0400xxxx IO 区间内，单独映射） */
#define BUS_IPC_FIFO_RECV 0x04100000u

/* 前向声明：bus 只存指针，IO 寄存器语义在 src/io/ 模块实现（阶段 6） */
typedef struct io io_t;

/* 总线：持有各内存数组，按地址区间换算读写。
   阶段 2：只建数组与区间范围；阶段 6 起 IO 区间转发给 io 模块；
   阶段 8 起加 ARM7 WRAM 与「当前访问者身份」（FIFO/中断按 CPU 分流）。 */
typedef struct bus {
    uint8_t  main_ram[BUS_MAIN_RAM_SIZE]; /* Main RAM：4MB */
    uint8_t  vram[BUS_VRAM_SIZE];         /* VRAM：656KB */
    uint8_t  arm7_wram[BUS_ARM7_WRAM_SIZE]; /* ARM7 WRAM：64KB */
    io_t    *io;                          /* IO 寄存器区实现（由 nds 挂入） */
    int active_is_arm7;                   /* 当前访问者身份：0=ARM9, 1=ARM7 */
} bus_t;

bus_t *bus_create(void);
void bus_destroy(bus_t *bus);

/* 按 8 位读写一个字节。
   地址换算规则：把总线地址减去区间基址，得到该数组的下标
   （例如 Main RAM 基址 0x02000000，地址 0x02000100 → 下标 0x100）。
   未映射区间读返回 0，写忽略（保证任意地址访问不崩）。 */
uint8_t bus_read8(const bus_t *bus, uint32_t addr);
void bus_write8(bus_t *bus, uint32_t addr, uint8_t val);

/* 16/32 位读写（小端：低地址先放低字节）。
   实现方式：先用 read8/write8 逐字节读写，再按小端拼/拆，
   保证跨区间边界（如 Main RAM 末尾 3 字节处读 32 位）也能逐段换算。 */
uint16_t bus_read16(const bus_t *bus, uint32_t addr);
void bus_write16(bus_t *bus, uint32_t addr, uint16_t val);
uint32_t bus_read32(const bus_t *bus, uint32_t addr);
void bus_write32(bus_t *bus, uint32_t addr, uint32_t val);

#endif /* NDS_EMU_BUS_H */
