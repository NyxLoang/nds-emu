#ifndef NDS_EMU_BUS_H
#define NDS_EMU_BUS_H

#include <stdint.h>
#include <stddef.h>

/* Main RAM：NDS 主内存，ARM9 镜像装载处，共 4MB。
   ARM9 还有一个无缓存镜像区 0x02400000-0x027FFFFF（同一物理内存、同偏移别名），
   商业 ROM 常把栈放到该区（如 FFXII 用 0x027E0000 附近）以避开缓存。
   镜像内的 0x027E0000-0x027E3FFF 在 FFXII 复位时被 CP15 配置成 ARM9 的 DTCM
   （私有紧密耦合内存），因此 ARM7 对该地址的写访问仍会落到同一块 Main RAM，
   不会碰 ARM9 的 DTCM 栈（21-B8 修正 21-B6 的“仅 ARM9 可见”简化）。 */
#define BUS_MAIN_RAM_SIZE (4 * 1024 * 1024)

/* ARM9 DTCM：16KB 私有数据内存，基址/使能由 CP15 c9,c1,0 + c1 bit16 配置
   （ARM946E-S 的 Data TCM；NDS 上固定最大 16KB）。 */
#define BUS_ARM9_DTCM_SIZE (16 * 1024)

/* ARM9 ITCM：32KB 私有指令内存，固定映射在 0x01FF8000（ARM9 指令 TCM）。
   FFXII 复位时把自己的系统例程从 ARM9 镜像 0x02070420 拷到此处。 */
#define BUS_ARM9_ITCM_BASE 0x01FF8000u
#define BUS_ARM9_ITCM_SIZE (32 * 1024)

/* VRAM：显存，程序往这里写颜色字；本阶段先分配 656KB 区间。 */
#define BUS_VRAM_SIZE (656 * 1024)

/* ARM7 专属 WRAM：64KB（阶段 8，ARM7 镜像装载于此）。 */
#define BUS_ARM7_WRAM_SIZE (64 * 1024)

/* Shared WRAM：32KB（阶段 21-B1，双核共享内存，真 ROM 启动时 ARM7 会拷贝代码到此处执行）。
   真机该区有两个地址别名：主区 0x03000000 与镜像区 0x037F8000，均映射到同一数组。 */
#define BUS_SHARED_WRAM_SIZE (32 * 1024)

/* 调色板 RAM：2KB（阶段 9，BG/OBJ 颜色查表，主 0x05000000 + 副 0x05000400）。 */
#define BUS_PALETTE_SIZE 0x800u

/* OAM（OBJ 属性内存）：2KB（阶段 9，主 0x07000000 1KB + 副 0x07000400 1KB）。 */
#define BUS_OAM_SIZE 0x800u

/* VRAM 固定窗口（阶段 9 最小实现，对应 libnds vramDefault 的 bank 分配：
   bank A=主 BG、bank B=主 OBJ、bank C=副 BG、bank D=副 OBJ，每窗 128KB）。
   0x06000000 仍保持 656KB 全量映射（向后兼容 + 主引擎全量访问）。 */
#define BUS_VRAM_WINDOW_SIZE    (128 * 1024)
#define BUS_VRAM_SUB_BG_BASE    0x06200000u   /* 副 BG 图形窗口 */
#define BUS_VRAM_MAIN_OBJ_BASE  0x06400000u   /* 主 OBJ 图形窗口 */
#define BUS_VRAM_SUB_OBJ_BASE   0x06600000u   /* 副 OBJ 图形窗口 */
#define BUS_VRAM_SUB_BG_PHYS    0x40000u      /* 副 BG 窗口物理偏移（bank C） */
#define BUS_VRAM_MAIN_OBJ_PHYS  0x20000u      /* 主 OBJ 窗口物理偏移（bank B） */
#define BUS_VRAM_SUB_OBJ_PHYS   0x60000u      /* 副 OBJ 窗口物理偏移（bank D） */

/* VRAMCNT 动态映射（21-B9wd）：NDS 的 9 个物理 VRAM bank 经 0x04000240-249
   可映射到 Engine A BG / A OBJ / Engine B BG / B OBJ / 3D 纹理等逻辑窗口。
   FFXII 把 bank D→顶屏 BG、bank C→底屏 BG、bank E→A OBJ、bank H→B OBJ，
   旧实现的固定“bank A→顶屏”只对默认 homebrew 布局成立。 */
#define BUS_VRAM_CNT_BASE 0x04000240u

