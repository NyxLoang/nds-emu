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

void bus_set_arm9_dtcm(bus_t *bus, int enabled, uint32_t base, uint32_t size)
{
    if (bus == NULL)
        return;
    if (size > BUS_ARM9_DTCM_SIZE)
        size = BUS_ARM9_DTCM_SIZE;
    bus->arm9_dtcm_on = enabled && size > 0;
    bus->arm9_dtcm_base = bus->arm9_dtcm_on ? base : 0xFFFFFFFFu;
    bus->arm9_dtcm_size = bus->arm9_dtcm_on ? size : 0;
}

/* Shared WRAM 按 WRAMCNT 低 2 位 + 当前访问者切分（阶段 21-B7）。
   真机整片 0x03xxxxxx 区域会按每个核的「基址指针 + 掩码」重复别名，
   这里只处理本项目映射的两个 32KB 口（0x03000000 主区 / 0x037F8000 镜像），
   换算采用同一套「指针 + addr & mask」，与 melonDS MapSharedWRAM 对应：
   - 0：全给 ARM9（ARM7 转而看到自己的 ARM7 WRAM：主区=低 32KB、镜像=高 32KB）
   - 1：ARM9 高 16KB、ARM7 低 16KB（各自 16KB 窗口在镜像区同样重复）
   - 2：ARM9 低 16KB、ARM7 高 16KB
   - 3：全给 ARM7（ARM9 读 0、写忽略；直接启动默认态）
   未分得的窗口读 0、写忽略（与本项目「未映射读 0 写忽略」约定一致）。 */
static int bus_shared_resolve(const bus_t *bus, uint32_t addr,
                              const uint8_t **region, size_t *off)
{
    const uint8_t *mem = NULL;
    uint32_t mask = 0;
    uint8_t mode = bus->io != NULL ? (uint8_t)(bus->io->memctl.wramcnt & 3u)
                                   : (uint8_t)IO_WRAMCNT_INIT;

    if (!bus->active_is_arm7) {
        switch (mode) {
        case 0: mem = bus->shared_wram;         mask = 0x7FFFu; break;
        case 1: mem = bus->shared_wram + 0x4000; mask = 0x3FFFu; break;
        case 2: mem = bus->shared_wram;         mask = 0x3FFFu; break;
        default: return 0; /* case 3：Shared WRAM 全归 ARM7 */
        }
    } else {
        switch (mode) {
        case 0: mem = bus->arm7_wram;           mask = 0xFFFFu; break;
        case 1: mem = bus->shared_wram;         mask = 0x3FFFu; break;
        case 2: mem = bus->shared_wram + 0x4000; mask = 0x3FFFu; break;
        case 3: mem = bus->shared_wram;         mask = 0x7FFFu; break;
        default: return 0;
        }
    }
    *region = mem;
    *off = (size_t)(addr & mask);
    return 1;
}

/* 地址换算核心：判断 addr 落在哪个内存区间，填出「区间数组指针 + 偏移」。
   命中返回 1；未映射（含 IO 桩区间）返回 0，调用方按「读 0 / 写忽略」处理。
   换算规则：区间内下标 = addr - 区间基址（如 0x02000100 - 0x02000000 = 0x100）。
   用「先减后比较」避免 addr 小于基址时无符号减法下溢误命中。 */
static int bus_resolve(const bus_t *bus, uint32_t addr,
                       const uint8_t **region, size_t *off)
{
    /* ARM9 ITCM（阶段 21-B8）：固定 0x01FF8000-0x01FFFFFF，只有 ARM9 能访问。
       FFXII 复位从 0x02070420 拷贝 0x6BC0 字节到此处作为系统例程。 */
    if (!bus->active_is_arm7 &&
        addr >= BUS_ARM9_ITCM_BASE &&
        addr - BUS_ARM9_ITCM_BASE < BUS_ARM9_ITCM_SIZE) {
        *region = bus->arm9_itcm;
        *off = (size_t)(addr - BUS_ARM9_ITCM_BASE);
        return 1;
    }
    if (addr >= BUS_MAIN_RAM_BASE &&
        addr - BUS_MAIN_RAM_BASE < BUS_MAIN_RAM_SIZE) {
        *region = bus->main_ram;
        *off = (size_t)(addr - BUS_MAIN_RAM_BASE);
        return 1;
    }
    /* ARM9 DTCM（阶段 21-B8）：FFXII 复位把 DTCM 配到 0x027E0000 并把栈建在
       0x027E0000-0x027E3FFF。DTCM 在 ARM9 地址空间内优先于 Main RAM 镜像，
       ARM7 看不到 DTCM，仍访问同一地址下的 Main RAM（两套物理内存不冲突）。 */
    if (!bus->active_is_arm7 && bus->arm9_dtcm_on &&
        addr >= bus->arm9_dtcm_base &&
        addr - bus->arm9_dtcm_base < bus->arm9_dtcm_size) {
        *region = bus->arm9_dtcm;
        *off = (size_t)(addr - bus->arm9_dtcm_base);
        return 1;
    }
    /* Main RAM 无缓存镜像：0x02400000 起 4MB，与主区同一物理数组（别名，阶段 21-B3）。
       换算规则与主区相同：区间内下标 = addr - 镜像基址。
       21-B8：镜像本身只是 ARM9 的“绕过缓存”通道，但同一段 0x024-0x027 地址
       在 ARM7 总线也解码到这块 4MB 主存（melonDS ARM7 也命中 0x02000000/
       0x02800000 两个窗口）。之前把 ARM7 拦在镜像外是因为没实现 DTCM——ARM7
       拷贝会覆盖“看似 ARM9 栈”的主存字节；真正原因是那些字节应属于 ARM9
       DTCM，与主存镜像无关。 */
    if (addr >= BUS_MAIN_RAM_MIRROR_BASE &&
        addr - BUS_MAIN_RAM_MIRROR_BASE < BUS_MAIN_RAM_SIZE) {
        *region = bus->main_ram;
        *off = (size_t)(addr - BUS_MAIN_RAM_MIRROR_BASE);
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
    /* Shared WRAM 主区：32KB（阶段 21-B1 映射，21-B7 起按 WRAMCNT 双核切分） */
    if (addr >= BUS_SHARED_WRAM_BASE &&
        addr - BUS_SHARED_WRAM_BASE < BUS_SHARED_WRAM_SIZE)
        return bus_shared_resolve(bus, addr, region, off);
    /* Shared WRAM 镜像区：0x037F8000 起 32KB，与主区同一物理数组的别名口 */
    if (addr >= BUS_SHARED_WRAM_MIRROR &&
        addr - BUS_SHARED_WRAM_MIRROR < BUS_SHARED_WRAM_SIZE)
        return bus_shared_resolve(bus, addr, region, off);
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
