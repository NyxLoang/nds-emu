#include <stdlib.h>
#include "bus.h"
#include "io/io.h"

bus_t *bus_create(void)
{
    /* calloc 清零，保证未写入区域读 0，符合未初始化内存约定 */
    return calloc(1, sizeof(bus_t));
}

void bus_destroy(bus_t *bus)
{
    free(bus);
}

void bus_set_diag(bus_t *bus, int on)
{
    if (bus != NULL)
        bus->diag = on;
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
    /* ARM7 WRAM：64KB（阶段 8，ARM7 镜像装载于此；ARM9 也可访问） */
    if (addr >= BUS_ARM7_WRAM_BASE &&
        addr - BUS_ARM7_WRAM_BASE < BUS_ARM7_WRAM_SIZE) {
        *region = bus->arm7_wram;
        *off = (size_t)(addr - BUS_ARM7_WRAM_BASE);
        return 1;
    }
    /* Shared WRAM 主区：32KB（阶段 21-B1，真 ROM 启动时 ARM7 拷贝代码到 0x03000000 后跳入执行） */
    if (addr >= BUS_SHARED_WRAM_BASE &&
        addr - BUS_SHARED_WRAM_BASE < BUS_SHARED_WRAM_SIZE) {
        *region = bus->shared_wram;
        *off = (size_t)(addr - BUS_SHARED_WRAM_BASE);
        return 1;
    }
    /* Shared WRAM 镜像区：0x037F8000 起 32KB，与主区同一物理数组（别名）。
       换算规则与主区相同：区间内下标 = addr - 镜像基址。 */
    if (addr >= BUS_SHARED_WRAM_MIRROR &&
        addr - BUS_SHARED_WRAM_MIRROR < BUS_SHARED_WRAM_SIZE) {
        *region = bus->shared_wram;
        *off = (size_t)(addr - BUS_SHARED_WRAM_MIRROR);
        return 1;
    }
    /* 调色板 RAM：2KB（阶段 9，BG/OBJ 颜色查表） */
    if (addr >= BUS_PALETTE_BASE &&
        addr - BUS_PALETTE_BASE < BUS_PALETTE_SIZE) {
        *region = bus->palette;
        *off = (size_t)(addr - BUS_PALETTE_BASE);
        return 1;
    }
    /* OAM：2KB（阶段 9，OBJ 属性内存，主/副各 1KB） */
    if (addr >= BUS_OAM_BASE &&
        addr - BUS_OAM_BASE < BUS_OAM_SIZE) {
        *region = bus->oam;
        *off = (size_t)(addr - BUS_OAM_BASE);
        return 1;
    }
    /* VRAM 固定窗口（阶段 9 最小实现）：副 BG / 主 OBJ / 副 OBJ 各映射到一段
       固定物理 VRAM（对应 libnds vramDefault 的 bank C/B/D 分配）。 */
    if (addr >= BUS_VRAM_SUB_BG_BASE &&
        addr - BUS_VRAM_SUB_BG_BASE < BUS_VRAM_WINDOW_SIZE) {
        *region = bus->vram + BUS_VRAM_SUB_BG_PHYS;
        *off = (size_t)(addr - BUS_VRAM_SUB_BG_BASE);
        return 1;
    }
    if (addr >= BUS_VRAM_MAIN_OBJ_BASE &&
        addr - BUS_VRAM_MAIN_OBJ_BASE < BUS_VRAM_WINDOW_SIZE) {
        *region = bus->vram + BUS_VRAM_MAIN_OBJ_PHYS;
        *off = (size_t)(addr - BUS_VRAM_MAIN_OBJ_BASE);
        return 1;
    }
    if (addr >= BUS_VRAM_SUB_OBJ_BASE &&
        addr - BUS_VRAM_SUB_OBJ_BASE < BUS_VRAM_WINDOW_SIZE) {
        *region = bus->vram + BUS_VRAM_SUB_OBJ_PHYS;
        *off = (size_t)(addr - BUS_VRAM_SUB_OBJ_BASE);
        return 1;
    }
    /* LCDC 分配 VRAM（阶段 20.2 显示捕获目标）→ vram[0] */
    if (addr >= BUS_LCDC_VRAM_BASE &&
        addr - BUS_LCDC_VRAM_BASE < BUS_LCDC_VRAM_SIZE) {
        *region = bus->vram;
        *off = (size_t)(addr - BUS_LCDC_VRAM_BASE);
        return 1;
    }
    /* IO 寄存器区不在这里命中（阶段 6 起由 io 模块处理），返回 0 表示非内存数组 */
    return 0;
}