/* LCDC 分配 VRAM 窗口（阶段 20.2 显示捕获目标）：0x06800000 起 512KB，映射到 vram[0] */
#define BUS_LCDC_VRAM_BASE 0x06800000u
#define BUS_LCDC_VRAM_SIZE (512 * 1024)

/* 地址区间基址（内存地图见 docs/03-memory-map.md） */
#define BUS_MAIN_RAM_BASE 0x02000000u
#define BUS_MAIN_RAM_MIRROR_BASE 0x02400000u /* Main RAM 无缓存镜像基址（别名） */
#define BUS_SHARED_WRAM_BASE 0x03000000u       /* Shared WRAM 主区基址 */
#define BUS_SHARED_WRAM_MIRROR 0x037F8000u     /* Shared WRAM 镜像区基址（别名） */
#define BUS_VRAM_BASE     0x06000000u
#define BUS_IO_BASE       0x04000000u
#define BUS_IO_SIZE       0x00010000u   /* IO 区间 64KB，具体寄存器由 io 模块实现 */
#define BUS_ARM7_WRAM_BASE 0x03800000u  /* ARM7 WRAM 基址 */
#define BUS_PALETTE_BASE  0x05000000u   /* 调色板 RAM 基址 */
#define BUS_OAM_BASE      0x07000000u   /* OAM（OBJ 属性内存）基址 */

/* IPC FIFO RECV 的独占地址（不在 0x0400xxxx IO 区间内，单独映射） */
#define BUS_IPC_FIFO_RECV 0x04100000u

/* 卡带数据端口 CARD_DATA（不在 0x0400xxxx IO 区间内，单独映射；阶段 15） */
#define BUS_CARD_DATA 0x04100010u

/* 前向声明：bus 只存指针，IO 寄存器语义在 src/io/ 模块实现（阶段 6） */
typedef struct io io_t;

/* 总线：持有各内存数组，按地址区间换算读写。
   阶段 2：只建数组与区间范围；阶段 6 起 IO 区间转发给 io 模块；
   阶段 8 起加 ARM7 WRAM 与「当前访问者身份」（FIFO/中断按 CPU 分流）。 */
typedef struct bus {
    uint8_t  main_ram[BUS_MAIN_RAM_SIZE]; /* Main RAM：4MB */
    uint8_t  arm9_dtcm[BUS_ARM9_DTCM_SIZE]; /* ARM9 DTCM：16KB（CP15 可配置基址） */
    uint8_t  arm9_itcm[BUS_ARM9_ITCM_SIZE]; /* ARM9 ITCM：32KB（0x01FF8000 起） */
uint8_t  vram[BUS_VRAM_SIZE];         /* VRAM：656KB */
uint8_t  vram_dummy[16];              /* 未映射 VRAM 窗口的落点（读 0 / 写丢弃） */
    uint8_t  arm7_wram[BUS_ARM7_WRAM_SIZE]; /* ARM7 WRAM：64KB */
    uint8_t  shared_wram[BUS_SHARED_WRAM_SIZE]; /* Shared WRAM：32KB（WRAMCNT 切分，见 bus.c） */
    uint8_t  palette[BUS_PALETTE_SIZE];   /* 调色板 RAM：2KB */
    uint8_t  oam[BUS_OAM_SIZE];           /* OAM：2KB（OBJ 属性） */
    io_t    *io;                          /* IO 寄存器区实现（由 nds 挂入） */
    uint8_t  vramcnt[9];                  /* VRAMCNT A-I（0x04000240-249） */
    uint32_t vram_map_abg[0x20];          /* Engine A BG：16KB 槽 → bank 位掩码 */
    uint32_t vram_map_aobj[0x10];         /* Engine A OBJ：16KB 槽 → bank 位掩码 */
    uint32_t vram_map_bbg[0x8];           /* Engine B BG：16KB 槽 → bank 位掩码 */
    uint32_t vram_map_bobj[0x8];          /* Engine B OBJ：16KB 槽 → bank 位掩码 */
uint32_t vram_map_tex[4];             /* 3D 纹理 512KB 块 → bank 位掩码 */
uint32_t vram_map_texpal[8];          /* 3D 纹理调色板 16KB 槽 → bank 位掩码
                                         （21-B9yi 续34：E/F/G 的 mode 3） */
    uint32_t vram_map_abg_ext[4];         /* Engine A BG 扩展调色板 8KB 槽 */
    uint32_t vram_map_bbg_ext[4];         /* Engine B BG 扩展调色板 8KB 槽 */
    uint32_t vram_map_aobj_ext;           /* Engine A OBJ 扩展调色板 */
    uint32_t vram_map_bobj_ext;           /* Engine B OBJ 扩展调色板 */
    int      arm9_dtcm_on;                /* ARM9 DTCM 是否使能（CP15 c1 bit16） */
    uint32_t arm9_dtcm_base;              /* ARM9 DTCM 基址（未使能为 0xFFFFFFFF） */
    uint32_t arm9_dtcm_size;              /* ARM9 DTCM 大小 */
    int active_is_arm7;                   /* 当前访问者身份：0=ARM9, 1=ARM7 */
    int diag;                             /* 诊断开关：只打印异常事件（未知 SWI/未实现指令/未知 IO），供 bring-up 定位卡点 */
    /* 21-B9xj：写监视（把写入者的 PC 打出来，用于“谁改了这个寄存器”这类定位）。
       addr ∈ [watch_lo[i], watch_hi[i]) 时打印；dbg_pc 由 cpu_step 维护。 */
    uint32_t dbg_pc;
    uint32_t dbg_lr;   /* 21-B9xj：写监视打印的 LR（helper 的调用点） */
    uint32_t dbg_sp;   /* 21-B9xj：写监视打印的 SP 与栈上 4 个字（找调用链） */
    uint32_t dbg_cpsr; /* 21-B9xp：写/读监视打印 CPSR（判断 ARM/Thumb） */
    uint32_t watch_lo[4];
    uint32_t watch_hi[4];
    /* 21-B9xk：读监视（谁在读这个寄存器）。比如看 ARM9 何时从 IPC FIFO 取报文。 */
    uint32_t watch_r_lo[4];
    uint32_t watch_r_hi[4];
} bus_t;

