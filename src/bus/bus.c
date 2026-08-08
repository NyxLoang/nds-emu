#include <stdlib.h>
#include "bus.h"

bus_t *bus_create(void)
{
    /* calloc 清零，保证未写入区域读 0，符合未初始化内存约定 */
    return calloc(1, sizeof(bus_t));
}

void bus_destroy(bus_t *bus)
{
    free(bus);
}

/* 地址换算核心：判断 addr 落在哪个内存区间，填出「区间数组指针 + 偏移」。
   命中返回 1；未映射（含 IO 桩区间）返回 0，调用方按「读 0 / 写忽略」处理。
   换算规则：区间内下标 = addr - 区间基址（如 0x02000100 - 0x02000000 = 0x100）。
   用「先减后比较」避免 addr 小于基址时无符号减法下溢误命中。 */
static int bus_resolve(const bus_t *bus, uint32_t addr,
                       const uint8_t **region, size_t *off)
{
    if (addr >= BUS_MAIN_RAM_BASE &&
        addr - BUS_MAIN_RAM_BASE < BUS_MAIN_RAM_SIZE) {
        *region = bus->main_ram;
        *off = (size_t)(addr - BUS_MAIN_RAM_BASE);
        return 1;
    }
    /* VRAM：显存区间，换算方式与 Main RAM 相同（addr - 0x06000000 = VRAM 下标） */
    if (addr >= BUS_VRAM_BASE &&
        addr - BUS_VRAM_BASE < BUS_VRAM_SIZE) {
        *region = bus->vram;
        *off = (size_t)(addr - BUS_VRAM_BASE);
        return 1;
    }
    /* IO 寄存器区（0x04000000 起）：本阶段做桩——读 0、写忽略。
       显式列出该区间，便于后续微步（阶段 6 等）逐个实现真实寄存器。 */
    if (addr >= BUS_IO_BASE && addr - BUS_IO_BASE < BUS_IO_SIZE)
        return 0;
    /* 其余未映射空间：同样读 0 / 写忽略，保证任意地址访问不崩 */
    return 0;
}

uint8_t bus_read8(const bus_t *bus, uint32_t addr)
{
    const uint8_t *region;
    size_t off;
    if (!bus_resolve(bus, addr, &region, &off))
        return 0; /* IO / 未映射：读返回 0 */
    return region[off];
}

void bus_write8(bus_t *bus, uint32_t addr, uint8_t val)
{
    const uint8_t *region;
    size_t off;
    if (!bus_resolve(bus, addr, &region, &off))
        return; /* IO / 未映射：写忽略 */
    ((uint8_t *)region)[off] = val; /* region 指向调用方拥有的可变内存，cast 仅用于复用只读换算 */
}

/* 小端 16 位：把两个字节拼成一个半字。
   低地址字节是最低 8 位，高地址字节移到 <<8 位置（复习 docs/00b）。 */
uint16_t bus_read16(const bus_t *bus, uint32_t addr)
{
    return (uint16_t)bus_read8(bus, addr)
         | ((uint16_t)bus_read8(bus, addr + 1) << 8);
}

/* 小端 16 位写：反向拆字节，最低字节落到低地址。 */
void bus_write16(bus_t *bus, uint32_t addr, uint16_t val)
{
    bus_write8(bus, addr, (uint8_t)(val & 0xFF));         /* 最低字节 → addr */
    bus_write8(bus, addr + 1, (uint8_t)(val >> 8));       /* 最高字节 → addr+1 */
}

/* 小端 32 位：四个字节按 b0<<0 | b1<<8 | b2<<16 | b3<<24 拼成字。 */
uint32_t bus_read32(const bus_t *bus, uint32_t addr)
{
    return (uint32_t)bus_read8(bus, addr)
         | ((uint32_t)bus_read8(bus, addr + 1) << 8)
         | ((uint32_t)bus_read8(bus, addr + 2) << 16)
         | ((uint32_t)bus_read8(bus, addr + 3) << 24);
}

/* 小端 32 位写：最低字节 → addr，最高字节 → addr+3。 */
void bus_write32(bus_t *bus, uint32_t addr, uint32_t val)
{
    bus_write8(bus, addr,     (uint8_t)(val & 0xFF));
    bus_write8(bus, addr + 1, (uint8_t)(val >> 8));
    bus_write8(bus, addr + 2, (uint8_t)(val >> 16));
    bus_write8(bus, addr + 3, (uint8_t)(val >> 24));
}