uint8_t bus_read8(const bus_t *bus, uint32_t addr)
{
    /* IO 区间转发给 io 模块（真实寄存器语义），未挂 io 时读 0 兜底。
       active_is_arm7 让中断/FIFO CNT 等按访问者身份分流。 */
    if (addr >= BUS_IO_BASE && addr - BUS_IO_BASE < BUS_IO_SIZE)
        return bus->io != NULL ? io_read8(bus->io, addr, bus->active_is_arm7) : 0;
    const uint8_t *region;
    size_t off;
    if (!bus_resolve(bus, addr, &region, &off))
        return 0; /* 未映射空间：读返回 0 */
    return region[off];
}

void bus_write8(bus_t *bus, uint32_t addr, uint8_t val)
{
    /* IO 区间转发给 io 模块（含未实现寄存器写忽略的桩语义） */
    if (addr >= BUS_IO_BASE && addr - BUS_IO_BASE < BUS_IO_SIZE) {
        if (bus->io != NULL)
            io_write8(bus->io, addr, val, bus->active_is_arm7);
        return;
    }
    const uint8_t *region;
    size_t off;
    if (!bus_resolve(bus, addr, &region, &off))
        return; /* 未映射空间：写忽略 */
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
    /* IPC FIFO RECV（0x04100000）在 IO 区间外，需整体读（拆字节会破坏队列） */
    if (addr == BUS_IPC_FIFO_RECV)
        return bus->io != NULL ? io_recv32(bus->io, bus->active_is_arm7) : 0;
    /* 卡带数据端口 CARD_DATA（0x04100010）在 IO 区间外，需整体读（读自动推进地址） */
    if (addr == BUS_CARD_DATA)
        return bus->io != NULL ? io_card_data_read32(bus->io) : 0xFFFFFFFFu;
    return (uint32_t)bus_read8(bus, addr)
         | ((uint32_t)bus_read8(bus, addr + 1) << 8)
         | ((uint32_t)bus_read8(bus, addr + 2) << 16)
         | ((uint32_t)bus_read8(bus, addr + 3) << 24);
}

/* 小端 32 位写：最低字节 → addr，最高字节 → addr+3。 */
void bus_write32(bus_t *bus, uint32_t addr, uint32_t val)
{
    /* IPC FIFO SEND（0x04000188）是 32 位寄存器，需整体入队（拆字节会被忽略） */
    if (addr == IO_FIFO_SEND) {
        if (bus->io != NULL)
            io_send32(bus->io, bus->active_is_arm7, val);
        return;
    }
    /* 卡带数据端口 CARD_DATA 写（本阶段占位：读 ROM 用不到） */
    if (addr == BUS_CARD_DATA) {
        if (bus->io != NULL)
            io_card_data_write32(bus->io, val);
        return;
    }
    /* 几何命令区（0x04000400..0x040005FF）：ARM9 视角整体转发给 gx，
       拆字节会破坏「命令字 + 参数字」的 40 位命令语义。ARM7 视角仍走音频。 */
    if (addr >= GX_GXFIFO && addr < GX_CMD_PORT_END && !bus->active_is_arm7) {
        if (bus->io != NULL)
            io_gx_write32(bus->io, addr, val);
        return;
    }
    bus_write8(bus, addr,     (uint8_t)(val & 0xFF));
    bus_write8(bus, addr + 1, (uint8_t)(val >> 8));
    bus_write8(bus, addr + 2, (uint8_t)(val >> 16));
    bus_write8(bus, addr + 3, (uint8_t)(val >> 24));
}