bus_t *bus_create(void);
void bus_destroy(bus_t *bus);

/* 诊断开关：on=1 时，未实现指令 / 未知 SWI / 未知 IO 访问打印一次日志（阶段 21 bring-up）。 */
void bus_set_diag(bus_t *bus, int on);
/* 21-B9xj：设置写监视区间（最多 4 组，lo==hi 表示关闭）。 */
void bus_set_watch(bus_t *bus, int idx, uint32_t lo, uint32_t hi);
/* 21-B9xk：设置读监视区间（最多 4 组，lo==hi 表示关闭）。 */
void bus_set_watch_read(bus_t *bus, int idx, uint32_t lo, uint32_t hi);

/* CP15 更新 ARM9 DTCM 映射（阶段 21-B8）：enabled=0 时 0x027E0000 等地址走 Main RAM
   镜像；enabled=1 时 ARM9 对 [base, base+size) 的读写改走私有 DTCM。 */
void bus_set_arm9_dtcm(bus_t *bus, int enabled, uint32_t base, uint32_t size);

/* 写 VRAMCNT：按 melonDS GPU::MapVRAM_* 的分支更新逻辑窗口映射。
   bank：0=A … 8=I；cnt：VRAMCNT 寄存器写入的 8 位值。 */
void bus_set_vramcnt(bus_t *bus, int bank, uint8_t cnt);

/* 恢复阶段 9 的默认 homebrew 映射（A→A BG、B→A OBJ、C→B BG、D→B OBJ）。 */
void bus_vram_reset_default(bus_t *bus);

/* 读 BG 扩展调色板颜色（is_sub=1 → Engine B）。slot=0-3、pal=0-15、color=0-255。 */
uint16_t bus_vram_extpal16(const bus_t *bus, int is_sub, int slot,
                           unsigned pal, unsigned color);

/* DISPCNT VRAM 显示模式直读物理 bank 的 16 位字（off 为字节偏移）。 */
uint16_t bus_vram_phys16(const bus_t *bus, int bank, uint32_t off);

/* 21-B9yi(续34)：3D 纹理/纹理调色板的**扁平空间**读取（对齐 melonDS
   `ReadVRAMFlat_Texture<T>` / `ReadVRAMFlat_TexPal<T>` 的槽位映射：
   纹理按 128KB 槽、调色板按 16KB 槽，各槽里的 bank 按位掩码 OR 在一起）。 */
uint8_t bus_vram_tex8(const bus_t *bus, uint32_t addr);
uint16_t bus_vram_texpal16(const bus_t *bus, uint32_t addr);

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
