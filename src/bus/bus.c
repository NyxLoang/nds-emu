#include <stdio.h>
#include <stdlib.h>
#include "bus.h"
#include "io/io.h"
#include "bios/bios7_image.h"

bus_t *bus_create(void)
{
    /* calloc 清零，保证未写入区域读 0，符合未初始化内存约定 */
    bus_t *bus = calloc(1, sizeof(bus_t));
    if (bus == NULL)
        return NULL;
    bus_vram_reset_default(bus);
    return bus;
}

void bus_vram_reset_default(bus_t *bus)
{
    if (bus == NULL)
        return;
    for (int i = 0; i < 0x20; i++) bus->vram_map_abg[i] = 0;
    for (int i = 0; i < 0x10; i++) bus->vram_map_aobj[i] = 0;
    for (int i = 0; i < 0x8; i++) {
        bus->vram_map_bbg[i] = 0;
        bus->vram_map_bobj[i] = 0;
    }
    for (int i = 0; i < 4; i++) bus->vram_map_tex[i] = 0;
    for (int i = 0; i < 4; i++) {
        bus->vram_map_abg_ext[i] = 0;
        bus->vram_map_bbg_ext[i] = 0;
    }
    bus->vram_map_aobj_ext = 0;
    bus->vram_map_bobj_ext = 0;
    /* 默认映射（阶段 9 的 libnds vramDefault 布局）：
       A→Engine A BG、B→Engine A OBJ、C→Engine B BG、D→Engine B OBJ。
       FFXII 启动后写 VRAMCNT 会覆盖为参考配置。 */
    for (int i = 0; i < 8; i++) {
        bus->vram_map_abg[i] |= 1u << 0;
        bus->vram_map_aobj[i] |= 1u << 1;
        bus->vram_map_bbg[i] |= 1u << 2;
        bus->vram_map_bobj[i] |= 1u << 3;
    }
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

/* 21-B9xj：写监视。addr 落在任一 [lo,hi) 时打印写入者身份与 PC，
   用于“这个寄存器是谁改的”这类 bring-up 定位（lo==hi 表示该组关闭）。 */
extern unsigned long long g_dbg_frame;   /* 定义在 io.c（runner 每帧更新） */

void bus_set_watch(bus_t *bus, int idx, uint32_t lo, uint32_t hi)
{
    if (bus == NULL || idx < 0 || idx >= 4)
        return;
    bus->watch_lo[idx] = lo;
    bus->watch_hi[idx] = hi;
}

void bus_set_watch_read(bus_t *bus, int idx, uint32_t lo, uint32_t hi)
{
    if (bus == NULL || idx < 0 || idx >= 4)
        return;
    bus->watch_r_lo[idx] = lo;
    bus->watch_r_hi[idx] = hi;
}

/* 读监视：把读到的值与读取者一起打出来（用于“谁在什么时候取走了报文”）。 */
static void bus_dbg_watch_read(const bus_t *bus, uint32_t addr, int width,
                               uint32_t val)
{
    for (int i = 0; i < 4; i++) {
        if (bus->watch_r_lo[i] == bus->watch_r_hi[i])
            continue;
        if (addr < bus->watch_r_lo[i] || addr >= bus->watch_r_hi[i])
            continue;
        printf("watchr: arm%d w%d a=%08X v=%08X pc=%08X lr=%08X sp=%08X f=%llu\n",
               bus->active_is_arm7 ? 7 : 9, width * 8, addr, val,
               bus->dbg_pc, bus->dbg_lr, bus->dbg_sp, g_dbg_frame);
        return;
    }
}

static void bus_dbg_watch(const bus_t *bus, uint32_t addr, int width,
                          uint32_t val)
{
    for (int i = 0; i < 4; i++) {
        if (bus->watch_lo[i] == bus->watch_hi[i])
            continue;
        if (addr < bus->watch_lo[i] || addr >= bus->watch_hi[i])
            continue;
        printf("watch: arm%d w%d a=%08X v=%08X pc=%08X lr=%08X sp=%08X"
               " cpsr=%08X st=%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X f=%llu\n",
               bus->active_is_arm7 ? 7 : 9, width * 8, addr, val,
               bus->dbg_pc, bus->dbg_lr, bus->dbg_sp,
               bus->dbg_cpsr,
               bus_read32(bus, bus->dbg_sp), bus_read32(bus, bus->dbg_sp + 4u),
               bus_read32(bus, bus->dbg_sp + 8u),
               bus_read32(bus, bus->dbg_sp + 12u),
               bus_read32(bus, bus->dbg_sp + 16u),
               bus_read32(bus, bus->dbg_sp + 20u),
               bus_read32(bus, bus->dbg_sp + 24u),
               bus_read32(bus, bus->dbg_sp + 28u), g_dbg_frame);
        return;
    }
}

/* 宽写（16/32 位）拆分过程中抑制 8 位监视，避免同一次写打印多行。 */
static int g_wide_write = 0;
/* 宽读（16/32 位）组合过程中抑制 8 位监视，避免同一次读打印多行。 */
static int g_wide_read = 0;

static uint8_t bus_read8_core(const bus_t *bus, uint32_t addr);

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

/* ---- 21-B9wd：VRAMCNT 动态映射（端口自 melonDS GPU::MapVRAM_*） ---- */

/* 物理 bank 在 bus->vram 数组里的基址与大小/掩码 */
static const uint32_t vram_bank_phys[9] = {
    0x00000u, 0x20000u, 0x40000u, 0x60000u, 0x80000u,
    0x90000u, 0x94000u, 0x98000u, 0xA0000u
};
static const uint32_t vram_bank_mask[9] = {
    0x1FFFFu, 0x1FFFFu, 0x1FFFFu, 0x1FFFFu, 0x0FFFFu,
    0x03FFFu, 0x03FFFu, 0x07FFFu, 0x03FFFu
};

static void vram_clear_bank(bus_t *bus, uint32_t bankmask)
{
    for (int i = 0; i < 0x20; i++) bus->vram_map_abg[i] &= ~bankmask;
    for (int i = 0; i < 0x10; i++) bus->vram_map_aobj[i] &= ~bankmask;
    for (int i = 0; i < 0x8; i++) {
        bus->vram_map_bbg[i] &= ~bankmask;
        bus->vram_map_bobj[i] &= ~bankmask;
    }
    for (int i = 0; i < 4; i++) bus->vram_map_tex[i] &= ~bankmask;
    for (int i = 0; i < 4; i++) {
        bus->vram_map_abg_ext[i] &= ~bankmask;
        bus->vram_map_bbg_ext[i] &= ~bankmask;
    }
    bus->vram_map_aobj_ext &= ~bankmask;
    bus->vram_map_bobj_ext &= ~bankmask;
}

static void vram_set_abg(bus_t *bus, int slot, int n, uint32_t mask)
{
    for (int i = 0; i < n; i++)
        bus->vram_map_abg[slot + i] |= mask;
}

static void vram_set_aobj(bus_t *bus, int slot, int n, uint32_t mask)
{
    for (int i = 0; i < n; i++)
        bus->vram_map_aobj[slot + i] |= mask;
}

static void vram_set_bbg(bus_t *bus, int n, uint32_t mask)
{
    for (int i = 0; i < n; i++)
        bus->vram_map_bbg[i] |= mask;
}

static void vram_set_bobj(bus_t *bus, int n, uint32_t mask)
{
    for (int i = 0; i < n; i++)
        bus->vram_map_bobj[i] |= mask;
}

void bus_set_vramcnt(bus_t *bus, int bank, uint8_t cnt)
{
    if (bus == NULL || bank < 0 || bank >= 9)
        return;
    uint8_t old = bus->vramcnt[bank];
    bus->vramcnt[bank] = cnt;
    if (old == cnt)
        return;

    uint32_t bankmask = 1u << bank;
    vram_clear_bank(bus, bankmask);
    if (!(cnt & 0x80u))
        return;

    if (bank <= 1) { /* A/B：mask 0x9B */
        unsigned ofs = (cnt >> 3) & 3u;
        switch (cnt & 3u) {
        case 1: vram_set_abg(bus, (int)(ofs << 3), 8, bankmask); break;
        case 2: vram_set_aobj(bus, (int)((ofs & 1u) << 3), 8, bankmask); break;
        case 3: bus->vram_map_tex[ofs] |= bankmask; break;
        default: break; /* LCDC：本项目暂不建模 */
        }
    } else if (bank <= 3) { /* C/D：mask 0x9F */
        unsigned ofs = (cnt >> 3) & 7u;
        switch (cnt & 7u) {
        case 1: vram_set_abg(bus, (int)(ofs << 3), 8, bankmask); break;
        case 3: bus->vram_map_tex[ofs & 3u] |= bankmask; break;
        case 4:
            if (bank == 2) vram_set_bbg(bus, 8, bankmask);
            else           vram_set_bobj(bus, 8, bankmask);
            break;
        default: break; /* LCDC/ARM7：暂不建模 */
        }
    } else if (bank == 4) { /* E：mask 0x87 */
        switch (cnt & 7u) {
        case 1: vram_set_abg(bus, 0, 4, bankmask); break;
        case 2: vram_set_aobj(bus, 0, 4, bankmask); break;
        default: break; /* LCDC/纹理调色板/扩展调色板：暂不建模 */
        }
    } else if (bank == 7) { /* H：mask 0x87，当前 FFXII 配置为 B OBJ */
        if ((cnt & 7u) == 2) {
            vram_set_bobj(bus, 8, bankmask);
            /* melonDS MapVRAM_H：mode 2 同时把 H 接到 Engine B BG 扩展调色板 */
            for (int i = 0; i < 4; i++)
                bus->vram_map_bbg_ext[i] |= bankmask;
        }
    }
    /* F/G/I：FFXII 当前配置不用于 2D 引擎，后续需要时按 melonDS 补齐 */
}

uint16_t bus_vram_extpal16(const bus_t *bus, int is_sub, int slot,
                           unsigned pal, unsigned color)
{
    if (bus == NULL || slot < 0 || slot >= 4 || pal >= 16 || color >= 256)
        return 0;
    uint32_t mask = is_sub ? bus->vram_map_bbg_ext[slot]
                           : bus->vram_map_abg_ext[slot];
    if (mask == 0)
        return 0;
    int bank = 0;
    while (bank < 9 && !(mask & (1u << bank)))
        bank++;
    if (bank >= 9)
        return 0;
    size_t off = (size_t)slot * 0x2000u + (size_t)pal * 0x200u
               + (size_t)color * 2u;
    const uint8_t *p = bus->vram + vram_bank_phys[bank] + off;
    return (uint16_t)(p[0] | (uint16_t)(p[1] << 8));
}

/* DISPCNT VRAM 显示模式（bit16-17=2）按物理 bank 直读，不经逻辑窗口换算。 */
uint16_t bus_vram_phys16(const bus_t *bus, int bank, uint32_t off)
{
    if (bus == NULL || bank < 0 || bank > 8)
        return 0;
    uint32_t size = vram_bank_mask[bank] + 1u;
    if (off + 2u > size)
        return 0;
    const uint8_t *p = bus->vram + vram_bank_phys[bank] + off;
    return (uint16_t)(p[0] | (uint16_t)(p[1] << 8));
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

/* 21-B9wd：逻辑 VRAM 窗口按 VRAMCNT 映射到物理 bank。
   范围与 melonDS GPU 一致：A BG=0x06000000、B BG=0x06200000、
   A OBJ=0x06400000、B OBJ=0x06600000，每窗 2MB 地址空间里用 16KB 槽查掩码。 */
static int bus_vram_resolve(const bus_t *bus, uint32_t addr,
                            const uint8_t **region, size_t *off)
{
    if (addr < 0x06000000u || addr >= 0x06800000u)
        return 0;
    uint32_t mask;
    if (addr < 0x06200000u)
        mask = bus->vram_map_abg[(addr >> 14) & 0x1Fu];
    else if (addr < 0x06400000u)
        mask = bus->vram_map_bbg[(addr >> 14) & 0x7u];
    else if (addr < 0x06600000u)
        mask = bus->vram_map_aobj[(addr >> 14) & 0xFu];
    else
        mask = bus->vram_map_bobj[(addr >> 14) & 0x7u];

    if (mask == 0)
        return 0; /* 未映射：读 0 / 写忽略 */
    int bank = 0;
    while (bank < 9 && !(mask & (1u << bank)))
        bank++;
    if (bank >= 9)
        return 0;
    *region = bus->vram + vram_bank_phys[bank];
    *off = (size_t)(addr & vram_bank_mask[bank]);
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
    /* 21-B9wd：Engine A/B 的 BG/OBJ 逻辑窗口先按 VRAMCNT 映射解析 */
    if (addr >= 0x06000000u && addr < 0x06800000u)
        return bus_vram_resolve(bus, addr, region, off);
    /* LCDC/显示捕获区按物理 bank 分窗口映射（melonDS ReadVRAM_LCDC）：
       0x06800000 起每 128KB 对应 bank A-D；E/F/G/H/I 各在固定小窗。 */
    if (addr >= BUS_LCDC_VRAM_BASE && addr < 0x068A4000u) {
        uint32_t rel = addr - BUS_LCDC_VRAM_BASE;
        int bank;
        if (rel < 0x20000u)            bank = 0;
        else if (rel < 0x40000u)       bank = 1;
        else if (rel < 0x60000u)       bank = 2;
        else if (rel < 0x80000u)       bank = 3;
        else if (rel < 0x90000u)       bank = 4;
        else if (rel < 0x94000u)       bank = 5;
        else if (rel < 0x98000u)       bank = 6;
        else if (rel < 0xA0000u)       bank = 7;
        else                           bank = 8;
        *region = bus->vram + vram_bank_phys[bank];
        *off = (size_t)(addr & vram_bank_mask[bank]);
        return 1;
    }
    /* ARM7 WRAM：64KB（阶段 8，ARM7 镜像装载于此；ARM9 也可访问）。
       21-B9wt：ARM7 视角下 0x03800000-0x03FFFFFF 是 64KB 步长的镜像区
       （参考核 memregion_WRAM7 口径），0x03FFFFFC 与 0x0380FFFC 指向同一
       字节。FreeBIOS 的 IRQ handler 槽（[0x04000000-4] = 0x03FFFFFC）与
       IntrWait 轮询的软件中断标志（0x03FFFFF8）都在这个镜像顶上，缺了
       镜像会让 ARM7 读回 0、跳不到用户 handler。 */
    if (bus->active_is_arm7 && addr >= BUS_ARM7_WRAM_BASE &&
        addr < 0x04000000u) {
        *region = bus->arm7_wram;
        *off = (size_t)(addr & (BUS_ARM7_WRAM_SIZE - 1));
        return 1;
    }
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

/* 21-B9xp：读监视包装。宽读（16/32 位）期间抑制字节级打印，只按访问宽度打印一次。 */
uint8_t bus_read8(const bus_t *bus, uint32_t addr)
{
    uint8_t v = bus_read8_core(bus, addr);
    if (bus->diag && !g_wide_read)
        bus_dbg_watch_read(bus, addr, 1, v);
    return v;
}

static uint8_t bus_read8_core(const bus_t *bus, uint32_t addr)
{
    /* 21-B9xq：GBA 扩展槽（0x08000000-0x0BFFFFFF）空槽按参考核返回 0xFF。
       FFXII 启动时用 DMA1 从 0x08000080 取 0x40 字节填 0x02079920-0x0207995F：
       空槽 → 全 0xFFFF → 该表尾段 0xFFFF 被拷进 0x027FFC30（见 21-B9xo）。
       本地此前未映射该区间（读 0），于是填成 0x0000，导致 ARM7 调度器闸门
       [0x0380BA7C] 不置 1、心跳条数偏多。 */
    if (addr >= 0x08000000u && addr < 0x0C000000u)
        return 0xFFu;
    /* IO 区间转发给 io 模块（真实寄存器语义），未挂 io 时读 0 兜底。
       active_is_arm7 让中断/FIFO CNT 等按访问者身份分流。 */
    if (addr >= BUS_IO_BASE && addr - BUS_IO_BASE < BUS_IO_SIZE)
        return bus->io != NULL ? io_read8(bus->io, addr, bus->active_is_arm7) : 0;
    /* ARM7 低地址 0x0000-0x3FFF 是 BIOS ROM：整段返回 FreeBIOS 镜像字节
       （21-B9wt 起提供「会被读的字节」，21-B9yd 起换成完整 0x4000 镜像）。
       这样两类路径与参考核一致：
       <1> 游戏恢复 BIOS 内部现场后，SWI 分发器从 [lr-2] 取编号字节
           （0x08 处的 `b swi_handler` 在 [lr-2] 读到 0x04）；
       <2> 未被 bios7_low.c 逐条建模的 SWI 函数体（Div/CpuSet/CRC16/音频查表…）
           以及异常向量，直接在真地址上取指并执行真实字节。 */
    if (bus->active_is_arm7 && addr < 0x4000u) {
        uint8_t v;
        if (bios7_image_read8(addr, &v))
            return v;
    }
    const uint8_t *region;
    size_t off;
    if (!bus_resolve(bus, addr, &region, &off))
        return 0; /* 未映射空间：读返回 0 */
    return region[off];
}

void bus_write8(bus_t *bus, uint32_t addr, uint8_t val)
{
    if (bus->diag && !g_wide_write)
        bus_dbg_watch(bus, addr, 1, val);
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
    g_wide_read = 1;
    uint16_t v = (uint16_t)bus_read8(bus, addr)
               | ((uint16_t)bus_read8(bus, addr + 1) << 8);
    g_wide_read = 0;
    if (bus->diag)
        bus_dbg_watch_read(bus, addr, 2, v);
    return v;
}

/* 小端 16 位写：反向拆字节，最低字节落到低地址。 */
void bus_write16(bus_t *bus, uint32_t addr, uint16_t val)
{
    if (bus->diag)
        bus_dbg_watch(bus, addr, 2, val);
    g_wide_write = 1;
    bus_write8(bus, addr, (uint8_t)(val & 0xFF));         /* 最低字节 → addr */
    bus_write8(bus, addr + 1, (uint8_t)(val >> 8));       /* 最高字节 → addr+1 */
    g_wide_write = 0;
}

/* 小端 32 位：四个字节按 b0<<0 | b1<<8 | b2<<16 | b3<<24 拼成字。 */
uint32_t bus_read32(const bus_t *bus, uint32_t addr)
{
    /* IPC FIFO RECV（0x04100000）在 IO 区间外，需整体读（拆字节会破坏队列） */
    if (addr == BUS_IPC_FIFO_RECV) {
        uint32_t v = bus->io != NULL ? io_recv32(bus->io, bus->active_is_arm7) : 0;
        if (bus->diag)
            bus_dbg_watch_read(bus, addr, 4, v);
        return v;
    }
    /* 卡带数据端口 CARD_DATA（0x04100010）在 IO 区间外，需整体读（读自动推进地址） */
    if (addr == BUS_CARD_DATA)
    {
        /* 21-B9yi 诊断：NDS_CARTLOG2=LO-HI 时打印每次数据端口读的（帧, CPSR）,
           用于对照「参考核的块读是否在关中断状态下原子完成」 */
        {
            extern unsigned long long g_dbg_frame;
            static long lo = -2, hi = -1;
            if (lo == -2) {
                const char *e = getenv("NDS_CARTLOG2");
                lo = 0; hi = -1;
                if (e != NULL && sscanf(e, "%ld-%ld", &lo, &hi) != 2) { lo = 0; hi = -1; }
            }
            {
                extern int g_dma_active; /* 定义在 io/dma.c（21-B9yi 诊断） */
                if ((long)g_dbg_frame >= lo && (long)g_dbg_frame <= hi)
                    printf("cartrd: f=%llu cpsr=%08X pc=%08X src=%s\n",
                           g_dbg_frame, bus->dbg_cpsr, bus->dbg_pc,
                           g_dma_active ? "dma" : "cpu");
            }
        }
        uint32_t v = bus->io != NULL ? io_card_data_read32(bus->io) : 0xFFFFFFFFu;
        if (bus->diag)
            bus_dbg_watch_read(bus, addr, 4, v);
        return v;
    }
    g_wide_read = 1;
    uint32_t v = (uint32_t)bus_read8(bus, addr)
               | ((uint32_t)bus_read8(bus, addr + 1) << 8)
               | ((uint32_t)bus_read8(bus, addr + 2) << 16)
               | ((uint32_t)bus_read8(bus, addr + 3) << 24);
    g_wide_read = 0;
    if (bus->diag)
        bus_dbg_watch_read(bus, addr, 4, v);
    return v;
}

/* 小端 32 位写：最低字节 → addr，最高字节 → addr+3。 */
void bus_write32(bus_t *bus, uint32_t addr, uint32_t val)
{
    /* IPC FIFO SEND（0x04000188）是 32 位寄存器，需整体入队（拆字节会被忽略） */
    if (addr == IO_FIFO_SEND) {
        if (bus->diag)
            bus_dbg_watch(bus, addr, 4, val);   /* 21-B9xr：FIFO 发送走特例，需单独挂钩 */
        if (bus->io != NULL)
            io_send32(bus->io, bus->active_is_arm7, val);
        return;
    }
    /* 卡带数据端口 CARD_DATA 写（本阶段占位：读 ROM 用不到） */
    if (addr == BUS_CARD_DATA) {
        if (bus->diag)
            bus_dbg_watch(bus, addr, 4, val);
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
    if (bus->diag)
        bus_dbg_watch(bus, addr, 4, val);
    g_wide_write = 1;
    bus_write8(bus, addr,     (uint8_t)(val & 0xFF));
    bus_write8(bus, addr + 1, (uint8_t)(val >> 8));
    bus_write8(bus, addr + 2, (uint8_t)(val >> 16));
    bus_write8(bus, addr + 3, (uint8_t)(val >> 24));
    g_wide_write = 0;
}
