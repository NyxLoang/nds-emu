/* 阶段 5：统一测试入口。
   把所有此前散落在 main.c 的临时自测（bus 读写、ARM 指令集、CPU 写 VRAM）
   迁到这里，并用同样的「装程序 → 跑 CPU → 断言」方式新增验收用例：
   清屏、画矩形、死循环保活。
   本文件只链 ndscore 核心库（不依赖 SDL/窗口），可直接运行或经 ctest 收集。 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "nds/nds.h"
#include "bus/bus.h"
#include "cpu/cpu.h"
#include "cpu/exec.h"
#include "cpu/thumb.h"
#include "io/io.h"
#include "io/disp.h"
#include "io/touch.h"
#include "snd/snd.h"
#include "gx/gx.h"
#include "ppu/render.h"
#include "timing/timing.h"
#include "cart/cart.h"
#include "cart/key1.h"
#include "cart/cartbus.h"
#include "cart/save.h"

/* 与 ppu.h 的 framebuffer 约定保持一致（此处不 include SDL 头，故重复定义）：
   顶屏 = VRAM 起始 256×192；底屏 = VRAM + 0x18000（见 docs/05-framebuffer.md）。 */
#define TEST_SCREEN_W 256
#define TEST_SCREEN_H 192
#define TEST_VRAM_TOP_OFFSET    0x00000u
#define TEST_VRAM_BOTTOM_OFFSET 0x18000u

static int g_checks = 0;
static int g_failures = 0;

/* 断言工具：got 与 want 不一致则计一次失败并打印。 */
#define CHECK_EQ(name, got, want)                                            \
    do {                                                                     \
        uint32_t _g = (uint32_t)(got);                                       \
        uint32_t _w = (uint32_t)(want);                                      \
        g_checks++;                                                          \
        if (_g != _w) {                                                      \
            printf("  FAIL %-24s got=0x%08X want=0x%08X\n", name, _g, _w);   \
            g_failures++;                                                    \
        } else {                                                             \
            printf("  ok   %-24s (0x%08X)\n", name, _g);                     \
        }                                                                    \
    } while (0)

/* 通用驱动：把程序写进 Main RAM（base 起），从 start_pc 跑到 halt_pc 或 max_steps。
   批量循环期间关闭逐条日志避免刷屏，跑完恢复。返回实际执行步数。 */
static int run_program(nds_t *nds, uint32_t base, const uint32_t *prog,
                       size_t count, uint32_t start_pc, uint32_t halt_pc,
                       int max_steps)
{
    for (size_t i = 0; i < count; i++)
        bus_write32(nds->bus, base + 4 * i, prog[i]);
    exec_set_trace(0);
    cpu_reset(nds->cpu, start_pc);
    int steps = 0;
    while (steps++ < max_steps && nds->cpu->r[15] != halt_pc)
        cpu_step(nds->cpu);
    exec_set_trace(1);
    return steps;
}

/* 同上，但驱动第二颗 ARM7 核（阶段 8）。程序写进指定 base（通常是 ARM7 WRAM）。 */
static int run_cpu7(nds_t *nds, uint32_t base, const uint32_t *prog,
                    size_t count, uint32_t start_pc, uint32_t halt_pc,
                    int max_steps)
{
    for (size_t i = 0; i < count; i++)
        bus_write32(nds->bus, base + 4 * i, prog[i]);
    exec_set_trace(0);
    cpu_reset(nds->cpu7, start_pc);
    int steps = 0;
    while (steps++ < max_steps && nds->cpu7->r[15] != halt_pc)
        cpu_step(nds->cpu7);
    exec_set_trace(1);
    return steps;
}

/* ---- 阶段 13 Thumb 辅助：把 16 位指令写进内存，置 T 位后跑固定步数 ---- */
static void thumb_write(nds_t *nds, uint32_t base, const uint16_t *prog, size_t count)
{
    for (size_t i = 0; i < count; i++)
        bus_write16(nds->bus, base + 2 * i, prog[i]);
}

static void thumb_start(nds_t *nds, uint32_t pc)
{
    exec_set_trace(0);
    thumb_set_trace(0);
    nds->cpu->cpsr |= CPSR_T;          /* 进入 Thumb 状态 */
    cpu_reset(nds->cpu, pc);
}

static void thumb_stop(nds_t *nds)
{
    nds->cpu->cpsr &= ~CPSR_T;         /* 复位回 ARM 状态，避免污染后续用例 */
    thumb_set_trace(1);
    exec_set_trace(1);
}

/* 顶屏像素(x,y) 的颜色字（RGB555）。地址 = VRAM 基址 + (y*宽 + x)*2 字节。 */
static uint16_t top_px(nds_t *nds, int x, int y)
{
    return bus_read16(nds->bus, BUS_VRAM_BASE + TEST_VRAM_TOP_OFFSET
                      + (uint32_t)(y * TEST_SCREEN_W + x) * 2u);
}

/* ---- 21-B9wt ARM7 助手：SWI 走真机低地址路径后，测试要跨多步跑到返回点 ----
   a7_setup_stacks：给 ARM7 一套合法的 System/IRQ/SVC 栈（真机由直接启动或
   SoftReset 初始化；不给栈的话 BIOS 压帧会落到栈外）。 */
static void a7_setup_stacks(arm_cpu_t *cpu)
{
    cpu->r13_sys = BUS_ARM7_WRAM_BASE + 0x7000u;
    cpu->r[13] = cpu->r13_sys;
    cpu->r13_bank[1] = BUS_ARM7_WRAM_BASE + 0x7400u; /* IRQ */
    cpu->r13_bank[2] = BUS_ARM7_WRAM_BASE + 0x7800u; /* SVC */
}

/* 一直单步到 PC == expect（或步数用尽）；返回是否到达。 */
static int a7_run_to_pc(arm_cpu_t *cpu, uint32_t expect)
{
    for (int i = 0; i < 64 && cpu->r[15] != expect; i++)
        cpu_step(cpu);
    return cpu->r[15] == expect;
}

/* 以 ARM7 视角读写（0x0380xxxx 的 WRAM 顶 / BIOS 槽只有 ARM7 视角才映射）。 */
static uint32_t a7_read32(nds_t *nds, uint32_t addr)
{
    nds->bus->active_is_arm7 = 1;
    uint32_t v = bus_read32(nds->bus, addr);
    nds->bus->active_is_arm7 = 0;
    return v;
}

static void a7_write32(nds_t *nds, uint32_t addr, uint32_t val)
{
    nds->bus->active_is_arm7 = 1;
    bus_write32(nds->bus, addr, val);
    nds->bus->active_is_arm7 = 0;
}

/* ---- 阶段 2 迁移：bus 读写换算 + 小端拼拆 + VRAM/IO 桩 ---- */
static void test_bus_rw(nds_t *nds)
{
    bus_t *bus = nds->bus;

    /* 8 位：写再读回同一字节，未映射区间读返回 0 */
    bus_write8(bus, BUS_MAIN_RAM_BASE + 0x100, 0xAB);
    CHECK_EQ("write8 -> read8", bus_read8(bus, BUS_MAIN_RAM_BASE + 0x100), 0xAB);
    CHECK_EQ("unmapped read8", bus_read8(bus, 0x0F000000u), 0x00);

    /* 16 位小端：写 0xABCD 应读回 0xABCD，内存字节序为 CD AB */
    bus_write16(bus, BUS_MAIN_RAM_BASE + 0x200, 0xABCD);
    CHECK_EQ("write16 -> read16", bus_read16(bus, BUS_MAIN_RAM_BASE + 0x200), 0xABCD);
    CHECK_EQ("endian lo byte", bus_read8(bus, BUS_MAIN_RAM_BASE + 0x200), 0xCD);
    CHECK_EQ("endian hi byte", bus_read8(bus, BUS_MAIN_RAM_BASE + 0x201), 0xAB);

    /* 32 位小端：写 0x12345678 应读回原值，字节序为 78 56 34 12 */
    bus_write32(bus, BUS_MAIN_RAM_BASE + 0x300, 0x12345678);
    CHECK_EQ("write32 -> read32", bus_read32(bus, BUS_MAIN_RAM_BASE + 0x300), 0x12345678u);
    CHECK_EQ("endian byte0", bus_read8(bus, BUS_MAIN_RAM_BASE + 0x300), 0x78);
    CHECK_EQ("endian byte1", bus_read8(bus, BUS_MAIN_RAM_BASE + 0x301), 0x56);
    CHECK_EQ("endian byte2", bus_read8(bus, BUS_MAIN_RAM_BASE + 0x302), 0x34);
    CHECK_EQ("endian byte3", bus_read8(bus, BUS_MAIN_RAM_BASE + 0x303), 0x12);

    /* VRAM 区间：写 0xBEEF 读回 0xBEEF */
    bus_write16(bus, BUS_VRAM_BASE + 0x10, 0xBEEF);
    CHECK_EQ("vram write16 -> read16", bus_read16(bus, BUS_VRAM_BASE + 0x10), 0xBEEF);

    /* IO 桩（0x4000004 现为 DISPSTAT：低字节只接受 bit3-5 IRQ 使能） */
    bus_write8(bus, BUS_IO_BASE + 0x04, 0x11);
    CHECK_EQ("io stub read8", bus_read8(bus, BUS_IO_BASE + 0x04), 0x10);
}

/* ---- 阶段 3b 迁移：ARM 指令集（MOV/ADD/SUB/CMP/LDR/STR/B/BL/BX/AND/ORR/EOR） ---- */
static void test_arm_instructions(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE;

    /* 手工汇编测试程序（指令字来自 ARM 编码手册，见 cpulog.md 附表）。
       布局：程序 0x00-0x54，数据 0x68，BL 子程序 0x70。 */
    static const uint32_t prog[] = {
        /* 0x00 */ 0xE3A00005, /* MOV r0, #5            → r0=5        */
        /* 0x04 */ 0xE3A01007, /* MOV r1, #7            → r1=7        */
        /* 0x08 */ 0xE0802001, /* ADD r2, r0, r1        → r2=12       */
        /* 0x0C */ 0xE0423000, /* SUB r3, r2, r0        → r3=7        */
        /* 0x10 */ 0xE2804008, /* ADD r4, r0, #8        → r4=13       */
        /* 0x14 */ 0xE3500005, /* CMP r0, #5            → Z=1         */
        /* 0x18 */ 0xE1500001, /* CMP r0, r1            → N=1 C=0     */
        /* 0x1C */ 0xE3A05402, /* MOV r5, #0x02000000（0x02 ROR 8）→ Main RAM 基址 */
        /* 0x20 */ 0xE3A0607F, /* MOV r6, #0x7F         → r6=0x7F     */
        /* 0x24 */ 0xE5856060, /* STR r6, [r5, #0x60]   → mem[0x60]=0x7F */
        /* 0x28 */ 0xE3A07000, /* MOV r7, #0            → r7=0        */
        /* 0x2C */ 0xE5957060, /* LDR r7, [r5, #0x60]   → r7=0x7F     */
        /* 0x30 */ 0xE5958068, /* LDR r8, [r5, #0x68]   → r8=0x12345678 */
        /* 0x34 */ 0xEA000003, /* B 0x48（跳过 0x38-0x44）              */
        /* 0x38 */ 0xE3A080FF, /* MOV r8, #0xFF（应被跳过）             */
        /* 0x3C */ 0xE1A00000, /* NOP (MOV r0, r0)                     */
        /* 0x40 */ 0xE1A00000, /* NOP                                 */
        /* 0x44 */ 0xE1A00000, /* NOP                                 */
        /* 0x48 */ 0xE3A0B011, /* MOV r11, #0x11（B 的落点）→ r11=0x11 */
        /* 0x4C */ 0xEB000007, /* BL 0x70（lr=0x50，跳子程序）         */
        /* 0x50 */ 0xE3A09022, /* MOV r9, #0x22（返回后执行）→ r9=0x22 */
        /* 0x54 */ 0xEAFFFFFE, /* B self（停机）                       */
    };
    for (size_t i = 0; i < sizeof prog / sizeof prog[0]; i++)
        bus_write32(nds->bus, base + 4 * i, prog[i]);
    /* 数据区：LDR r8, [r5, #8] 读的就是这个预置值 */
    bus_write32(nds->bus, base + 0x68, 0x12345678);
    /* BL 子程序：0x70 MOV r10,#0x33；0x74 BX lr 返回调用处 */
    bus_write32(nds->bus, base + 0x70, 0xE3A0A033);
    bus_write32(nds->bus, base + 0x74, 0xE12FFF1E);

    run_program(nds, base, prog, sizeof prog / sizeof prog[0],
                base, base + 0x54, 64);

    CHECK_EQ("r0 (MOV imm)",  nds->cpu->r[0],  0x00000005u);
    CHECK_EQ("r1 (MOV imm)",  nds->cpu->r[1],  0x00000007u);
    CHECK_EQ("r2 (ADD reg)",  nds->cpu->r[2],  0x0000000Cu);
    CHECK_EQ("r3 (SUB reg)",  nds->cpu->r[3],  0x00000007u);
    CHECK_EQ("r4 (ADD imm)",  nds->cpu->r[4],  0x0000000Du);
    CHECK_EQ("r5 (MOV addr)", nds->cpu->r[5],  0x02000000u);
    CHECK_EQ("r7 (LDR mem)",  nds->cpu->r[7],  0x0000007Fu);
    CHECK_EQ("r8 (LDR off)",  nds->cpu->r[8],  0x12345678u);
    CHECK_EQ("r9 (BL ret)",   nds->cpu->r[9],  0x00000022u);
    CHECK_EQ("r10 (BL sub)",  nds->cpu->r[10], 0x00000033u);
    CHECK_EQ("r11 (B tgt)",   nds->cpu->r[11], 0x00000011u);
    CHECK_EQ("lr (BL saved)", nds->cpu->r[14], base + 0x50u);

    /* 3b.9 位运算程序（0x80 起）：
       AND r7, r5, r6 / ORR r8, r5, r6 / EOR r9, r5, r6 */
    static const uint32_t bitop[] = {
        /* 0x80 */ 0xE3A0500F, /* MOV r5, #0x0F        → r5=0x0F */
        /* 0x84 */ 0xE3A06033, /* MOV r6, #0x33        → r6=0x33 */
        /* 0x88 */ 0xE0057006, /* AND r7, r5, r6       → r7=0x0F & 0x33 = 0x03 */
        /* 0x8C */ 0xE1858006, /* ORR r8, r5, r6       → r8=0x0F | 0x33 = 0x3F */
        /* 0x90 */ 0xE0259006, /* EOR r9, r5, r6       → r9=0x0F ^ 0x33 = 0x3C */
        /* 0x94 */ 0xEAFFFFFE, /* B self（停机） */
    };
    run_program(nds, base + 0x80, bitop, sizeof bitop / sizeof bitop[0],
                base + 0x80, base + 0x94, 16);

    CHECK_EQ("r7 (AND)",  nds->cpu->r[7],  0x00000003u);
    CHECK_EQ("r8 (ORR)",  nds->cpu->r[8],  0x0000003Fu);
    CHECK_EQ("r9 (EOR)",  nds->cpu->r[9],  0x0000003Cu);

    /* 3b.10 综合：用纯机器码把一个 RGB555 颜色字写进 VRAM，再读回。 */
    static const uint32_t vramprog[] = {
        /* 0x98 */ 0xE3A00406, /* MOV r0, #0x06000000（0x06 ROR 8）→ VRAM 基址 */
        /* 0x9C */ 0xE3A01C7C, /* MOV r1, #0x7C00（0x7C ROR 24）→ 红色（RGB555） */
        /* 0xA0 */ 0xE5801000, /* STR r1, [r0]           → VRAM[0] = 0x7C00 */
        /* 0xA4 */ 0xE5902000, /* LDR r2, [r0]           → r2 = 读回 */
        /* 0xA8 */ 0xEAFFFFFE, /* B self（停机） */
    };
    run_program(nds, base + 0x98, vramprog, sizeof vramprog / sizeof vramprog[0],
                base + 0x98, base + 0xA8, 16);

    CHECK_EQ("r2 (VRAM readback)", nds->cpu->r[2], 0x00007C00u);
    CHECK_EQ("VRAM[0] color word", bus_read32(nds->bus, BUS_VRAM_BASE), 0x00007C00u);

    /* CMP 更新了 flags：CMP r0,#5 → Z=1；CMP r0,r1 → N=1 C=0（V 未知，只看 N/Z/C） */
    uint32_t cpsr = nds->cpu->cpsr;
    if ((cpsr & (1u << 31)) && !(cpsr & (1u << 30)) && !(cpsr & (1u << 29))) {
        printf("  ok   %-24s (N=1 Z=0 C=0)\n", "flags N/Z/C");
        g_checks++;
    } else {
        printf("  FAIL %-24s got cpsr=%08X\n", "flags N/Z/C", cpsr);
        g_checks++;
        g_failures++;
    }
}

/* ---- 阶段 4.5 迁移：CPU 写 VRAM 出图（顶屏黄十字 + 底屏整屏绿） ---- */
static void test_cpu_draw_vram(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE;

    /* 手工汇编（编码注释见 docs/04-cpu-loop.md 与 cpulog.md）：
       程序布局：0x00-0x5C，B self 停机在 0x5C。
       画顶屏黄色十字（水平线 y=96 + 竖线 x=128），底屏整屏绿色。 */
    /* 手工汇编（编码注释见 docs/04-cpu-loop.md 与 cpulog.md）：
       程序布局：0x00-0x74，B self 停机在 0x74。
       十字两臂都 2px：横线画 y=95、y=96 两行；竖线画 x=128、129 两列，
       从 y=0 一直画到 y=191（终点 = 起点 + 0x18000，越过末行保证下半段完整）。 */
    static const uint32_t drawprog[] = {
        /* 0x00 */ 0xE3A00406, /* MOV r0, #0x06000000       r0 = 顶屏基址 */
        /* 0x04 */ 0xE2800CBE, /* ADD r0, r0, #0xBE00       r0 = 0x0600BE00（y=95 行起点） */
        /* 0x08 */ 0xE3A014FF, /* MOV r1, #0xFF000000       黄色高半字（0xFF ROR 8） */
        /* 0x0C */ 0xE3811CFF, /* ORR r1, r1, #0xFF00       → r1=0xFF00FF00（2 像素同色） */
        /* 0x10 */ 0xE3A02002, /* MOV r2, #2                r2 = 2 行（横线 2px 粗） */
        /* 0x14 */ 0xE3A03080, /* MOV r3, #0x80             r3 = 每行 128 字（256 像素） */
        /* 0x18 */ 0xE5801000, /* STR r1, [r0]              写 2 像素黄 */
        /* 0x1C */ 0xE2800004, /* ADD r0, r0, #4            行内前进 */
        /* 0x20 */ 0xE2533001, /* SUBS r3, r3, #1           行内字数-1 */
        /* 0x24 */ 0x1AFFFFFB, /* BNE 0x18                  行内循环（r0 自动到下一行行首） */
        /* 0x28 */ 0xE3A03080, /* MOV r3, #0x80             重置行内计数 */
        /* 0x2C */ 0xE2522001, /* SUBS r2, r2, #1           行数-1 */
        /* 0x30 */ 0x1AFFFFF8, /* BNE 0x18                  行循环（画第 2 行 y=96） */
        /* 0x34 */ 0xE3A00406, /* MOV r0, #0x06000000       r0 = 顶屏基址 */
        /* 0x38 */ 0xE2800C01, /* ADD r0, r0, #0x100        r0 = 0x06000100（x=128, y=0） */
        /* 0x3C */ 0xE2804A18, /* ADD r4, r0, #0x18000      r4 = 0x06018100（越过末行 y=191） */
        /* 0x40 */ 0xE5801000, /* STR r1, [r0]              写 2 像素黄（x=128,129） */
        /* 0x44 */ 0xE2800C02, /* ADD r0, r0, #0x200        下一行同一列 */
        /* 0x48 */ 0xE1500004, /* CMP r0, r4                列画完了吗 */
        /* 0x4C */ 0x1AFFFFFB, /* BNE 0x40                  未到终点继续 */
        /* 0x50 */ 0xE3A00406, /* MOV r0, #0x06000000       r0 = 顶屏基址 */
        /* 0x54 */ 0xE2801A18, /* ADD r1, r0, #0x18000      r1 = 底屏基址 0x06018000 */
        /* 0x58 */ 0xE2815A18, /* ADD r5, r1, #0x18000      r5 = 底屏终点 0x06030000 */
        /* 0x5C */ 0xE3A0263E, /* MOV r2, #0x03E00000       绿色高半字（0x3E ROR 12） */
        /* 0x60 */ 0xE3822E3E, /* ORR r2, r2, #0x03E0       → r2=0x03E003E0（2 像素同色） */
        /* 0x64 */ 0xE5812000, /* STR r2, [r1]              写 2 像素绿 */
        /* 0x68 */ 0xE2811004, /* ADD r1, r1, #4            前进 2 像素 */
        /* 0x6C */ 0xE1510005, /* CMP r1, r5                底屏画完了吗 */
        /* 0x70 */ 0x1AFFFFFB, /* BNE 0x64                  未到终点继续 */
        /* 0x74 */ 0xEAFFFFFE, /* B self（停机） */
    };

    int steps = run_program(nds, base, drawprog,
                            sizeof drawprog / sizeof drawprog[0],
                            base, base + 0x74, 1 << 20);

    /* 回读 CPU 实际写过的像素。注意 top_px(x,y) 返回的是 16 位像素，
       而 32 位 STR 写入的字 = 0xFF00FF00，低半字 0xFF00 才是 (x,128) 的像素。 */
    CHECK_EQ("top cross (128,0)",    top_px(nds, 128, 0),   0xFF00u);
    CHECK_EQ("top cross (128,191)",  top_px(nds, 128, 191), 0xFF00u); /* 竖线下半段 */
    CHECK_EQ("top cross (128,96)",   top_px(nds, 128, 96),  0xFF00u); /* 交点 */
    CHECK_EQ("top cross (128,97)",   top_px(nds, 128, 97),  0xFF00u); /* 交点下方仍黄（2px 宽） */
    CHECK_EQ("top cross (0,95)",     top_px(nds, 0, 95),    0xFF00u); /* 横线第 1 行 */
    CHECK_EQ("top cross (0,96)",     top_px(nds, 0, 96),    0xFF00u); /* 横线第 2 行 */
    CHECK_EQ("top cross (255,95)",   top_px(nds, 255, 95),  0xFF00u); /* 横线右端 */
    CHECK_EQ("top blank (10,10)",    top_px(nds, 10, 10),   0x0000u); /* 十字外 */
    CHECK_EQ("top blank (10,190)",   top_px(nds, 10, 190),  0x0000u); /* 竖线左侧 */
    CHECK_EQ("bottom green (0,0)",   bus_read16(nds->bus, BUS_VRAM_BASE
                                     + TEST_VRAM_BOTTOM_OFFSET), 0x03E0u);
    printf("  cpu_draw steps=%d\n", steps);
}

/* ---- 5.2 用例：清屏（CPU 把顶屏整屏填成约定底色 0x0000） ----
   先把 VRAM 涂成 0xFFFF 乱码，再跑清屏程序，逐像素验证全为底色，
   证明「清屏」确实由模拟 CPU 覆盖完成而非初始为 0。 */
static void test_clear_screen(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE;
    for (uint32_t i = 0; i < TEST_SCREEN_W * TEST_SCREEN_H; i++)
        bus_write16(nds->bus, BUS_VRAM_BASE + TEST_VRAM_TOP_OFFSET + i * 2u, 0xFFFF);

    /* 整屏 256×192 = 49152 像素 = 24576 字（一次 STR 写 2 像素）。
       循环：STR → ADD +4 → SUBS 计数-1 → BNE（Z=0 继续），写完落到 B self。 */
    static const uint32_t prog[] = {
        /* 0x00 */ 0xE3A00406, /* MOV r0, #0x06000000       r0 = 顶屏 framebuffer 起点 */
        /* 0x04 */ 0xE3A01000, /* MOV r1, #0x0000           r1 = 约定底色（黑） */
        /* 0x08 */ 0xE3A02A06, /* MOV r2, #0x6000           r2 = 24576 字（0x06 ROR 20） */
        /* 0x0C */ 0xE5801000, /* STR r1, [r0]              写 2 像素 */
        /* 0x10 */ 0xE2800004, /* ADD r0, r0, #4            下一字 */
        /* 0x14 */ 0xE2522001, /* SUBS r2, r2, #1           字数-1（更新 Z） */
        /* 0x18 */ 0x1AFFFFFB, /* BNE 0x0C                  未清零继续 */
        /* 0x1C */ 0xEAFFFFFE, /* B self                    停机 */
    };
    int steps = run_program(nds, base, prog, sizeof prog / sizeof prog[0],
                            base, base + 0x1C, 1 << 20);

    int bad = 0;
    for (uint32_t i = 0; i < TEST_SCREEN_W * TEST_SCREEN_H; i++) {
        uint32_t v = bus_read16(nds->bus, BUS_VRAM_BASE
                                + TEST_VRAM_TOP_OFFSET + i * 2u);
        if (v != 0x0000u) {
            if (bad < 3)
                printf("  clear: px%u = %04X (not cleared)\n", i, v);
            bad++;
        }
    }
    g_checks++;
    if (bad == 0) {
        printf("  ok   %-24s (all %d px cleared, %d steps)\n",
               "clear screen", TEST_SCREEN_W * TEST_SCREEN_H, steps);
    } else {
        printf("  FAIL %-24s (%d px not cleared)\n", "clear screen", bad);
        g_failures++;
    }
}

/* ---- 5.3 用例：画矩形（CPU 在顶屏左上象限 128×96 填红色） ----
   先整屏清黑，再画实心矩形；逐像素验证矩形内全红、矩形外全黑。 */
static void test_draw_rect(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE;

    /* 矩形 = 顶屏左上象限：x∈[0,128)、y∈[0,96)。
       每行 64 字（128 像素 = 256 字节），行间跨 512 字节（0x200）。
       注意 STR 是 32 位写 = 2 个 RGB555 像素，颜色必须把同一值放进
       两个字半（0x7C007C00），否则 2 像素只会第 1 个着色（竖条纹）。 */
    static const uint32_t prog[] = {
        /* 清屏：与 test_clear_screen 相同 */
        /* 0x00 */ 0xE3A00406, /* MOV r0, #0x06000000 */
        /* 0x04 */ 0xE3A01000, /* MOV r1, #0x0000 */
        /* 0x08 */ 0xE3A02A06, /* MOV r2, #0x6000 */
        /* 0x0C */ 0xE5801000, /* STR r1, [r0] */
        /* 0x10 */ 0xE2800004, /* ADD r0, r0, #4 */
        /* 0x14 */ 0xE2522001, /* SUBS r2, r2, #1 */
        /* 0x18 */ 0x1AFFFFFB, /* BNE 0x0C */
        /* 画矩形 */
        /* 0x1C */ 0xE3A00406, /* MOV r0, #0x06000000       r0 = 矩形左上角（0,0） */
        /* 0x20 */ 0xE3A0147C, /* MOV r1, #0x7C000000       红色高半字（0x7C ROR 8） */
        /* 0x24 */ 0xE3811C7C, /* ORR r1, r1, #0x7C00       红色低半字 → r1=0x7C007C00 */
        /* 0x28 */ 0xE3A02060, /* MOV r2, #0x60             r2 = 96 行 */
        /* 0x2C */ 0xE3A03040, /* MOV r3, #0x40             r3 = 每行 64 字 */
        /* 0x30 */ 0xE5801000, /* STR r1, [r0]              写 2 像素红 */
        /* 0x34 */ 0xE2800004, /* ADD r0, r0, #4            行内前进 */
        /* 0x38 */ 0xE2533001, /* SUBS r3, r3, #1           行内字数-1 */
        /* 0x3C */ 0x1AFFFFFB, /* BNE 0x30                  行内循环 */
        /* 0x40 */ 0xE2800C01, /* ADD r0, r0, #0x100        行尾 + 0x100 = 下一行行首 */
        /* 0x44 */ 0xE2522001, /* SUBS r2, r2, #1           行数-1 */
        /* 0x48 */ 0x1AFFFFF7, /* BNE 0x2C                  行循环 */
        /* 0x4C */ 0xEAFFFFFE, /* B self                    停机 */
    };
    int steps = run_program(nds, base, prog, sizeof prog / sizeof prog[0],
                            base, base + 0x4C, 1 << 20);

    /* 抽查 4 个代表点：矩形内/外各 2 个 */
    CHECK_EQ("rect inside (10,10)", top_px(nds, 10, 10), 0x7C00u);
    CHECK_EQ("rect inside (127,95)", top_px(nds, 127, 95), 0x7C00u);
    CHECK_EQ("rect outside (200,10)", top_px(nds, 200, 10), 0x0000u);
    CHECK_EQ("rect outside (10,150)", top_px(nds, 10, 150), 0x0000u);

    /* 全屏扫描：矩形内全红、矩形外全黑 */
    int bad = 0;
    for (int y = 0; y < TEST_SCREEN_H; y++) {
        for (int x = 0; x < TEST_SCREEN_W; x++) {
            int inside = (x < 128 && y < 96);
            uint32_t want = inside ? 0x7C00u : 0x0000u;
            if (top_px(nds, x, y) != want) {
                if (bad < 3)
                    printf("  rect: px(%d,%d) = %04X (want %04X)\n",
                           x, y, top_px(nds, x, y), want);
                bad++;
            }
        }
    }
    g_checks++;
    if (bad == 0) {
        printf("  ok   %-24s (128x96 red rect on black, %d steps)\n",
               "draw rect", steps);
    } else {
        printf("  FAIL %-24s (%d px wrong)\n", "draw rect", bad);
        g_failures++;
    }
}

/* ---- 5.4 用例：死循环保活（跑 10 万步不崩、PC 稳定） ---- */
static void test_dead_loop(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE;
    bus_write32(nds->bus, base, 0xEAFFFFFE); /* B self */

    exec_set_trace(0);
    cpu_reset(nds->cpu, base);
    for (int i = 0; i < 100000; i++)
        cpu_step(nds->cpu);
    exec_set_trace(1);

    CHECK_EQ("dead-loop PC stable", nds->cpu->r[15], base);
    CHECK_EQ("dead-loop cycles", (uint32_t)nds->cpu->cycles, 100000u);
    printf("  dead-loop survived 100000 steps, PC=0x%08X\n", nds->cpu->r[15]);
}

/* ---- 6.2 用例：IME / IE / IF 读写（含 IF 写 1 清除） ---- */
static void test_irq_regs(nds_t *nds)
{
    /* IME：写 0x00000001 读回（32 位寄存器，只用了 bit0） */
    bus_write32(nds->bus, IO_IME_ADDR, 0x00000001u);
    CHECK_EQ("IME write/read", bus_read32(nds->bus, IO_IME_ADDR), 0x00000001u);

    /* IE：使能 VBlank（bit0，与 NDS 硬件一致） */
    bus_write32(nds->bus, IO_IE_ADDR, IO_IF_VBLANK);
    CHECK_EQ("IE write/read", bus_read32(nds->bus, IO_IE_ADDR), IO_IF_VBLANK);

    /* IF：硬件（io_set_vblank）置位后能读到 */
    io_set_vblank(nds->io);
    CHECK_EQ("IF vblank set", bus_read32(nds->bus, IO_IF_ADDR), IO_IF_VBLANK);

    /* IF 写 1 清除：只清被写 1 的位，写 0 的位不受影响 */
    io_set_vblank(nds->io);              /* 重新挂起 */
    bus_write32(nds->bus, IO_IF_ADDR, 0xFFFFFFFFu); /* 全部清掉 */
    CHECK_EQ("IF clear-all", bus_read32(nds->bus, IO_IF_ADDR), 0x00000000u);

    io_set_vblank(nds->io);
    bus_write32(nds->bus, IO_IF_ADDR, IO_IF_VBLANK); /* 只清 VBlank 位 */
    CHECK_EQ("IF clear vblank only", bus_read32(nds->bus, IO_IF_ADDR), 0x00000000u);
}

/* ---- 6.3 用例：指令计数产生 VBlank（IF bit0 位置位） ---- */
static void test_vblank_flag(nds_t *nds)
{
    /* 每帧跑完固定步数后 io_set_vblank，IF 的 bit0 应为 1 */
    io_set_vblank(nds->io);
    CHECK_EQ("IF bit0 (VBlank) set", bus_read32(nds->bus, IO_IF_ADDR), IO_IF_VBLANK);
    CHECK_EQ("IF7 vblank set", nds->io->irq[1].ifl & IO_IF_VBLANK,
             IO_IF_VBLANK);
}

/* ---- 6.4 用例：最小 IRQ 响应（pending 检测） ----
   只有 IF 有挂起 + IE 使能 + IME 总开关打开，才算真中断。 */
static void test_irq_pending(nds_t *nds)
{
    /* 1) 有挂起但没使能/没总开关 → 不 pending */
    io_set_vblank(nds->io);
    CHECK_EQ("no IE/IME -> not pending", io_irq_pending(nds->io), 0);

    /* 2) 使能了但总开关没开 → 仍不 pending */
    bus_write32(nds->bus, IO_IE_ADDR, IO_IF_VBLANK);
    CHECK_EQ("no IME -> not pending", io_irq_pending(nds->io), 0);

    /* 3) 三者齐 → pending */
    bus_write32(nds->bus, IO_IME_ADDR, 0x00000001u);
    CHECK_EQ("IME+IE+IF -> pending", io_irq_pending(nds->io), 1);

    /* 4) 程序写 IF 清掉挂起 → 不再 pending */
    bus_write32(nds->bus, IO_IF_ADDR, IO_IF_VBLANK);
    CHECK_EQ("after IF clear -> not pending", io_irq_pending(nds->io), 0);
}

/* ---- 6.5 用例：Timers 0-3 启停 / 分频 / 计数 ----
   每个定时器 4 字节：base+0/1 = CNT_L，base+2/3 = CNT_H（bit7 使能，bit0-1 分频）。
   用 B self 死循环跑固定步数，观察计数值随指令数增长。 */
static void test_timers(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE;

    /* 死循环程序（3a.5 已支持）：跑 N 步时 PC 不动、定时器照走 */
    bus_write32(nds->bus, base, 0xEAFFFFFE);
    exec_set_trace(0);
    cpu_reset(nds->cpu, base);

    /* TM0 使能 + 1:1 分频（CNT_H=0x80）：每 1 条指令涨 1 */
    bus_write16(nds->bus, IO_TIMER0_BASE + 2, 0x0080);
    /* TM1 使能 + 1:64 分频（CNT_H=0x81）：每 64 条指令涨 1 */
    bus_write16(nds->bus, IO_TIMER0_BASE + 4 + 2, 0x0081);
    /* TM2 保持禁止（CNT_H=0x00）：不计数 */
    bus_write16(nds->bus, IO_TIMER0_BASE + 8 + 2, 0x0000);

    const int n = 640;
    for (int i = 0; i < n; i++)
        cpu_step(nds->cpu);
    exec_set_trace(1);

    uint32_t t0 = bus_read16(nds->bus, IO_TIMER0_BASE);
    uint32_t t1 = bus_read16(nds->bus, IO_TIMER0_BASE + 4);
    uint32_t t2 = bus_read16(nds->bus, IO_TIMER0_BASE + 8);
    printf("  timers after %d steps: t0=%u t1=%u t2=%u\n", n, t0, t1, t2);
    CHECK_EQ("TM0 1:1 counts steps", t0, (uint32_t)n);
    CHECK_EQ("TM1 1:64 counts div",  t1, (uint32_t)(n / 64));
    CHECK_EQ("TM2 disabled stays 0", t2, 0u);
}

/* ---- 6.6 用例：KEYINPUT 读写（按下=0） ---- */
static void test_keyinput(nds_t *nds)
{
    /* 21-B9wv：复位后的默认值必须是“全部松开”。旧实现 calloc 出 0，
       KEYINPUT 读成 0（= 所有键按住），游戏会以为 START/A 一直按着。 */
    /* 21-B9xy：按 melonDS 口径，只有低 10 位（松开=1），bit10-15 读 0
       （参考核帧 1900 无按键时 KEYINPUT 读回 0x03FF）。 */
    CHECK_EQ("keypad default released",
             bus_read16(nds->bus, IO_KEYINPUT_ADDR), 0x03FFu);

    /* 未按键：低 10 位全 1 */
    io_set_keyinput(nds->io, 0x0000);
    CHECK_EQ("no key pressed", bus_read16(nds->bus, IO_KEYINPUT_ADDR), 0x03FFu);

    /* 按下 A（bit0）：读值该位变 0 */
    io_set_keyinput(nds->io, KEY_A);
    CHECK_EQ("A pressed -> bit0=0", bus_read16(nds->bus, IO_KEYINPUT_ADDR), 0x03FEu);

    /* 按下 UP + B */
    io_set_keyinput(nds->io, KEY_UP | KEY_B);
    CHECK_EQ("UP+B pressed", bus_read16(nds->bus, IO_KEYINPUT_ADDR), 0x03BDu);

    /* 21-B9wr：KEYCNT OR 模式按键中断（ARM9） */
    nds->io->irq[0].ifl = 0;
    bus_write16(nds->bus, IO_KEYCNT_ADDR, KEYCNT_IRQ_ENABLE | KEY_A);
    io_set_keyinput(nds->io, 0);
    CHECK_EQ("keycnt no irq released", nds->io->irq[0].ifl & IO_IF_KEY, 0u);
    io_set_keyinput(nds->io, KEY_A);
    CHECK_EQ("keycnt or irq", nds->io->irq[0].ifl & IO_IF_KEY, IO_IF_KEY);

    /* AND 模式：只有掩码内全部键都按下才触发 */
    nds->io->irq[0].ifl = 0;
    bus_write16(nds->bus, IO_KEYCNT_ADDR,
                KEYCNT_IRQ_ENABLE | KEYCNT_IRQ_AND | KEY_A | KEY_B);
    io_set_keyinput(nds->io, 0);
    nds->io->irq[0].ifl = 0;
    io_set_keyinput(nds->io, KEY_A);
    CHECK_EQ("keycnt and partial", nds->io->irq[0].ifl & IO_IF_KEY, 0u);
    io_set_keyinput(nds->io, KEY_A | KEY_B);
    CHECK_EQ("keycnt and full", nds->io->irq[0].ifl & IO_IF_KEY, IO_IF_KEY);
}

/* ---- 6.7 用例：等 VBlank（CPU 轮询 IF，置位后写 VRAM） ----
   程序：读 IF → 检查 bit0 → 没置位就继续转圈 → 置位后把黄色写进 VRAM。
   测试先跑一段（不给 VBlank），应停在轮询循环、不写屏；
   再 io_set_vblank 后继续，应走出循环并写屏。 */
static void test_wait_vblank(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE;

    static const uint32_t prog[] = {
        /* 0x00 */ 0xE3A00404, /* MOV r0, #0x04000000       r0 = IO 基址 */
        /* 0x04 */ 0xE2800C02, /* ADD r0, r0, #0x200        r0 = 0x04000200（0x02 ROR 24） */
        /* 0x08 */ 0xE2800014, /* ADD r0, r0, #0x14         r0 = 0x04000214（IF） */
        /* 0x0C */ 0xE5901000, /* LDR r1, [r0]              r1 = IF */
        /* 0x10 */ 0xE2112001, /* ANDS r2, r1, #1           VBlank 位（bit0）→ Z 标志 */
        /* 0x14 */ 0x1A000000, /* BNE 0x1C                  置位则跳出等待 */
        /* 0x18 */ 0xEAFFFFFB, /* B 0x0C                    没置位继续轮询 */
        /* 0x1C */ 0xE3A03406, /* MOV r3, #0x06000000       r3 = VRAM 基址 */
        /* 0x20 */ 0xE3A04CFF, /* MOV r4, #0xFF00           r4 = 黄色 */
        /* 0x24 */ 0xE5834000, /* STR r4, [r3]              VRAM[0] = 黄 */
        /* 0x28 */ 0xEAFFFFFE, /* B self（停机） */
    };

    /* 第一次运行：不触发 VBlank，程序应停在 0x0C-0x18 轮询循环里，
       PC 不会到停机点 0x28，也不会写屏。 */
    run_program(nds, base, prog, sizeof prog / sizeof prog[0],
                base, base + 0x28, 64);
    CHECK_EQ("wait: PC in poll loop", nds->cpu->r[15] != base + 0x28, 1);
    CHECK_EQ("wait: no pixel yet", bus_read16(nds->bus, BUS_VRAM_BASE), 0x0000u);

    /* 第二次运行（全新 reset）：先触发 VBlank 再从头跑，应走出循环、写屏、停机 */
    io_set_vblank(nds->io);
    run_program(nds, base, prog, sizeof prog / sizeof prog[0],
                base, base + 0x28, 64);
    CHECK_EQ("wait: PC reaches halt", nds->cpu->r[15], base + 0x28);
    CHECK_EQ("wait: pixel written", bus_read16(nds->bus, BUS_VRAM_BASE), 0xFF00u);
}

/* ---- 6.7 用例：读键改变显示（CPU 读 KEYINPUT，按 A 显示蓝、否则红） ----
   程序：读 KEYINPUT → ANDS 检查 bit0（A 键）→ 按下则写蓝色、否则写红色到 VRAM。 */
static void test_key_program(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE;

    static const uint32_t prog[] = {
        /* 0x00 */ 0xE3A00404, /* MOV r0, #0x04000000       r0 = IO 基址 */
        /* 0x04 */ 0xE2800C01, /* ADD r0, r0, #0x100        r0 = 0x04000100 */
        /* 0x08 */ 0xE2800030, /* ADD r0, r0, #0x30         r0 = 0x04000130（KEYINPUT） */
        /* 0x0C */ 0xE5901000, /* LDR r1, [r0]              r1 = 按键读值（按下=0） */
        /* 0x10 */ 0xE3A02001, /* MOV r2, #1                r2 = A 键位（bit0） */
        /* 0x14 */ 0xE0113002, /* ANDS r3, r1, r2           A 按下→0、未按→1 → Z 标志 */
        /* 0x18 */ 0x1A000001, /* BNE 0x24                  未按 A → 红色 */
        /* 0x1C */ 0xE3A0401F, /* MOV r4, #0x001F           r4 = 蓝色 */
        /* 0x20 */ 0xEA000000, /* B 0x28（pc+8=0x28）        跳过红色分支 */
        /* 0x24 */ 0xE3A04C7C, /* MOV r4, #0x7C00           r4 = 红色 */
        /* 0x28 */ 0xE3A05406, /* MOV r5, #0x06000000       r5 = VRAM 基址 */
        /* 0x2C */ 0xE5854000, /* STR r4, [r5]              VRAM[0] = 颜色 */
        /* 0x30 */ 0xEAFFFFFE, /* B self（停机） */
    };

    /* 按 A：读值 bit0=0，程序写蓝色 */
    io_set_keyinput(nds->io, KEY_A);
    run_program(nds, base, prog, sizeof prog / sizeof prog[0],
                base, base + 0x30, 64);
    CHECK_EQ("key: A pressed -> blue", bus_read16(nds->bus, BUS_VRAM_BASE), 0x001Fu);

    /* 未按：读值 bit0=1，程序写红色 */
    io_set_keyinput(nds->io, 0x0000);
    run_program(nds, base, prog, sizeof prog / sizeof prog[0],
                base, base + 0x30, 64);
    CHECK_EQ("key: no A -> red", bus_read16(nds->bus, BUS_VRAM_BASE), 0x7C00u);
}

/* ---- 7.2 用例：DMA0 寄存器写读回 ---- */
static void test_dma_regs(nds_t *nds)
{
    const uint32_t dma = IO_DMA0_BASE;

    bus_write32(nds->bus, dma + 0, 0x02001000u);   /* SAD */
    bus_write32(nds->bus, dma + 4, 0x06000000u);   /* DAD */
    bus_write16(nds->bus, dma + 8, 0x0080u);       /* CNT_L 字数 */
    bus_write16(nds->bus, dma + 10, 0x0000u);      /* CNT_H 控制（不置使能，不触发） */

    CHECK_EQ("DMA SAD write/read",  bus_read32(nds->bus, dma + 0), 0x02001000u);
    CHECK_EQ("DMA DAD write/read",  bus_read32(nds->bus, dma + 4), 0x06000000u);
    CHECK_EQ("DMA CNT_L write/read", bus_read16(nds->bus, dma + 8), 0x0080u);
    CHECK_EQ("DMA CNT_H write/read", bus_read16(nds->bus, dma + 10), 0x0000u);
}

/* ---- 7.3 用例：立即模式同步拷贝 ----
   a) 字块拷贝：Main RAM → VRAM（源/目的递增）；
   b) 半字填色：SRC_FIX 固定源，把一种颜色刷满 VRAM 目标区；
   c) 搬完自动清使能。 */
static void test_dma_copy(nds_t *nds)
{
    const uint32_t dma  = IO_DMA0_BASE;
    const uint32_t src  = 0x02001000u;
    const uint32_t vram = BUS_VRAM_BASE;

    /* a) 4 个字从 Main RAM 拷到 VRAM */
    static const uint32_t words[4] = { 0x11112222u, 0x33334444u, 0x55556666u, 0x77778888u };
    for (int i = 0; i < 4; i++)
        bus_write32(nds->bus, src + 4u * i, words[i]);

    bus_write32(nds->bus, dma + 0, src);            /* SAD = 源 */
    bus_write32(nds->bus, dma + 4, vram);           /* DAD = VRAM 起点 */
    bus_write16(nds->bus, dma + 8, 4u);             /* CNT_L = 4 块 */
    bus_write16(nds->bus, dma + 10, DMA_CNT_32BIT | DMA_CNT_ENABLE); /* 32 位 + 启动 */

    CHECK_EQ("DMA word copy [0]", bus_read32(nds->bus, vram + 0), 0x11112222u);
    CHECK_EQ("DMA word copy [1]", bus_read32(nds->bus, vram + 4), 0x33334444u);
    CHECK_EQ("DMA word copy [2]", bus_read32(nds->bus, vram + 8), 0x55556666u);
    CHECK_EQ("DMA word copy [3]", bus_read32(nds->bus, vram + 12), 0x77778888u);
    CHECK_EQ("DMA enable auto-clear", bus_read16(nds->bus, dma + 10),
             DMA_CNT_32BIT /* 使能位已被硬件清 0 */);

    /* b) 半字填色：源固定，把 0x7C00 刷满 8 个像素（覆盖上面的拷贝结果） */
    bus_write16(nds->bus, src, 0x7C00u);
    bus_write32(nds->bus, dma + 0, src);            /* SAD = 颜色单元 */
    bus_write32(nds->bus, dma + 4, vram);           /* DAD = VRAM 起点 */
    bus_write16(nds->bus, dma + 8, 8u);             /* CNT_L = 8 个半字 */
    bus_write16(nds->bus, dma + 10, DMA_CNT_SRC_FIX | DMA_CNT_ENABLE);

    for (int i = 0; i < 8; i++)
        CHECK_EQ("DMA fill pixel", bus_read16(nds->bus, vram + 2u * i), 0x7C00u);
}

/* ---- 7.4 用例：CPU 程序触发 DMA 填 VRAM 色块 ----
   程序：r0=DMA0 基址 → 写 SAD(颜色单元) → 写 DAD(VRAM) → 一次 STR 32 位写
   CNT(0x81000080：SRC_FIX|ENABLE + 字数 128) 触发搬运 → 停机。
   颜色单元提前放在 0x02001000（程序区之外）。 */
static void test_dma_program(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE;
    const uint32_t color_cell = 0x02001000u;

    static const uint32_t prog[] = {
        /* 0x00 */ 0xE3A00404, /* MOV r0, #0x04000000       r0 = IO 基址 */
        /* 0x04 */ 0xE28000B0, /* ADD r0, r0, #0xB0         r0 = 0x040000B0（DMA0） */
        /* 0x08 */ 0xE3A01402, /* MOV r1, #0x02000000       r1 = Main RAM 基址 */
        /* 0x0C */ 0xE2811A01, /* ADD r1, r1, #0x1000       r1 = 0x02001000（0x01 ROR20） */
        /* 0x10 */ 0xE5801000, /* STR r1, [r0]              SAD = 颜色单元 */
        /* 0x14 */ 0xE3A02406, /* MOV r2, #0x06000000       r2 = VRAM 基址 */
        /* 0x18 */ 0xE5802004, /* STR r2, [r0, #4]          DAD = VRAM */
        /* 0x1C */ 0xE3A03481, /* MOV r3, #0x81000000       r3 = 使能+源固定 */
        /* 0x20 */ 0xE3833080, /* ORR r3, r3, #0x80         r3 = 0x81000080（字数 128） */
        /* 0x24 */ 0xE5803008, /* STR r3, [r0, #8]          CNT → 触发搬运 */
        /* 0x28 */ 0xEAFFFFFE, /* B self（停机） */
    };

    /* 颜色单元：红色 0x7C00（DMA 半字源，SRC_FIX 反复读取） */
    bus_write16(nds->bus, color_cell, 0x7C00u);

    run_program(nds, base, prog, sizeof prog / sizeof prog[0],
                base, base + 0x28, 64);
    CHECK_EQ("dma prog: PC reaches halt", nds->cpu->r[15], base + 0x28);
    CHECK_EQ("dma prog: enable auto-clear", bus_read16(nds->bus, IO_DMA0_BASE + 10),
             DMA_CNT_SRC_FIX);

    /* 128 个半字 = 128 个像素全为红 */
    int ok = 1;
    for (int i = 0; i < 128; i++)
        if (bus_read16(nds->bus, BUS_VRAM_BASE + 2u * i) != 0x7C00u) { ok = 0; break; }
    CHECK_EQ("dma prog: 128px red fill", ok, 1);
}

/* ---- 8.2 用例：双核都能 step ---- */
static void test_dual_core_step(nds_t *nds)
{
    /* 两核各写一条 NOP（MOV r0,r0），reset 到各自入口，step 一次后 PC 应前进 4 */
    bus_write32(nds->bus, 0x02000800u, 0xE1A00000u);
    bus_write32(nds->bus, 0x03800000u, 0xE1A00000u);

    cpu_reset(nds->cpu,  0x02000800u);
    cpu_reset(nds->cpu7, 0x03800000u);

    cpu_step(nds->cpu);
    cpu_step(nds->cpu7);

    CHECK_EQ("arm9 step PC",  nds->cpu->r[15],  0x02000804u);
    CHECK_EQ("arm7 step PC",  nds->cpu7->r[15], 0x03800004u);
    CHECK_EQ("arm9 is_arm7=0", nds->cpu->is_arm7, 0);
    CHECK_EQ("arm7 is_arm7=1", nds->cpu7->is_arm7, 1);
}

/* ---- 8.3 用例：ARM7 WRAM 映射 ---- */
static void test_arm7_wram(nds_t *nds)
{
    bus_write8(nds->bus, BUS_ARM7_WRAM_BASE, 0xAB);
    CHECK_EQ("arm7 wram read8", bus_read8(nds->bus, BUS_ARM7_WRAM_BASE), 0xAB);
    bus_write32(nds->bus, BUS_ARM7_WRAM_BASE + 0x100, 0x12345678u);
    CHECK_EQ("arm7 wram read32", bus_read32(nds->bus, BUS_ARM7_WRAM_BASE + 0x100), 0x12345678u);
    /* 越界（0x03900000 在 64KB WRAM 之外）读 0 */
    CHECK_EQ("arm7 wram beyond", bus_read8(nds->bus, 0x03900000u), 0x00);
}

/* ---- 8.3 用例：ARM7 入口取指执行 ---- */
static void test_arm7_fetch(nds_t *nds)
{
    bus_write32(nds->bus, BUS_ARM7_WRAM_BASE, 0xE3A00005u); /* MOV r0, #5 */
    cpu_reset(nds->cpu7, BUS_ARM7_WRAM_BASE);
    cpu_step(nds->cpu7);
    CHECK_EQ("arm7 fetch r0", nds->cpu7->r[0], 5u);
    CHECK_EQ("arm7 fetch PC", nds->cpu7->r[15], BUS_ARM7_WRAM_BASE + 4);
}

/* ---- 阶段 21-B1 用例：Shared WRAM（0x03000000 32KB + 0x037F8000 镜像） ---- */
static void test_shared_wram(nds_t *nds)
{
    /* 21-B7 起 Shared WRAM 按 WRAMCNT 切分，直接启动默认全给 ARM7。
       旧 B1 用例的本体是「ARM9 视角全 32KB 双向别名」，先切到 WRAMCNT=0。 */
    nds->bus->active_is_arm7 = 0;
    bus_write8(nds->bus, IO_WRAMCNT_ARM9_ADDR, 0x00u);
    CHECK_EQ("wramcnt arm9 set0", bus_read8(nds->bus, IO_WRAMCNT_ARM9_ADDR), 0x00u);

    /* 主区写 32 位，读回一致 */
    bus_write32(nds->bus, BUS_SHARED_WRAM_BASE + 0x100, 0xDEADBEEFu);
    CHECK_EQ("shared main read32", bus_read32(nds->bus, BUS_SHARED_WRAM_BASE + 0x100),
             0xDEADBEEFu);

    /* 主区写 → 镜像区同偏移读到同一数据（别名） */
    bus_write32(nds->bus, BUS_SHARED_WRAM_BASE + 0x200, 0x12345678u);
    CHECK_EQ("shared mirror sees main", bus_read32(nds->bus, BUS_SHARED_WRAM_MIRROR + 0x200),
             0x12345678u);

    /* 镜像区写 → 主区同偏移读到同一数据（反向别名） */
    bus_write16(nds->bus, BUS_SHARED_WRAM_MIRROR + 0x300, 0xBEEFu);
    CHECK_EQ("shared main sees mirror", bus_read16(nds->bus, BUS_SHARED_WRAM_BASE + 0x300),
             0xBEEFu);

    /* 区间末字节可写读回（32KB 边界内） */
    bus_write8(nds->bus, BUS_SHARED_WRAM_BASE + BUS_SHARED_WRAM_SIZE - 1, 0xA5);
    CHECK_EQ("shared last byte", bus_read8(nds->bus, BUS_SHARED_WRAM_BASE + BUS_SHARED_WRAM_SIZE - 1),
             0xA5u);
    CHECK_EQ("shared mirror last byte",
             bus_read8(nds->bus, BUS_SHARED_WRAM_MIRROR + BUS_SHARED_WRAM_SIZE - 1), 0xA5u);

    /* 主区后越界（0x03008000）读 0；镜像区上界恰为 ARM7 WRAM 基址，不在此断言 */
    CHECK_EQ("shared main beyond", bus_read8(nds->bus, BUS_SHARED_WRAM_BASE + BUS_SHARED_WRAM_SIZE),
             0x00u);

    /* 镜像区末字（镜像基址 + 0x7FFC）与主区同物理数据（别名在区间边界仍成立） */
    bus_write32(nds->bus, BUS_SHARED_WRAM_BASE + BUS_SHARED_WRAM_SIZE - 4, 0xAABBCCDDu);
    CHECK_EQ("shared mirror last word",
             bus_read32(nds->bus, BUS_SHARED_WRAM_MIRROR + BUS_SHARED_WRAM_SIZE - 4),
             0xAABBCCDDu);
}

/* ---- 阶段 21-B7 用例：EXMEMCNT / WRAMCNT 寄存器语义 ---- */
static void test_wramcnt_regs(nds_t *nds)
{
    /* 直接启动初值：WRAMCNT=3（Shared WRAM 全给 ARM7）、EXMEMCNT=0xE880 */
    nds->bus->active_is_arm7 = 0;
    CHECK_EQ("exmem arm9 init", bus_read16(nds->bus, IO_EXMEMCNT_ADDR), 0xE880u);
    CHECK_EQ("wramcnt arm9 init", bus_read8(nds->bus, IO_WRAMCNT_ARM9_ADDR), 0x03u);
    nds->bus->active_is_arm7 = 1;
    CHECK_EQ("exmem arm7 init", bus_read16(nds->bus, IO_EXMEMCNT_ADDR), 0xE880u);
    CHECK_EQ("wramcnt arm7 init", bus_read8(nds->bus, IO_WRAMCNT_ARM7_ADDR), 0x03u);

    /* ARM9 写 WRAMCNT=1：ARM9 在 0x247、ARM7 在 0x241 都能读到 */
    nds->bus->active_is_arm7 = 0;
    bus_write8(nds->bus, IO_WRAMCNT_ARM9_ADDR, 0x01u);
    CHECK_EQ("wramcnt arm9 write", bus_read8(nds->bus, IO_WRAMCNT_ARM9_ADDR), 0x01u);
    nds->bus->active_is_arm7 = 1;
    CHECK_EQ("wramcnt arm7 sees", bus_read8(nds->bus, IO_WRAMCNT_ARM7_ADDR), 0x01u);

    /* ARM7 的 0x241 是只读视图：尝试写不会改变状态 */
    bus_write8(nds->bus, IO_WRAMCNT_ARM7_ADDR, 0x00u);
    nds->bus->active_is_arm7 = 0;
    CHECK_EQ("wramcnt arm7 write ignored", bus_read8(nds->bus, IO_WRAMCNT_ARM9_ADDR), 0x01u);

    /* EXMEMCNT：ARM9 写 0xFFFF 只落下可写位（bit13/14 保留），并把高 7 位同步给 ARM7 */
    bus_write16(nds->bus, IO_EXMEMCNT_ADDR, 0xFFFFu);
    CHECK_EQ("exmem arm9 after ff", bus_read16(nds->bus, IO_EXMEMCNT_ADDR), 0xE8FFu);
    nds->bus->active_is_arm7 = 1;
    CHECK_EQ("exmem arm7 sync high", bus_read16(nds->bus, IO_EXMEMCNT_ADDR), 0xE880u);

    /* ARM7 只能写自己的低 7 位：写 0x003F → 0xE8BF，ARM9 那份不变 */
    bus_write16(nds->bus, IO_EXMEMCNT_ADDR, 0x003Fu);
    CHECK_EQ("exmem arm7 low write", bus_read16(nds->bus, IO_EXMEMCNT_ADDR), 0xE8BFu);
    CHECK_EQ("exmem arm7 low byte", bus_read8(nds->bus, IO_EXMEMCNT_ADDR), 0xBFu);
    CHECK_EQ("exmem arm7 high byte", bus_read8(nds->bus, IO_EXMEMCNT_ADDR + 1), 0xE8u);
    nds->bus->active_is_arm7 = 0;
    CHECK_EQ("exmem arm9 unchanged", bus_read16(nds->bus, IO_EXMEMCNT_ADDR), 0xE8FFu);

    /* 复位 WRAMCNT，避免影响同一台机器上的下一个用例（默认 3 = 全给 ARM7） */
    bus_write8(nds->bus, IO_WRAMCNT_ARM9_ADDR, 0x03u);
}

/* ---- 阶段 21-B7 用例：WRAMCNT 切分 Shared WRAM（含 ARM7 未持有时的 ARM7 WRAM 别名） ---- */
static void test_wramcnt_split(nds_t *nds)
{
    /* 默认 3：Shared WRAM 全归 ARM7，ARM9 读 0 / 写忽略 */
    nds->bus->active_is_arm7 = 1;
    bus_write32(nds->bus, BUS_SHARED_WRAM_BASE + 0x100, 0x11111111u);
    CHECK_EQ("mode3 arm7 write", bus_read32(nds->bus, BUS_SHARED_WRAM_BASE + 0x100),
             0x11111111u);
    nds->bus->active_is_arm7 = 0;
    CHECK_EQ("mode3 arm9 blind", bus_read32(nds->bus, BUS_SHARED_WRAM_BASE + 0x100), 0x00u);
    bus_write32(nds->bus, BUS_SHARED_WRAM_BASE + 0x100, 0x22222222u);
    nds->bus->active_is_arm7 = 1;
    CHECK_EQ("mode3 arm9 write ignored", bus_read32(nds->bus, BUS_SHARED_WRAM_BASE + 0x100),
             0x11111111u);

    /* 切到 0：ARM9 全 32KB；ARM7 改经主区/镜像看到自己的 ARM7 WRAM 低/高 32KB */
    nds->bus->active_is_arm7 = 0;
    bus_write8(nds->bus, IO_WRAMCNT_ARM9_ADDR, 0x00u);
    bus_write32(nds->bus, BUS_SHARED_WRAM_BASE + 0x100, 0x22222222u);
    CHECK_EQ("mode0 arm9 write", bus_read32(nds->bus, BUS_SHARED_WRAM_BASE + 0x100),
             0x22222222u);
    nds->bus->active_is_arm7 = 1;
    bus_write8(nds->bus, BUS_SHARED_WRAM_BASE + 0x234, 0xABu);
    CHECK_EQ("mode0 arm7 sees wram low", bus_read8(nds->bus, BUS_ARM7_WRAM_BASE + 0x234),
             0xABu);
    bus_write8(nds->bus, BUS_SHARED_WRAM_MIRROR + 0x234, 0xCDu);
    CHECK_EQ("mode0 arm7 mirror wram high",
             bus_read8(nds->bus, BUS_ARM7_WRAM_BASE + 0x8234), 0xCDu);

    /* 切到 1：ARM9 看到高 16KB（主区/镜像按 16KB 掩码重复），ARM7 看到低 16KB */
    nds->bus->active_is_arm7 = 0;
    bus_write8(nds->bus, IO_WRAMCNT_ARM9_ADDR, 0x01u);
    CHECK_EQ("mode1 arm9 low blind", bus_read32(nds->bus, BUS_SHARED_WRAM_BASE + 0x100),
             0x00u);
    bus_write32(nds->bus, BUS_SHARED_WRAM_BASE + 0x100, 0x33333333u); /* → 高半 0x4100 */
    CHECK_EQ("mode1 arm9 high write", bus_read32(nds->bus, BUS_SHARED_WRAM_BASE + 0x100),
             0x33333333u);
    nds->bus->active_is_arm7 = 1;
    CHECK_EQ("mode1 arm7 low keeps", bus_read32(nds->bus, BUS_SHARED_WRAM_BASE + 0x100),
             0x22222222u);
    bus_write32(nds->bus, BUS_SHARED_WRAM_BASE + 0x100, 0x44444444u);
    nds->bus->active_is_arm7 = 0;
    CHECK_EQ("mode1 arm9 high unchanged", bus_read32(nds->bus, BUS_SHARED_WRAM_BASE + 0x100),
             0x33333333u);
    CHECK_EQ("mode1 mirror arm9 high", bus_read32(nds->bus, BUS_SHARED_WRAM_MIRROR + 0x100),
             0x33333333u);
    nds->bus->active_is_arm7 = 1;
    CHECK_EQ("mode1 mirror arm7 low", bus_read32(nds->bus, BUS_SHARED_WRAM_MIRROR + 0x100),
             0x44444444u);

    /* 切到 2：两核半区互换——ARM9 看低 16KB（ARM7 刚写的 0x44444444），ARM7 看高 16KB */
    nds->bus->active_is_arm7 = 0;
    bus_write8(nds->bus, IO_WRAMCNT_ARM9_ADDR, 0x02u);
    CHECK_EQ("mode2 arm9 low sees", bus_read32(nds->bus, BUS_SHARED_WRAM_BASE + 0x100),
             0x44444444u);
    nds->bus->active_is_arm7 = 1;
    CHECK_EQ("mode2 arm7 high sees", bus_read32(nds->bus, BUS_SHARED_WRAM_BASE + 0x100),
             0x33333333u);
}

/* ---- 阶段 21-B2 用例：IPCSYNC 同步寄存器（双核数据交叉 + 只读/只写位） ---- */
static void test_ipcsync_regs(nds_t *nds)
{
    /* 初始两核都 out=0、enable=0 */
    nds->bus->active_is_arm7 = 0;
    CHECK_EQ("ipcsync arm9 init", bus_read16(nds->bus, IO_IPCSYNC), 0x0000u);
    nds->bus->active_is_arm7 = 1;
    CHECK_EQ("ipcsync arm7 init", bus_read16(nds->bus, IO_IPCSYNC), 0x0000u);

    /* ARM9 写 out=A + enable=1（0x4A00）：自己读回高字节 0x4A；ARM7 在 bit0-3 读到 A */
    nds->bus->active_is_arm7 = 0;
    bus_write16(nds->bus, IO_IPCSYNC, 0x0A00u | IPCSYNC_ENABLE);
    CHECK_EQ("ipcsync arm9 local", bus_read16(nds->bus, IO_IPCSYNC), 0x4A00u);
    nds->bus->active_is_arm7 = 1;
    CHECK_EQ("ipcsync arm7 reads A", bus_read16(nds->bus, IO_IPCSYNC), 0x000Au);

    /* ARM7 写 out=5 + enable=1（0x4500）：ARM7 读 0x450A（含 ARM9 的 A）；
       ARM9 的低 4 位变成 5（读到 ARM7 的 out） */
    bus_write16(nds->bus, IO_IPCSYNC, 0x0500u | IPCSYNC_ENABLE);
    CHECK_EQ("ipcsync arm7 local", bus_read16(nds->bus, IO_IPCSYNC), 0x450Au);
    nds->bus->active_is_arm7 = 0;
    CHECK_EQ("ipcsync arm9 reads 5", bus_read16(nds->bus, IO_IPCSYNC), 0x4A05u);

    /* 字节访问：低字节=对端数据，高字节=本核 out/enable */
    CHECK_EQ("ipcsync arm9 low byte", bus_read8(nds->bus, IO_IPCSYNC), 0x05u);
    CHECK_EQ("ipcsync arm9 high byte", bus_read8(nds->bus, IO_IPCSYNC + 1), 0x4Au);
    nds->bus->active_is_arm7 = 1;
    CHECK_EQ("ipcsync arm7 low byte", bus_read8(nds->bus, IO_IPCSYNC), 0x0Au);
    CHECK_EQ("ipcsync arm7 high byte", bus_read8(nds->bus, IO_IPCSYNC + 1), 0x45u);

    /* ARM9 尝试写低字节（本应是对端只读数据 + 未用位）：out/enable 不变 */
    nds->bus->active_is_arm7 = 0;
    bus_write8(nds->bus, IO_IPCSYNC, 0xFFu);
    CHECK_EQ("ipcsync low-byte read-only", bus_read16(nds->bus, IO_IPCSYNC), 0x4A05u);
}

/* ---- 阶段 21-B2 用例：IPCSYNC bit13 请求 → 对端 IF bit16 ---- */
static void test_ipcsync_irq(nds_t *nds)
{
    /* 清两端 IF，并把 enable/out 全清成初始态 */
    nds->bus->active_is_arm7 = 0;
    bus_write32(nds->bus, IO_IF_ADDR, 0xFFFFFFFFu); /* IF 写 1 清除全部挂起位 */
    bus_write16(nds->bus, IO_IPCSYNC, 0x0000u);
    nds->bus->active_is_arm7 = 1;
    bus_write32(nds->bus, IO_IF_ADDR, 0xFFFFFFFFu);
    bus_write16(nds->bus, IO_IPCSYNC, 0x0000u);

    /* ARM9 未使能接收：ARM7 写 bit13（高字节 0x20）→ ARM9 IF16 不置位 */
    nds->bus->active_is_arm7 = 1;
    bus_write8(nds->bus, IO_IPCSYNC + 1, 0x20u);
    nds->bus->active_is_arm7 = 0;
    CHECK_EQ("ipcsync disabled no IF16",
             bus_read32(nds->bus, IO_IF_ADDR) & IO_IF_IPC_SYNC, 0u);

    /* ARM9 使能接收后再发请求 → IF16 置位；请求位只写不存，读回不带 bit13 */
    bus_write16(nds->bus, IO_IPCSYNC, IPCSYNC_ENABLE);
    nds->bus->active_is_arm7 = 1;
    bus_write8(nds->bus, IO_IPCSYNC + 1, 0x20u);
    nds->bus->active_is_arm7 = 0;
    CHECK_EQ("ipcsync request IF16",
             bus_read32(nds->bus, IO_IF_ADDR) & IO_IF_IPC_SYNC, IO_IF_IPC_SYNC);
    CHECK_EQ("ipcsync request not stored", bus_read16(nds->bus, IO_IPCSYNC),
             IPCSYNC_ENABLE);

    /* IF 写 1 清 0：软件应答后 IF16 应消失 */
    bus_write32(nds->bus, IO_IF_ADDR, 0xFFFFFFFFu);
    CHECK_EQ("ipcsync IF16 cleared",
             bus_read32(nds->bus, IO_IF_ADDR) & IO_IF_IPC_SYNC, 0u);

    /* 反向：ARM7 使能后，ARM9 发请求 → ARM7 IF16 置位 */
    nds->bus->active_is_arm7 = 1;
    bus_write16(nds->bus, IO_IPCSYNC, IPCSYNC_ENABLE);
    nds->bus->active_is_arm7 = 0;
    bus_write8(nds->bus, IO_IPCSYNC + 1, 0x20u);
    nds->bus->active_is_arm7 = 1;
    CHECK_EQ("ipcsync reverse IF16",
             bus_read32(nds->bus, IO_IF_ADDR) & IO_IF_IPC_SYNC, IO_IF_IPC_SYNC);
    nds->bus->active_is_arm7 = 0; /* 恢复默认视角，避免污染后续用例 */
}

/* ---- 阶段 21-B3/B8 用例：Main RAM 无缓存镜像 + ARM9 DTCM/ITCM ---- */
static void test_main_ram_mirror(nds_t *nds)
{
    /* 主区写 → 镜像区同偏移读到同一数据（别名） */
    bus_write32(nds->bus, BUS_MAIN_RAM_BASE + 0x1E0000, 0xDEADBEEFu);
    CHECK_EQ("ram mirror sees main",
             bus_read32(nds->bus, BUS_MAIN_RAM_MIRROR_BASE + 0x1E0000), 0xDEADBEEFu);

    /* 镜像区写 → 主区同偏移读到同一数据（反向别名） */
    bus_write16(nds->bus, BUS_MAIN_RAM_MIRROR_BASE + 0x1000, 0xBEEFu);
    CHECK_EQ("ram main sees mirror",
             bus_read16(nds->bus, BUS_MAIN_RAM_BASE + 0x1000), 0xBEEFu);

    /* FFXII 栈区往返：0x027E3F80（镜像）与主区 0x021E3F80 是同一物理地址 */
    bus_write32(nds->bus, BUS_MAIN_RAM_MIRROR_BASE + 0x1E3F80, 0x020008F8u);
    CHECK_EQ("ffxii stack save",
             bus_read32(nds->bus, BUS_MAIN_RAM_BASE + 0x1E3F80), 0x020008F8u);
    CHECK_EQ("ffxii stack alias",
             bus_read32(nds->bus, BUS_MAIN_RAM_MIRROR_BASE + 0x1E3F80), 0x020008F8u);

    /* ARM7 视角：0x024-0x027 仍是同一块 4MB 主存的解码窗口（melonDS 口径）。
       21-B8 之前把 ARM7 拦在外面是因为模拟器没有 ARM9 DTCM——ARM7 拷贝会覆盖
       “看似 ARM9 栈”的主存字节；真正原因是那些字节属于 ARM9 DTCM，见下方用例。 */
    nds->bus->active_is_arm7 = 1;
    bus_write32(nds->bus, BUS_MAIN_RAM_MIRROR_BASE + 0x3E3B34, 0xDEADBEEFu);
    CHECK_EQ("mirror arm7 write lands",
             bus_read32(nds->bus, BUS_MAIN_RAM_MIRROR_BASE + 0x3E3B34), 0xDEADBEEFu);
    CHECK_EQ("mirror arm7 main sees",
             bus_read32(nds->bus, BUS_MAIN_RAM_BASE + 0x3E3B34), 0xDEADBEEFu);
    nds->bus->active_is_arm7 = 0;

    /* 镜像首字节与末字边界 */
    bus_write8(nds->bus, BUS_MAIN_RAM_MIRROR_BASE, 0xA5);
    CHECK_EQ("mirror first byte", bus_read8(nds->bus, BUS_MAIN_RAM_MIRROR_BASE), 0xA5u);
    CHECK_EQ("mirror first -> main", bus_read8(nds->bus, BUS_MAIN_RAM_BASE), 0xA5u);
    bus_write32(nds->bus, BUS_MAIN_RAM_MIRROR_BASE + BUS_MAIN_RAM_SIZE - 4, 0x10203040u);
    CHECK_EQ("mirror last word",
             bus_read32(nds->bus, BUS_MAIN_RAM_BASE + BUS_MAIN_RAM_SIZE - 4), 0x10203040u);

    /* 越界：镜像上界 0x02800000 读 0 */
    CHECK_EQ("mirror beyond",
             bus_read8(nds->bus, BUS_MAIN_RAM_MIRROR_BASE + BUS_MAIN_RAM_SIZE), 0x00u);
}

/* ---- 阶段 21-B8 用例：ARM9 DTCM / ITCM 与 Main RAM 镜像互不覆盖 ---- */
static void test_arm9_tcm(nds_t *nds)
{
    /* DTCM 配置到 FFXII 复位用的 0x027E0000-0x027E3FFF */
    bus_set_arm9_dtcm(nds->bus, 1, 0x027E0000u, 0x4000u);
    nds->bus->active_is_arm7 = 0;
    bus_write32(nds->bus, 0x027E3B34u, 0xCAFEBABEu); /* ARM9 视角 → DTCM */
    CHECK_EQ("dtcm arm9 write",
             bus_read32(nds->bus, 0x027E3B34u), 0xCAFEBABEu);
    CHECK_EQ("dtcm not in main ram",
             bus_read32(nds->bus, BUS_MAIN_RAM_BASE + 0x3E3B34u), 0x00000000u);

    /* ARM7 写同一地址 → 主存镜像（不碰 DTCM） */
    nds->bus->active_is_arm7 = 1;
    bus_write32(nds->bus, 0x027E3B34u, 0xDEADBEEFu);
    nds->bus->active_is_arm7 = 0;
    CHECK_EQ("arm7 write under dtcm main",
             bus_read32(nds->bus, BUS_MAIN_RAM_BASE + 0x3E3B34u), 0xDEADBEEFu);
    CHECK_EQ("arm7 write under dtcm intact",
             bus_read32(nds->bus, 0x027E3B34u), 0xCAFEBABEu);

    /* ITCM：ARM9 可读写 0x01FF8000，ARM7 访问落空 */
    bus_write32(nds->bus, BUS_ARM9_ITCM_BASE, 0xEA00000Eu);
    CHECK_EQ("itcm arm9 write",
             bus_read32(nds->bus, BUS_ARM9_ITCM_BASE), 0xEA00000Eu);
    nds->bus->active_is_arm7 = 1;
    bus_write32(nds->bus, BUS_ARM9_ITCM_BASE, 0xFFFFFFFFu);
    CHECK_EQ("itcm arm7 write dropped",
             bus_read32(nds->bus, BUS_ARM9_ITCM_BASE), 0x00u);
    nds->bus->active_is_arm7 = 0;
    CHECK_EQ("itcm arm9 after arm7", 
             bus_read32(nds->bus, BUS_ARM9_ITCM_BASE), 0xEA00000Eu);

    bus_set_arm9_dtcm(nds->bus, 0, 0, 0); /* 恢复禁用，避免污染后续用例 */
}

/* ---- 21-B9c 用例：ARM9 IRQ 从 DTCM 槽取 handler（模拟 BIOS 高向量跳板） ---- */
static void test_irq_slot_jump(nds_t *nds)
{
    arm_cpu_t *cpu = nds->cpu;
    const uint32_t base = BUS_MAIN_RAM_BASE;
    const uint32_t handler = BUS_ARM9_ITCM_BASE; /* 0x01FF8000：FFXII 的装法 */

    /* FFXII 复位约定：DTCM 在 0x027E0000，IRQ handler 指针放槽 0x027E3FFC */
    bus_set_arm9_dtcm(nds->bus, 1, 0x027E0000u, 0x4000u);
    nds->bus->active_is_arm7 = 0;
    bus_write32(nds->bus, 0x027E3FFCu, handler);
    /* handler：FFXII 分发器风格——stmdb sp!,{lr} / 破坏 r1 / ldmfd sp!,{pc}。
       FreeBIOS 入口把 lr 设成返回桩 0xFFFF06F0，弹栈后由 bios_irq_tail9
       模拟“ldmia sp!,{r0-r3,r12,r14}; subs pc,r14,#4”恢复现场。 */
    bus_write32(nds->bus, handler + 0x00, 0xE92D4000u); /* STMFD sp!, {lr} */
    bus_write32(nds->bus, handler + 0x04, 0xE3A01011u); /* MOV r1, #0x11（破坏现场） */
    bus_write32(nds->bus, handler + 0x08, 0xE8BD8000u); /* LDMFD sp!, {pc} */

    static const uint32_t prog[] = {
        /* 0x00 */ 0xE3A00005, /* MOV r0, #5 */
        /* 0x04 */ 0xE3A01007, /* MOV r1, #7 */
        /* 0x08 */ 0xEAFFFFFE, /* B self */
    };
    for (size_t i = 0; i < sizeof prog / sizeof prog[0]; i++)
        bus_write32(nds->bus, base + 4 * i, prog[i]);

    /* 模拟启动代码：先分别配置 System 与 IRQ 私有 SP，再回到 System 开中断 */
    cpu->cpsr = ARM_MODE_SYS;
    cpu->r[13] = base + 0x1000;                  /* System SP */
    exec_apply_cpsr(cpu, ARM_MODE_IRQ | CPSR_I);
    cpu->r[13] = base + 0x2000;                  /* IRQ 专用 SP（与 System 栈隔离） */
    exec_apply_cpsr(cpu, ARM_MODE_USER);         /* I=0 允许 IRQ */
    cpu->r[1] = 0xAB;                           /* 被中断代码的“活”寄存器值 */
    nds->io->irq[0].ie  = IO_IF_VBLANK;
    nds->io->irq[0].ifl = IO_IF_VBLANK;
    nds->io->irq[0].ime = 1;
    cpu_reset(cpu, base);

    exec_set_trace(0);
    cpu_step(cpu);   /* IRQ：先建 IRQ 异常现场，再从槽跳用户 handler */
    CHECK_EQ("slot pc=handler", cpu->r[15], handler);
    CHECK_EQ("slot mode=IRQ", cpu->cpsr & CPSR_MODE_MASK, ARM_MODE_IRQ);
    CHECK_EQ("slot I set", (cpu->cpsr & CPSR_I) ? 1u : 0u, 1u);
    CHECK_EQ("slot spsr saved", cpu->spsr[exec_spsr_index(ARM_MODE_IRQ)],
             ARM_MODE_USER);
    CHECK_EQ("slot lr=bios tail", cpu->r[14], 0xFFFF06F0u);
    /* 21-B9x/21-B9wa：真 BIOS 在跳用户 handler 前压 r0-r3/r12/lr 六字帧，
       FFXII ITCM 分发器从 IRQ 栈弹这 6 个字保存现场。 */
    CHECK_EQ("slot frame sp", cpu->r[13], base + 0x2000u - 0x18u);
    CHECK_EQ("slot frame r1", bus_read32(nds->bus, base + 0x2000u - 0x14u),
             0xABu);
    CHECK_EQ("slot frame ret", bus_read32(nds->bus, base + 0x2000u - 0x04u),
             base + 4u);

    nds->io->irq[0].ifl = 0; /* 模拟 handler 已写 IF 清除，避免返回后立即重入 */
    cpu_step(cpu);           /* STMFD sp!, {lr}：把返回点压栈 */
    cpu_step(cpu);           /* MOV r1, #0x11：破坏被中断现场的 r1 */
    CHECK_EQ("slot r1 clobbered", cpu->r[1], 0x11);
    cpu_step(cpu);           /* LDMFD sp!, {pc}：弹回 BIOS 返回桩 */
    CHECK_EQ("slot pop pc", cpu->r[15], 0xFFFF06F0u);
    CHECK_EQ("slot pop mode", cpu->cpsr & CPSR_MODE_MASK, ARM_MODE_IRQ);

    cpu_step(cpu);           /* bios_irq_tail9：弹六字帧 + SUBS 返回被中断 PC */
    CHECK_EQ("slot r1 restored", cpu->r[1], 0xAB);
    CHECK_EQ("slot return mode", cpu->cpsr & CPSR_MODE_MASK, ARM_MODE_USER);
    CHECK_EQ("slot return I", (cpu->cpsr & CPSR_I) ? 1u : 0u, 0u);
    CHECK_EQ("slot return PC", cpu->r[15], base);

    cpu_step(cpu);           /* 执行被中断的 MOV r0,#5 */
    CHECK_EQ("slot instr rerun", cpu->r[0], 5u);
    CHECK_EQ("slot return PC2", cpu->r[15], base + 4);
    CHECK_EQ("slot irq sp restored",
             cpu->r13_bank[exec_spsr_index(ARM_MODE_IRQ)], base + 0x2000u);
    exec_set_trace(1);

    bus_set_arm9_dtcm(nds->bus, 0, 0, 0); /* 恢复禁用，避免污染后续用例 */
}

/* ---- 21-B9d 用例：CLZ（数前导零）——FFXII IRQ 分发器依赖它找最高挂起位 ---- */
static void test_clz(nds_t *nds)
{
    arm_cpu_t *cpu = nds->cpu;
    const uint32_t base = BUS_MAIN_RAM_BASE;
    static const struct { uint32_t val; uint32_t want; const char *name; } cases[] = {
        { 0x00000000u, 32u, "clz zero"        },
        { 0x80000000u,  0u, "clz top bit"     },
        { 0x00000001u, 31u, "clz low bit"     },
        { 0x00042009u, 13u, "clz ffxii ie"    }, /* 最高位 bit18：前导 13 个零 */
        { IO_IF_VBLANK, 31u, "clz vblank bit0" },
    };

    bus_write32(nds->bus, base, 0xE16F0F11u); /* CLZ r0, r1 */
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        cpu->r[1] = cases[i].val;
        cpu_reset(cpu, base);
        exec_set_trace(0);
        cpu_step(cpu);
        CHECK_EQ(cases[i].name, cpu->r[0], cases[i].want);
        CHECK_EQ("clz pc", cpu->r[15], base + 4);
    }
    /* regression: CLZ into nonzero Rd (r10) must still dispatch as CLZ */
    bus_write32(nds->bus, base, 0xE16FAF13u); /* CLZ r10, r3 */
    cpu->r[3] = 0x617730B0u;
    cpu->cpsr = 0x8000001Fu;
    cpu_reset(cpu, base);
    exec_set_trace(0);
    cpu_step(cpu);
    CHECK_EQ("clz r10 nonzero rd", cpu->r[10], 1u);
    CHECK_EQ("clz no flag touch", cpu->cpsr, 0x8000001Fu);
    CHECK_EQ("clz r10 pc", cpu->r[15], base + 4);
    exec_set_trace(1);
}

/* ---- 21-B9e 用例：ARM BLX Rm（寄存器间接调用，FFXII 0x02006488 依赖） ---- */
/* ---- 21-B9wk：ARMv5 DSP 乘法（标题位流解码 0x01FFD904 实际依赖） ---- */
static void test_dsp_mul(nds_t *nds)
{
    arm_cpu_t *cpu = nds->cpu;
    const uint32_t base = BUS_MAIN_RAM_BASE;
    exec_set_trace(0);

    /* SMULBB r9,r4,r5：低16×低16（FFEE=-18, 0160=352 → FFFFE740） */
    bus_write32(nds->bus, base, 0xE1690584u);
    cpu->r[4] = 0xFFFFFFEEu; cpu->r[5] = 0x00000160u;
    cpu_reset(cpu, base); cpu_step(cpu);
    CHECK_EQ("smulbb low*low", cpu->r[9], 0xFFFFE740u);

    /* SMULBT r9,r4,r5：低16×高16（FFEE=-18, 高=2 → FFFFFFDC） */
    bus_write32(nds->bus, base, 0xE16905C4u);
    cpu->r[4] = 0xFFFFFFEEu; cpu->r[5] = 0x00020000u;
    cpu_reset(cpu, base); cpu_step(cpu);
    CHECK_EQ("smulbt low*high", cpu->r[9], 0xFFFFFFDCu);

    /* SMULTB r9,r4,r5：高16×低16（3×16=48） */
    bus_write32(nds->bus, base, 0xE16905A4u);
    cpu->r[4] = 0x00030000u; cpu->r[5] = 0x00000010u;
    cpu_reset(cpu, base); cpu_step(cpu);
    CHECK_EQ("smultb high*low", cpu->r[9], 48u);

    /* SMLABB r3,r5,r4：7×9+7=70 */
    bus_write32(nds->bus, base, 0xE1034584u);
    cpu->r[4] = 7u; cpu->r[5] = 9u;
    cpu_reset(cpu, base); cpu_step(cpu);
    CHECK_EQ("smlabb add", cpu->r[3], 70u);

    /* SMLABB 溢出：0x7FFF*0x7FFF + 0x7FFFFFFF → Q(bit27) 置位 */
    bus_write32(nds->bus, base, 0xE1035684u);
    cpu->r[4] = 0x7FFFu; cpu->r[5] = 0x7FFFFFFFu; cpu->r[6] = 0x7FFFu;
    cpu->cpsr = 0;
    cpu_reset(cpu, base); cpu_step(cpu);
    CHECK_EQ("smlabb q flag", (cpu->cpsr >> 27) & 1u, 1u);

    /* SMULWy r6,r7,r8：32×低16 → >>16（0x10000×4>>16=4） */
    bus_write32(nds->bus, base, 0xE12608A7u);
    cpu->r[7] = 0x00010000u; cpu->r[8] = 4u;
    cpu_reset(cpu, base); cpu_step(cpu);
    CHECK_EQ("smulwb", cpu->r[6], 4u);

    /* SMLALBB r0,r1,r2,r3：64 位累加 0+2*3 */
    bus_write32(nds->bus, base, 0xE1401283u);
    cpu->r[0] = 0u; cpu->r[1] = 0u; cpu->r[2] = 2u; cpu->r[3] = 3u;
    cpu_reset(cpu, base); cpu_step(cpu);
    CHECK_EQ("smlalbb lo", cpu->r[1], 6u);
    CHECK_EQ("smlalbb hi", cpu->r[0], 0u);

    /* 回归：bit20=1 的 CMP_REG（E15100CC）不是 DSP 乘法，ARM7 不应进未定义异常 */
    {
        arm_cpu_t *cpu7 = nds->cpu7;
        const uint32_t base7 = BUS_ARM7_WRAM_BASE + 0x2000u;
        bus_write32(nds->bus, base7, 0xE15100CCu); /* cmp r1, r12, asr #1 */
        cpu7->r[1] = 5u; cpu7->r[12] = 3u;
        cpu7->cpsr = 0x1Fu;
        cpu_reset(cpu7, base7);
        cpu_step(cpu7);
        CHECK_EQ("cmp not dsp pc", cpu7->r[15], base7 + 4u);
        CHECK_EQ("cmp not dsp mode", cpu7->cpsr & 0x1Fu, 0x1Fu);
    }

    exec_set_trace(1);
}

/* ---- 21-B9e 用例：ARM BLX Rm（寄存器间接调用，FFXII 0x02006488 依赖） ---- */
static void test_arm_blx_reg(nds_t *nds)
{
    arm_cpu_t *cpu = nds->cpu;
    const uint32_t base = BUS_MAIN_RAM_BASE;
    const uint32_t thumb_fn = 0x02001000u;
    const uint32_t arm_fn   = 0x02001100u;

    /* BLX r1（编码 0xE12FFF31，rm=r1）：lr=PC+4，目标按 LSB 切 Thumb/ARM */
    bus_write32(nds->bus, base, 0xE12FFF31u);
    bus_write16(nds->bus, thumb_fn, 0x2000); /* Thumb: MOVS r0, #0 */
    bus_write32(nds->bus, arm_fn, 0xE3A0002Au); /* ARM: MOV r0, #42 */

    /* ARM → Thumb：r1=0x02001001 */
    cpu->cpsr = 0;
    cpu->r[1] = thumb_fn | 1u;
    cpu_reset(cpu, base);
    exec_set_trace(0);
    cpu_step(cpu);
    CHECK_EQ("blxr thumb lr", cpu->r[14], base + 4);
    CHECK_EQ("blxr thumb T", (cpu->cpsr & CPSR_T) ? 1u : 0u, 1u);
    CHECK_EQ("blxr thumb PC", cpu->r[15], thumb_fn);
    cpu_step(cpu); /* 执行目标函数第一条 Thumb */
    CHECK_EQ("blxr thumb r0", cpu->r[0], 0u);
    CHECK_EQ("blxr thumb PC2", cpu->r[15], thumb_fn + 2);

    /* ARM → ARM：r1=0x02001100（bit0=0，留在 ARM） */
    cpu->cpsr = 0;
    cpu->r[1] = arm_fn;
    cpu_reset(cpu, base);
    cpu_step(cpu);
    CHECK_EQ("blxr arm lr", cpu->r[14], base + 4);
    CHECK_EQ("blxr arm T", (cpu->cpsr & CPSR_T) ? 1u : 0u, 0u);
    CHECK_EQ("blxr arm PC", cpu->r[15], arm_fn);
    cpu_step(cpu);
    CHECK_EQ("blxr arm r0", cpu->r[0], 42u);
    CHECK_EQ("blxr arm PC2", cpu->r[15], arm_fn + 4);
    exec_set_trace(1);
}

/* ---- 21-B9s 用例：ARM BLX 立即数（安全区 Thumb SWI 桩入口，FFXII
   0x02012500 跳 0x020007A8 依赖：svc #N + bx lr） ---- */
static void test_arm_blx_imm(nds_t *nds)
{
    arm_cpu_t *cpu = nds->cpu;
    const uint32_t base = BUS_MAIN_RAM_BASE;
    const uint32_t thumb_fn = 0x02001000u;
    const uint32_t off = (thumb_fn - (base + 8)) >> 2;

    /* BLX 立即数（0xFA000000 | off24）：lr=PC+4，无条件切 Thumb */
    bus_write32(nds->bus, base, 0xFA000000u | off);
    bus_write16(nds->bus, thumb_fn, 0x2000); /* Thumb: MOVS r0, #0 */

    cpu->cpsr = 0;
    cpu_reset(cpu, base);
    exec_set_trace(0);
    cpu_step(cpu);
    CHECK_EQ("blxi thumb lr", cpu->r[14], base + 4);
    CHECK_EQ("blxi thumb T", (cpu->cpsr & CPSR_T) ? 1u : 0u, 1u);
    CHECK_EQ("blxi thumb PC", cpu->r[15], thumb_fn);
    cpu_step(cpu);
    CHECK_EQ("blxi thumb r0", cpu->r[0], 0u);
    CHECK_EQ("blxi thumb PC2", cpu->r[15], thumb_fn + 2);
    exec_set_trace(1);
}

/* ---- 21-B9g 用例：模式私有 r13/r14（SVC/IRQ/System 栈互不覆盖） ---- */
static void test_banked_r13_r14(nds_t *nds)
{
    arm_cpu_t *cpu = nds->cpu;

    /* FFXII 启动值：System sp=0x027E3AF8，SVC sp=0x027E3FC0，IRQ sp=0x027E3F7C */
    cpu->cpsr = ARM_MODE_SYS;
    cpu->r[13] = 0x027E3AF8u;
    cpu->r[14] = 0x11111111u;
    exec_apply_cpsr(cpu, ARM_MODE_SVC);
    CHECK_EQ("bank svc fresh sp", cpu->r[13], 0u);
    CHECK_EQ("bank svc fresh lr", cpu->r[14], 0u);

    cpu->r[13] = 0x027E3FC0u;
    cpu->r[14] = 0x22222222u;
    exec_apply_cpsr(cpu, ARM_MODE_SYS);
    CHECK_EQ("bank sys sp kept", cpu->r[13], 0x027E3AF8u);
    CHECK_EQ("bank sys lr kept", cpu->r[14], 0x11111111u);

    exec_apply_cpsr(cpu, ARM_MODE_SVC);
    CHECK_EQ("bank svc sp kept", cpu->r[13], 0x027E3FC0u);
    CHECK_EQ("bank svc lr kept", cpu->r[14], 0x22222222u);

    exec_apply_cpsr(cpu, ARM_MODE_IRQ | CPSR_I);
    CHECK_EQ("bank irq fresh sp", cpu->r[13], 0u);
    cpu->r[13] = 0x027E3F7Cu;
    cpu->r[14] = 0x44444444u;
    exec_apply_cpsr(cpu, ARM_MODE_SYS);
    exec_apply_cpsr(cpu, ARM_MODE_IRQ | CPSR_I);
    CHECK_EQ("bank irq sp kept", cpu->r[13], 0x027E3F7Cu);
    CHECK_EQ("bank irq lr kept", cpu->r[14], 0x44444444u);
    CHECK_EQ("bank mode", cpu->cpsr & CPSR_MODE_MASK, ARM_MODE_IRQ);
}

/* ---- 21-B9h 用例：LDM ^（列表不含 PC）载入 User 槽 r13/r14 ---- */
static void test_ldm_user_bank(nds_t *nds)
{
    arm_cpu_t *cpu = nds->cpu;
    const uint32_t base = BUS_MAIN_RAM_BASE;
    const uint32_t mem = base + 0x800;

    /* 当前在 SVC 模式：SVC 私有 SP/LR 与 User/System 槽必须互不影响 */
    cpu->cpsr = ARM_MODE_SYS;
    cpu->r[13] = 0x027E3AF8u;
    cpu->r[14] = 0x11111111u;
    exec_apply_cpsr(cpu, ARM_MODE_SVC);
    cpu->r[13] = 0x027E3FC0u;
    cpu->r[14] = 0x22222222u;

    /* 内存里放一份“任务现场”：r0-r12 各异，r13/r14 是要装进 User 槽的值 */
    for (int i = 0; i < 15; i++)
        bus_write32(nds->bus, mem + 4u * (uint32_t)i,
                    (uint32_t)(0x100 + i));
    bus_write32(nds->bus, mem + 4u * 13u, 0x03000000u);
    bus_write32(nds->bus, mem + 4u * 14u, 0x04000000u);

    bus_write32(nds->bus, base, 0xE8D07FFFu); /* LDMIA r0, {r0-r14}^ */
    cpu->r[0] = mem;
    cpu_reset(cpu, base);
    exec_set_trace(0);
    cpu_step(cpu);

    CHECK_EQ("ldmu pc", cpu->r[15], base + 4);
    CHECK_EQ("ldmu r1 loaded", cpu->r[1], 0x101u);
    CHECK_EQ("ldmu svc sp kept", cpu->r[13], 0x027E3FC0u);
    CHECK_EQ("ldmu svc lr kept", cpu->r[14], 0x22222222u);
    exec_apply_cpsr(cpu, ARM_MODE_SYS);
    CHECK_EQ("ldmu user sp loaded", cpu->r[13], 0x03000000u);
    CHECK_EQ("ldmu user lr loaded", cpu->r[14], 0x04000000u);
    exec_set_trace(1);
}

/* ---- 21-B9a: BIOS SWI 0x0E GetCRC16 (CRC-16/IBM) ---- */
static void test_bios_crc16(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE;
    const uint32_t data = 0x02001000u;
    arm_cpu_t *cpu = nds->cpu;
    static const uint32_t prog_arm[] = {
        /* 0x00 */ 0xEF0E0000, /* SWI 0x0E (GetCRC16) */
        /* 0x04 */ 0xEAFFFFFE, /* B self */
    };
    static const uint16_t prog_thumb[] = {
        0xDF0E, /* SWI 0x0E (GetCRC16) */
        0xE7FE, /* B self */
    };

    /* ASCII "123456789" 写入 Main RAM 供 SWI 读取 */
    for (int i = 0; i < 9; i++)
        bus_write8(nds->bus, data + (uint32_t)i, (uint8_t)('1' + i));

    /* ARM9/ARM 态：r0=0xFFFF 初值、r1=数据地址、r2=9 字节
       CRC-16/IBM("123456789", init=0xFFFF) = 0x4B37 */
    cpu->r[0] = 0xFFFFu;
    cpu->r[1] = data;
    cpu->r[2] = 9u;
    run_program(nds, base, prog_arm, 2, base, base + 4, 8);
    CHECK_EQ("crc16 arm result", cpu->r[0], 0x4B37u);
    CHECK_EQ("crc16 arm pc", cpu->r[15], base + 4u);

    /* 长度 0 时保持初值不变 */
    cpu->r[0] = 0xFFFFu;
    cpu->r[1] = data;
    cpu->r[2] = 0u;
    run_program(nds, base, prog_arm, 2, base, base + 4, 8);
    CHECK_EQ("crc16 arm len0", cpu->r[0], 0xFFFFu);

    /* Thumb 态同一组参数应得到同一结果 */
    cpu->r[0] = 0xFFFFu;
    cpu->r[1] = data;
    cpu->r[2] = 9u;
    thumb_write(nds, base + 0x200, prog_thumb, 2);
    thumb_start(nds, base + 0x200);
    cpu_step(cpu);
    CHECK_EQ("crc16 thumb result", cpu->r[0], 0x4B37u);
    CHECK_EQ("crc16 thumb pc", cpu->r[15], base + 0x202u);
    thumb_stop(nds);

    /* ARM7 核（真机首次调用点就在 ARM7）：21-B9yd 起，SWI 0x0E 在真地址
       0x134C 上真实执行 FreeBIOS 字节码（此前是 C 版 HLE 直接算结果）。
       FreeBIOS 的 ARM7 实现按**半字**推进（`lsrs r2,r2,#1` →
       ⌊n/2⌋ 轮、每轮吃 2 字节），且先把初值 bic 成 0x0000FFFF。
       8 字节 "12345678"（init=0xFFFF）→ **0x37DD**，正好等于独立的
       CRC-16/ARC（反射 0xA001）结果——说明本地是把真实字节码算对了，
       而不是"碰巧"；步数预算从 8 提到 256（真实执行需要 ~70 步）。 */
    nds->cpu7->r[0] = 0xFFFFu;
    nds->cpu7->r[1] = data;
    nds->cpu7->r[2] = 8u;
    run_cpu7(nds, base + 0x400, prog_arm, 2, base + 0x400, base + 0x404, 256);
    CHECK_EQ("crc16 cpu7 result(8B)", nds->cpu7->r[0], 0x37DDu);

    /* 奇数长度：FreeBIOS 只处理 ⌊n/2⌋ 个半字，第 9 个字节被忽略
       （真机 BIOS 支持任意长度，这是 FreeBIOS 与真 BIOS 的已知差异）。 */
    nds->cpu7->r[0] = 0xFFFFu;
    nds->cpu7->r[1] = data;
    nds->cpu7->r[2] = 9u;
    run_cpu7(nds, base + 0x400, prog_arm, 2, base + 0x400, base + 0x404, 256);
    CHECK_EQ("crc16 cpu7 odd-len tail ignored", nds->cpu7->r[0], 0x37DDu);
}

/* ---- 21-B9l：BIOS SWI 0x08 SoundBias（仅 ARM7；FFXII 启动在此卡住前） ---- */
static void test_bios_soundbias(nds_t *nds)
{
    const uint32_t base = BUS_ARM7_WRAM_BASE + 0x1800u;
    static const uint16_t prog[] = {
        0xDF08, /* SWI 0x08 (SoundBias) */
        0xE7FE, /* B self */
    };

    nds->bus->active_is_arm7 = 1;
    thumb_write(nds, base, prog, 2);

    /* r0≠0 → SOUNDBIAS 电平调到 0x200 */
    bus_write16(nds->bus, SND_SOUNDBIAS, 0x0100u);
    arm_cpu_t *cpu = nds->cpu7;
    a7_setup_stacks(cpu);
    cpu->r[0] = 0x1234u;
    cpu->r[1] = 8u;
    cpu->cpsr |= CPSR_T;
    cpu_reset(cpu, base);
    a7_run_to_pc(cpu, base + 2u);   /* SWI → 低地址分发器 → swi_complete */
    CHECK_EQ("soundbias arm7 pc", cpu->r[15], base + 2u);
    CHECK_EQ("soundbias to 0x200",
             bus_read16(nds->bus, SND_SOUNDBIAS), 0x0200u);

    /* 21-B9yd：SWI 0x08 现在在真地址 0x11C8 上真实执行 FreeBIOS 字节码：
       该实现**不看 r0**，只做「当前值非 0 → 写回 0x200」。所以把当前值清零后
       再调一次，电平保持 0x000（真实字节码的第二个分支）。 */
    bus_write16(nds->bus, SND_SOUNDBIAS, 0x0000u);
    cpu->r[0] = 0u;
    cpu->cpsr |= CPSR_T;
    cpu_reset(cpu, base);
    a7_run_to_pc(cpu, base + 2u);
    CHECK_EQ("soundbias arm7 pc2", cpu->r[15], base + 2u);
    CHECK_EQ("soundbias keeps 0x000",
             bus_read16(nds->bus, SND_SOUNDBIAS), 0x0000u);

    /* FreeBIOS 口径：非 0 的电平会被**改写回 0x200**（与 r0 无关） */
    bus_write16(nds->bus, SND_SOUNDBIAS, 0x0100u);
    cpu->r[0] = 0u;
    cpu->cpsr |= CPSR_T;
    cpu_reset(cpu, base);
    a7_run_to_pc(cpu, base + 2u);
    CHECK_EQ("soundbias forces 0x200",
             bus_read16(nds->bus, SND_SOUNDBIAS), 0x0200u);

    cpu->cpsr &= ~CPSR_T;
    nds->bus->active_is_arm7 = 0;
}

/* ---- 21-B9wi：ARM7 BIOS 音频查表 SWI 0x1A-0x1D（FFXII 在 0x0380443C
   Thumb SWI 0x1C 处卡住后漂移的根因回归） ---- */
static void test_bios_audio_tables(nds_t *nds)
{
    const uint32_t base = BUS_ARM7_WRAM_BASE + 0x1900u;
    static const uint16_t prog[] = {
        0xDF1A, /* SWI 0x1A GetSineTable   */
        0xDF1B, /* SWI 0x1B GetPitchTable  */
        0xDF1C, /* SWI 0x1C GetVolumeTable */
        0xDF1D, /* SWI 0x1D GetBootProcs   */
        0xE7FE,
    };

    nds->bus->active_is_arm7 = 1;
    thumb_write(nds, base, prog, 5);
    arm_cpu_t *cpu = nds->cpu7;
    a7_setup_stacks(cpu);

    cpu->cpsr = CPSR_T;
    cpu->r[0] = 1u; /* sine 索引 1 -> 0x0324 */
    cpu_reset(cpu, base);
    a7_run_to_pc(cpu, base + 2u);
    CHECK_EQ("sine pc", cpu->r[15], base + 2u);
    CHECK_EQ("sine r0", cpu->r[0], 0x0324u);

    cpu->cpsr = CPSR_T;
    cpu->r[0] = 1u; /* pitch 索引 1 -> 0x003B */
    cpu_reset(cpu, base + 2u);
    a7_run_to_pc(cpu, base + 4u);
    CHECK_EQ("pitch pc", cpu->r[15], base + 4u);
    CHECK_EQ("pitch r0", cpu->r[0], 0x003Bu);

    cpu->cpsr = CPSR_T;
    cpu->r[0] = 0x2A0u; /* FFXII 实际调用参数 -> 0x47 */
    cpu_reset(cpu, base + 4u);
    a7_run_to_pc(cpu, base + 6u);
    CHECK_EQ("volume pc", cpu->r[15], base + 6u);
    CHECK_EQ("volume r0", cpu->r[0], 0x47u);

    cpu->cpsr = CPSR_T;
    cpu_reset(cpu, base + 6u);
    a7_run_to_pc(cpu, base + 8u);
    CHECK_EQ("boot pc", cpu->r[15], base + 8u);
    CHECK_EQ("boot r0", cpu->r[0], 0x00000A2Eu);
    CHECK_EQ("boot r1", cpu->r[1], 0x00002C3Cu);
    CHECK_EQ("boot r2", cpu->r[2], 0x000005FFu);

    cpu->cpsr = 0;
    nds->bus->active_is_arm7 = 0;
}

/* ---- 21-B9i 用例：FIFO CNT 高字节 0xC4 = 错误应答 + 使能 + 收 IRQ ---- */
static void test_fifo_cnt_combine(nds_t *nds)
{
    /* FFXII 启动代码对两核各写：低字节 0x08（清发送）、高字节 0xC4。
       旧实现把含错误位的写入当“纯应答”，漏掉使能位导致 CNT 恒 0。 */
    for (int arm7 = 0; arm7 <= 1; arm7++) {
        nds->bus->active_is_arm7 = arm7;
        bus_write8(nds->bus, IO_FIFO_CNT, 0x08u);
        bus_write8(nds->bus, IO_FIFO_CNT + 1, 0xC4u);
    }
    nds->bus->active_is_arm7 = 0;
    uint16_t c9 = bus_read16(nds->bus, IO_FIFO_CNT);
    CHECK_EQ("fifo cnt9 enable", c9 & FIFO_CNT_ENABLE, FIFO_CNT_ENABLE);
    CHECK_EQ("fifo cnt9 recvirq", c9 & FIFO_CNT_RECV_IRQ, FIFO_CNT_RECV_IRQ);

    nds->io->irq[1].ifl = 0;
    bus_write32(nds->bus, IO_FIFO_SEND, 0x12345678u);
    CHECK_EQ("fifo from9 count", nds->io->fifo.from9.count, 1);
    CHECK_EQ("fifo arm7 IF18",
             nds->io->irq[1].ifl & IO_IF_FIFO_RECV_NOT_EMPTY,
             IO_IF_FIFO_RECV_NOT_EMPTY);
    nds->bus->active_is_arm7 = 0;
}

/* ---- 21-B9i 用例：ARM7 IRQ 槽（0x0380FFFC）跳用户 handler + FreeBIOS 出入口 ----
   21-B9wt 起 IRQ 走真机路径：异常 → 低向量 0x18 → 0x1FB0 压六字帧 +
   lr=0x1FC0 → [0x03FFFFFC] 用户 handler → 0x1FC0 弹帧 → 0x1FC4 异常返回。 */
static void test_arm7_irq_slot(nds_t *nds)
{
    arm_cpu_t *cpu = nds->cpu7;
    const uint32_t pc = BUS_ARM7_WRAM_BASE + 0x2000;      /* 被打断点 */
    const uint32_t handler = BUS_ARM7_WRAM_BASE + 0x1000; /* handler 代码 */
    const uint32_t irq_sp = BUS_ARM7_WRAM_BASE + 0x6000;  /* IRQ 栈 */

    nds->bus->active_is_arm7 = 1;
    bus_write32(nds->bus, 0x0380FFFCu, handler);           /* ARM7 BIOS 槽 */
    bus_write32(nds->bus, pc, 0xE1A00000u);                /* NOP 被中断指令 */
    bus_write32(nds->bus, handler + 0x00, 0xE92D4000u);    /* STMFD sp!,{lr} */
    bus_write32(nds->bus, handler + 0x04, 0xE3A01011u);    /* MOV r1,#0x11 */
    bus_write32(nds->bus, handler + 0x08, 0xE8BD8000u);    /* LDMFD sp!,{pc} */

    cpu->cpsr = ARM_MODE_SYS;
    cpu->r[13] = BUS_ARM7_WRAM_BASE + 0x5000;
    exec_apply_cpsr(cpu, ARM_MODE_IRQ | CPSR_I);
    cpu->r[13] = irq_sp;
    cpu->r13_bank[2] = BUS_ARM7_WRAM_BASE + 0x7800u;       /* SVC 栈（本例不用） */
    exec_apply_cpsr(cpu, ARM_MODE_USER);
    cpu->r[1] = 0xABu;
    nds->io->irq[1].ime = 1;
    nds->io->irq[1].ie = IO_IF_FIFO_RECV_NOT_EMPTY;
    nds->io->irq[1].ifl = IO_IF_FIFO_RECV_NOT_EMPTY;
    cpu_reset(cpu, pc);
    exec_set_trace(0);

    cpu_step(cpu);                                        /* IRQ 异常 → 低向量 */
    CHECK_EQ("arm7 slot pc=vec", cpu->r[15], 0x00000018u);
    CHECK_EQ("arm7 slot mode=IRQ", cpu->cpsr & CPSR_MODE_MASK, ARM_MODE_IRQ);
    nds->io->irq[1].ifl = 0;
    cpu_step(cpu);                                        /* 向量 → 0x1FB0 */
    cpu_step(cpu);                                        /* 压六字帧 */
    cpu_step(cpu);                                        /* mov r0,#0x04000000 */
    cpu_step(cpu);                                        /* mov lr,pc → 0x1FC0 */
    cpu_step(cpu);                                        /* ldr pc,[0x03FFFFFC] → handler */
    CHECK_EQ("arm7 slot pc=handler", cpu->r[15], handler);
    cpu_step(cpu); /* STMFD sp!,{lr} */
    cpu_step(cpu); /* MOV r1,#0x11 */
    CHECK_EQ("arm7 slot r1 clobbered", cpu->r[1], 0x11u);
    cpu_step(cpu); /* LDMFD sp!,{pc} -> 返回 FreeBIOS 0x1FC0 桩 */
    CHECK_EQ("arm7 slot tail pc", cpu->r[15], 0x00001FC0u);
    cpu_step(cpu); /* FreeBIOS 尾部：弹六字帧 */
    CHECK_EQ("arm7 slot tail pc2", cpu->r[15], 0x00001FC4u);
    cpu_step(cpu); /* subs pc,lr,#4 + SPSR 恢复 -> 返回被打断点 */
    CHECK_EQ("arm7 slot ret pc", cpu->r[15], pc);
    cpu_step(cpu); /* 重执行 NOP */
    CHECK_EQ("arm7 slot r1 restored", cpu->r[1], 0xABu);
    CHECK_EQ("arm7 slot mode restored", cpu->cpsr & CPSR_MODE_MASK,
             ARM_MODE_USER);
    CHECK_EQ("arm7 slot pc advanced", cpu->r[15], pc + 4);
    exec_set_trace(1);
}

/* ---- 21-B9j 用例：TM3 重载值 + 溢出置 IF（FFXII service6 依赖） ---- */
static void test_timer_reload_overflow(nds_t *nds)
{
    arm_cpu_t *cpu = nds->cpu7;
    const uint32_t base = BUS_ARM7_WRAM_BASE;
    const uint32_t tm3_l = IO_TIMER0_BASE + 3u * IO_TIMER_STRIDE;      /* 0x0400010C */
    const uint32_t tm3_h = tm3_l + 2;                                   /* 0x0400010E */

    nds->bus->active_is_arm7 = 1;
    bus_write32(nds->bus, base, 0xEAFFFFFEu); /* ARM7 死循环，让定时器随步进 */
    bus_write16(nds->bus, tm3_l, 0xFFF0u);    /* 先写重载值 */
    bus_write16(nds->bus, tm3_h, 0x00C0u);    /* 使能 + IRQ + 1:1 */
    CHECK_EQ("tm3 reload loaded", nds->io->timer[1][3].cnt_l, 0xFFF0u);

    nds->io->irq[1].ifl = 0;
    cpu_reset(cpu, base);
    exec_set_trace(0);
    for (int i = 0; i < 15; i++)
        cpu_step(cpu);
    CHECK_EQ("tm3 no overflow yet", nds->io->irq[1].ifl & 0x40u, 0u);
    cpu_step(cpu); /* 第 16 个周期：0xFFF0→0xFFFF 后回绕 */
    CHECK_EQ("tm3 overflow IF bit6", nds->io->irq[1].ifl & 0x40u, 0x40u);
    CHECK_EQ("tm3 reloaded", nds->io->timer[1][3].cnt_l, 0xFFF0u);
    exec_set_trace(1);
    nds->bus->active_is_arm7 = 0;
}

/* ---- 21-B9j+ 用例：SPI device1（固件 Flash）按片选事务解析 + 合法用户区 ---- */
static void test_spi_fw_hle(nds_t *nds)
{
    /* 按 FFXII 实际序列：SPICNT=0x8900（使能+device1+保持片选）。
       0x03 后跟 3 字节大端地址：首地址字节可以就是 0x03（0x3FE00），
       不应再被当成新的 READ 命令。 */
    nds->bus->active_is_arm7 = 1;
    bus_write8(nds->bus, IO_SPICNT, 0x00u);
    bus_write8(nds->bus, IO_SPICNT + 1, 0x89u); /* 使能 + device1 */
    bus_write8(nds->bus, IO_SPIDATA, 0x03u);    /* READ 命令 */
    CHECK_EQ("fw cmd echo 0xff",
             bus_read8(nds->bus, IO_SPIDATA), 0xFFu);
    bus_write8(nds->bus, IO_SPIDATA, 0x03u);    /* 地址 0x3FE00 首字节 */
    bus_write8(nds->bus, IO_SPIDATA, 0xFEu);
    bus_write8(nds->bus, IO_SPIDATA, 0x00u);
    bus_write8(nds->bus, IO_SPIDATA, 0x00u);    /* 读 0x3FE00：version=5 */
    CHECK_EQ("fw mirror0 version lo",
             bus_read8(nds->bus, IO_SPIDATA), 0x05u);
    bus_write8(nds->bus, IO_SPIDATA, 0x00u);    /* 0x3FE01 */
    CHECK_EQ("fw mirror0 version hi",
             bus_read8(nds->bus, IO_SPIDATA), 0x00u);
    bus_write8(nds->bus, IO_SPICNT + 1, 0x81u); /* 本字节传完撤片选 */
    bus_write8(nds->bus, IO_SPIDATA, 0x00u);    /* 0x3FE02：favoriteColor */
    CHECK_EQ("fw mirror0 favorite color",
             bus_read8(nds->bus, IO_SPIDATA), 0x07u);

    /* 新事务：读固件头 0x20 的用户设置偏移（C0 7F = 0x7FC0<<3） */
    bus_write8(nds->bus, IO_SPICNT + 1, 0x89u);
    bus_write8(nds->bus, IO_SPIDATA, 0x03u);
    bus_write8(nds->bus, IO_SPIDATA, 0x00u);
    bus_write8(nds->bus, IO_SPIDATA, 0x00u);
    bus_write8(nds->bus, IO_SPIDATA, 0x20u);
    bus_write8(nds->bus, IO_SPIDATA, 0x00u);
    CHECK_EQ("fw header offset lo",
             bus_read8(nds->bus, IO_SPIDATA), 0xC0u);
    bus_write8(nds->bus, IO_SPICNT + 1, 0x81u);
    bus_write8(nds->bus, IO_SPIDATA, 0x00u);
    CHECK_EQ("fw header offset hi",
             bus_read8(nds->bus, IO_SPIDATA), 0x7Fu);

    /* RDSR：命令后下一字节回状态 0（就绪） */
    bus_write8(nds->bus, IO_SPICNT + 1, 0x89u);
    bus_write8(nds->bus, IO_SPIDATA, 0x05u);
    bus_write8(nds->bus, IO_SPICNT + 1, 0x81u);
    bus_write8(nds->bus, IO_SPIDATA, 0x00u);
    CHECK_EQ("fw rdsr 0x00",
             bus_read8(nds->bus, IO_SPIDATA), 0x00u);

    /* 读 0x3FF70 起的镜像 1 尾部：Update Counter=1、CRC=0xBAFD（小端） */
    bus_write8(nds->bus, IO_SPICNT + 1, 0x89u);
    bus_write8(nds->bus, IO_SPIDATA, 0x03u);
    bus_write8(nds->bus, IO_SPIDATA, 0x03u);
    bus_write8(nds->bus, IO_SPIDATA, 0xFFu);
    bus_write8(nds->bus, IO_SPIDATA, 0x70u);
    bus_write8(nds->bus, IO_SPIDATA, 0x00u);    /* 读 0x3FF70：counter=1 */
    CHECK_EQ("fw mirror1 counter",
             bus_read8(nds->bus, IO_SPIDATA), 0x01u);
    bus_write8(nds->bus, IO_SPIDATA, 0x00u);    /* 读 0x3FF71：保留 0 */
    CHECK_EQ("fw mirror1 reserved",
             bus_read8(nds->bus, IO_SPIDATA), 0x00u);
    bus_write8(nds->bus, IO_SPIDATA, 0x00u);    /* 读 0x3FF72：CRC 低字节 */
    CHECK_EQ("fw mirror1 crc lo",
             bus_read8(nds->bus, IO_SPIDATA), 0xFDu);
    bus_write8(nds->bus, IO_SPICNT + 1, 0x81u);
    bus_write8(nds->bus, IO_SPIDATA, 0x00u);    /* 读 0x3FF73：CRC 高字节 */
    CHECK_EQ("fw mirror1 crc hi",
             bus_read8(nds->bus, IO_SPIDATA), 0xBAu);
    nds->bus->active_is_arm7 = 0;
}

/* ---- 21-B9k 用例：ARM9 硬件除法/开方寄存器（FFXII 启动早期访问 0x04000280-2BF） ---- */
static void test_math_div_sqrt(nds_t *nds)
{
    /* 模式 0（32/32）：100000 / 7 = 14285 余 5 */
    nds->bus->active_is_arm7 = 0;
    bus_write8(nds->bus, MATH_DIVCNT, 0x00u);
    bus_write32(nds->bus, MATH_DIV_NUMER, 100000u);
    bus_write32(nds->bus, MATH_DIV_DENOM, 7u);
    CHECK_EQ("math mode0 quot lo",
             bus_read32(nds->bus, MATH_DIV_RESULT), 14285u);
    CHECK_EQ("math mode0 quot hi",
             bus_read32(nds->bus, MATH_DIV_RESULT + 4), 0u);
    CHECK_EQ("math mode0 rem lo",
             bus_read32(nds->bus, MATH_DIV_REM), 5u);
    CHECK_EQ("math mode0 rem hi",
             bus_read32(nds->bus, MATH_DIV_REM + 4), 0u);
    CHECK_EQ("math mode0 busy bit clear",
             bus_read16(nds->bus, MATH_DIVCNT) & 0x8000u, 0u);

    /* 除零：-5/0 → DIV0 置位；32 位模式商低字=1、高字=-1，余数=-5 */
    bus_write32(nds->bus, MATH_DIV_NUMER, 0xFFFFFFFBu);
    bus_write32(nds->bus, MATH_DIV_DENOM, 0u);
    CHECK_EQ("math div0 flag",
             bus_read16(nds->bus, MATH_DIVCNT) & 0x4000u, 0x4000u);
    CHECK_EQ("math div0 quot lo",
             bus_read32(nds->bus, MATH_DIV_RESULT), 1u);
    CHECK_EQ("math div0 quot hi",
             bus_read32(nds->bus, MATH_DIV_RESULT + 4), 0xFFFFFFFFu);
    CHECK_EQ("math div0 rem lo",
             bus_read32(nds->bus, MATH_DIV_REM), 0xFFFFFFFBu);
    CHECK_EQ("math div0 rem hi",
             bus_read32(nds->bus, MATH_DIV_REM + 4), 0xFFFFFFFFu);

    /* 只读位写不进去：写 busy/div0 后仍保留除零状态 */
    bus_write16(nds->bus, MATH_DIVCNT, 0xC000u);
    CHECK_EQ("math div flags readonly",
             bus_read16(nds->bus, MATH_DIVCNT) & 0xC000u, 0x4000u);

    /* 模式 1（64/32）：0x123456789ABCDEF0 / 16 = 0x0123456789ABCDEF */
    bus_write8(nds->bus, MATH_DIVCNT, 0x01u);
    bus_write32(nds->bus, MATH_DIV_NUMER, 0x9ABCDEF0u);
    bus_write32(nds->bus, MATH_DIV_NUMER + 4, 0x12345678u);
    bus_write32(nds->bus, MATH_DIV_DENOM + 4, 0u);
    bus_write32(nds->bus, MATH_DIV_DENOM, 16u);
    CHECK_EQ("math mode1 quot lo",
             bus_read32(nds->bus, MATH_DIV_RESULT), 0x89ABCDEFu);
    CHECK_EQ("math mode1 quot hi",
             bus_read32(nds->bus, MATH_DIV_RESULT + 4), 0x01234567u);
    CHECK_EQ("math mode1 rem",
             bus_read32(nds->bus, MATH_DIV_REM), 0u);

    /* 模式 2（64/64）：2^32 / 2^32 = 1 */
    bus_write8(nds->bus, MATH_DIVCNT, 0x02u);
    bus_write32(nds->bus, MATH_DIV_NUMER, 0u);
    bus_write32(nds->bus, MATH_DIV_NUMER + 4, 1u);
    bus_write32(nds->bus, MATH_DIV_DENOM + 4, 1u);
    bus_write32(nds->bus, MATH_DIV_DENOM, 0u);
    CHECK_EQ("math mode2 quot lo",
             bus_read32(nds->bus, MATH_DIV_RESULT), 1u);
    CHECK_EQ("math mode2 quot hi",
             bus_read32(nds->bus, MATH_DIV_RESULT + 4), 0u);

    /* 32 位溢出：-0x80000000 / -1 → 商 0x80000000，高字 0 */
    bus_write8(nds->bus, MATH_DIVCNT, 0x00u);
    bus_write32(nds->bus, MATH_DIV_NUMER, 0x80000000u);
    bus_write32(nds->bus, MATH_DIV_NUMER + 4, 0u);
    bus_write32(nds->bus, MATH_DIV_DENOM + 4, 0u);
    bus_write32(nds->bus, MATH_DIV_DENOM, 0xFFFFFFFFu);
    CHECK_EQ("math min/-1 quot lo",
             bus_read32(nds->bus, MATH_DIV_RESULT), 0x80000000u);
    CHECK_EQ("math min/-1 quot hi",
             bus_read32(nds->bus, MATH_DIV_RESULT + 4), 0u);

    /* 开方 32 位输入：sqrt(2500)=50 */
    bus_write8(nds->bus, MATH_SQRTCNT, 0x00u);
    bus_write32(nds->bus, MATH_SQRT_PARAM, 2500u);
    CHECK_EQ("math sqrt32",
             bus_read32(nds->bus, MATH_SQRT_RESULT), 50u);
    CHECK_EQ("math sqrt busy clear",
             bus_read16(nds->bus, MATH_SQRTCNT) & 0x8000u, 0u);

    /* 开方 64 位输入：sqrt(2^32)=65536 */
    bus_write8(nds->bus, MATH_SQRTCNT, 0x01u);
    bus_write32(nds->bus, MATH_SQRT_PARAM, 0u);
    bus_write32(nds->bus, MATH_SQRT_PARAM + 4, 1u);
    CHECK_EQ("math sqrt64",
             bus_read32(nds->bus, MATH_SQRT_RESULT), 65536u);
}

/* ---- 21-B9n 用例：电源/启动标志（POSTFLG/POWCNT1/2/WIFIWAITCNT） ---- */
static void test_power_regs(nds_t *nds)
{
    nds->bus->active_is_arm7 = 0;

    /* 直接启动默认值：两核 POSTFLG=1、ARM9 POWCNT1=0x820F */
    CHECK_EQ("power post9 init",
             bus_read8(nds->bus, IO_POWER_POSTFLG), 0x01u);
    CHECK_EQ("power powcnt1 init",
             bus_read16(nds->bus, IO_POWER_POWCNT), 0x820Fu);

    /* bit0 粘住清不掉；bit1（ARM9 可写）能写 */
    bus_write8(nds->bus, IO_POWER_POSTFLG, 0x00u);
    CHECK_EQ("power post9 sticky",
             bus_read8(nds->bus, IO_POWER_POSTFLG), 0x01u);
    bus_write8(nds->bus, IO_POWER_POSTFLG, 0x02u);
    CHECK_EQ("power post9 bit1",
             bus_read8(nds->bus, IO_POWER_POSTFLG), 0x03u);

    /* POWCNT1 只允许 0x820F 内可写位 */
    bus_write16(nds->bus, IO_POWER_POWCNT, 0xFFFFu);
    CHECK_EQ("power powcnt1 mask",
             bus_read16(nds->bus, IO_POWER_POWCNT), 0x820Fu);
    bus_write16(nds->bus, IO_POWER_POWCNT, 0x0003u);
    CHECK_EQ("power powcnt1 off bits",
             bus_read16(nds->bus, IO_POWER_POWCNT), 0x0003u);

    /* ARM7 侧：POSTFLG bit1 恒 0、POWCNT2 默认喇叭开、WIFIWAITCNT=0x30 */
    nds->bus->active_is_arm7 = 1;
    CHECK_EQ("power post7 init",
             bus_read8(nds->bus, IO_POWER_POSTFLG), 0x01u);
    bus_write8(nds->bus, IO_POWER_POSTFLG, 0x03u);
    CHECK_EQ("power post7 bit1 zero",
             bus_read8(nds->bus, IO_POWER_POSTFLG), 0x01u);
    CHECK_EQ("power powcnt2 init",
             bus_read16(nds->bus, IO_POWER_POWCNT), 0x0001u);
    bus_write16(nds->bus, IO_POWER_POWCNT, 0xFFFFu);
    CHECK_EQ("power powcnt2 mask",
             bus_read16(nds->bus, IO_POWER_POWCNT), 0x0003u);
    CHECK_EQ("power wifiwait init",
             bus_read16(nds->bus, IO_POWER_WIFIWAIT), 0x0030u);
    bus_write8(nds->bus, IO_POWER_WIFIWAIT, 0x21u);
    CHECK_EQ("power wifiwait write",
             bus_read16(nds->bus, IO_POWER_WIFIWAIT), 0x0021u);
    nds->bus->active_is_arm7 = 0;
}

/* ---- 21-B9u 用例：VCOUNT（0x04000006 只读，任务调度时钟） ---- */
static void test_vcount(nds_t *nds)
{
    CHECK_EQ("vcount init 0", bus_read16(nds->bus, 0x04000006u), 0x0000u);
    CHECK_EQ("dispstat init 0", bus_read16(nds->bus, 0x04000004u), 0x0000u);
    io_set_vblank(nds->io);
    CHECK_EQ("vcount frame start 0", bus_read16(nds->bus, 0x04000006u), 0x0000u);
    CHECK_EQ("dispstat vblank bit", bus_read16(nds->bus, 0x04000004u) & 1u, 1u);
    io_advance_scanline(nds->io);
    CHECK_EQ("vcount after scanline 1", bus_read16(nds->bus, 0x04000006u), 0x0001u);
    /* 写 DISPSTAT：IRQ 使能 bit5 + VCount 比较值 1 */
    bus_write16(nds->bus, 0x04000004u, 0x0120u);
    CHECK_EQ("dispstat setting", bus_read16(nds->bus, 0x04000004u) & 0xFF20u, 0x0120u);
    io_advance_scanline(nds->io); /* 扫描线 2，无匹配 */
    CHECK_EQ("vcount read-only", bus_read16(nds->bus, 0x04000006u), 0x0002u);
    /* VCount 匹配应在下一轮从 2 回到? 简化：直接回到扫描线 1 不现实；改为验证
       使能写入后 IF bit2 会在匹配路径置位（匹配线 1 已错过，此处只查寄存器可写）。 */
    CHECK_EQ("dispstat vcount irq enabled", bus_read16(nds->bus, 0x04000004u) & 0x20u, 0x20u);
}

/* ---- 阶段 21-B4 用例：ARM BX 奇地址应切 Thumb ---- */
static void test_arm_bx_thumb(nds_t *nds)
{
    const uint32_t arm_base = BUS_MAIN_RAM_BASE;            /* ARM 驱动代码 */
    const uint32_t thumb_base = BUS_MAIN_RAM_BASE + 0x1000; /* Thumb 目标区 */

    /* Thumb：MOVS r1,#5；B self（停在 thumb_base+2） */
    bus_write16(nds->bus, thumb_base, 0x2105u);
    bus_write16(nds->bus, thumb_base + 2, 0xE7FEu);

    /* ARM：PC 相对载入 thumb_base → r0|=1 → BX r0。
       LDR 偏移按 PC+8 基准：0x02000000 + 8 + 8 = 字面量 0x02000010。 */
    static const uint32_t prog[] = {
        0xE59F0008u, /* LDR r0, [pc, #8]  r0 = 0x02001000 */
        0xE3800001u, /* ORR r0, r0, #1    r0 = 0x02001001（LSB=1） */
        0xE12FFF10u, /* BX r0             应切 Thumb */
        0x00000000u, /* 占位 */
        0x02001000u, /* 字面量：thumb_base */
    };

    for (size_t i = 0; i < sizeof prog / sizeof prog[0]; i++)
        bus_write32(nds->bus, arm_base + 4 * i, prog[i]);
    cpu_reset(nds->cpu, arm_base);
    exec_set_trace(0);
    thumb_set_trace(0); /* 本用例内部会切 Thumb，关逐条打印避免噪音 */
    int steps = 0;
    while (steps++ < 12 && nds->cpu->r[15] != thumb_base + 2)
        cpu_step(nds->cpu);
    thumb_set_trace(1);
    exec_set_trace(1);

    CHECK_EQ("arm bx odd T=1", nds->cpu->cpsr & CPSR_T, CPSR_T);
    CHECK_EQ("arm bx thumb r1", nds->cpu->r[1], 5u);
    CHECK_EQ("arm bx thumb PC", nds->cpu->r[15], thumb_base + 2);
}

/* ---- 8.4 用例：交错调度 2:1 ---- */
static void test_interleave(nds_t *nds)
{
    bus_write32(nds->bus, 0x02000800u, 0xE1A00000u); /* NOP */
    bus_write32(nds->bus, 0x03800000u, 0xE1A00000u); /* NOP */
    cpu_reset(nds->cpu,  0x02000800u);
    cpu_reset(nds->cpu7, 0x03800000u);

    /* 跑 6 步，i%3==2 时跑 ARM7，其余 ARM9：ARM9 4 步、ARM7 2 步 */
    for (int i = 0; i < 6; i++) {
        if (i % 3 == 2)
            cpu_step(nds->cpu7);
        else
            cpu_step(nds->cpu);
    }
    CHECK_EQ("interleave arm9 cycles", (uint32_t)nds->cpu->cycles, 4u);
    CHECK_EQ("interleave arm7 cycles", (uint32_t)nds->cpu7->cycles, 2u);
}

/* ---- 8.5 用例：中断寄存器按 CPU 分流 ---- */
static void test_irq_split(nds_t *nds)
{
    /* ARM9 写 IME/IE，ARM7 应读到独立（清零）的值 */
    nds->bus->active_is_arm7 = 0;
    bus_write32(nds->bus, IO_IME_ADDR, 0x00000001u);
    bus_write32(nds->bus, IO_IE_ADDR,  IO_IF_VBLANK);

    nds->bus->active_is_arm7 = 1;
    CHECK_EQ("arm7 IME independent", bus_read32(nds->bus, IO_IME_ADDR), 0x00000000u);
    CHECK_EQ("arm7 IE independent",  bus_read32(nds->bus, IO_IE_ADDR),  0x00000000u);

    /* ARM9 视角读回应得自己写过的值 */
    nds->bus->active_is_arm7 = 0;
    CHECK_EQ("arm9 IME", bus_read32(nds->bus, IO_IME_ADDR), 0x00000001u);
    CHECK_EQ("arm9 IE",  bus_read32(nds->bus, IO_IE_ADDR),  IO_IF_VBLANK);
    nds->bus->active_is_arm7 = 0; /* 恢复默认 */
}

/* ---- 8.6 用例：FIFO 收发 + 状态位 ---- */
static void test_fifo_basic(nds_t *nds)
{
    /* 使能两核 FIFO */
    nds->bus->active_is_arm7 = 0;
    bus_write16(nds->bus, IO_FIFO_CNT, FIFO_CNT_ENABLE);
    nds->bus->active_is_arm7 = 1;
    bus_write16(nds->bus, IO_FIFO_CNT, FIFO_CNT_ENABLE);
    nds->bus->active_is_arm7 = 0;

    /* 空队列：ARM9 视角 send/recv 都应空 */
    uint16_t c = bus_read16(nds->bus, IO_FIFO_CNT);
    CHECK_EQ("fifo arm9 send empty", c & FIFO_CNT_SEND_EMPTY, FIFO_CNT_SEND_EMPTY);
    CHECK_EQ("fifo arm9 recv empty", c & FIFO_CNT_RECV_EMPTY, FIFO_CNT_RECV_EMPTY);

    /* ARM9 发一个字 → ARM9 的 send 非空；ARM7 的 recv 非空 */
    bus_write32(nds->bus, IO_FIFO_SEND, 0xDEADBEEFu);
    c = bus_read16(nds->bus, IO_FIFO_CNT);
    CHECK_EQ("fifo arm9 send not empty", c & FIFO_CNT_SEND_EMPTY, 0u);

    nds->bus->active_is_arm7 = 1;
    c = bus_read16(nds->bus, IO_FIFO_CNT);
    CHECK_EQ("fifo arm7 recv not empty", c & FIFO_CNT_RECV_EMPTY, 0u);
    /* ARM7 收，应得原值 */
    uint32_t v = bus_read32(nds->bus, IO_FIFO_RECV);
    CHECK_EQ("fifo arm7 recv value", v, 0xDEADBEEFu);
    nds->bus->active_is_arm7 = 0;

    /* 反向：ARM7 发 → ARM9 收 */
    nds->bus->active_is_arm7 = 1;
    bus_write32(nds->bus, IO_FIFO_SEND, 0x11223344u);
    nds->bus->active_is_arm7 = 0;
    v = bus_read32(nds->bus, IO_FIFO_RECV);
    CHECK_EQ("fifo arm9 recv value", v, 0x11223344u);
}

/* ---- 8.6 用例：FIFO 中断 IF17/18 ---- */
static void test_fifo_irq(nds_t *nds)
{
    /* 清 ARM9 的 IF，再使能 send-empty IRQ：空队列 + IRQ 使能 → 边沿 0→1 → IF17 置位 */
    nds->bus->active_is_arm7 = 0;
    bus_write32(nds->bus, IO_IF_ADDR, 0xFFFFFFFFu); /* 写 1 清全部 IF */
    bus_write16(nds->bus, IO_FIFO_CNT, FIFO_CNT_ENABLE | FIFO_CNT_SEND_IRQ);
    CHECK_EQ("fifo send-empty IF17", bus_read32(nds->bus, IO_IF_ADDR) & IO_IF_FIFO_SEND_EMPTY,
             IO_IF_FIFO_SEND_EMPTY);

    /* 使能 recv-not-empty IRQ（初始 recv 空，不触发），再由 ARM7 发一个字
       → ARM9 的 recv 变非空 → IF18 置位 */
    bus_write32(nds->bus, IO_IF_ADDR, 0xFFFFFFFFu); /* 再清 IF */
    bus_write16(nds->bus, IO_FIFO_CNT, FIFO_CNT_ENABLE | FIFO_CNT_RECV_IRQ);
    CHECK_EQ("fifo recv empty no IF18", bus_read32(nds->bus, IO_IF_ADDR) & IO_IF_FIFO_RECV_NOT_EMPTY, 0u);

    nds->bus->active_is_arm7 = 1;
    bus_write32(nds->bus, IO_FIFO_SEND, 0x0000ABCDu); /* ARM7 发 → ARM9 收非空 */
    nds->bus->active_is_arm7 = 0;
    CHECK_EQ("fifo recv-not-empty IF18", bus_read32(nds->bus, IO_IF_ADDR) & IO_IF_FIFO_RECV_NOT_EMPTY,
             IO_IF_FIFO_RECV_NOT_EMPTY);
}

/* ---- 8.7 用例：双核经 FIFO 传值 + 写底屏 ----
   ARM7 程序发 0x7C00（红）到 SEND；ARM9 程序读 RECV 后写到底屏 VRAM。 */
static void test_fifo_program(nds_t *nds)
{
    const uint32_t arm9_base = BUS_MAIN_RAM_BASE;
    const uint32_t arm7_base = BUS_ARM7_WRAM_BASE;

    /* 数据区（Main RAM 0x02001000）：预写 RECV/SEND 地址，避免复杂立即数 */
    bus_write32(nds->bus, 0x02001000u, IO_FIFO_RECV);   /* ARM9 加载 RECV 地址用 */
    bus_write32(nds->bus, 0x02001004u, IO_FIFO_SEND);   /* ARM7 加载 SEND 地址用 */

    /* ARM7 程序：发 0x7C00 到 SEND */
    static const uint32_t prog7[] = {
        /* 0x00 */ 0xE3A00C7C, /* MOV r0, #0x7C00        r0 = 要发的值（红） */
        /* 0x04 */ 0xE3A01402, /* MOV r1, #0x02000000    r1 = Main RAM 基址 */
        /* 0x08 */ 0xE2811A01, /* ADD r1, r1, #0x1000    r1 = 0x02001000 */
        /* 0x0C */ 0xE5911004, /* LDR r1, [r1, #4]       r1 = SEND 地址 */
        /* 0x10 */ 0xE5810000, /* STR r0, [r1]           发送 */
        /* 0x14 */ 0xEAFFFFFE, /* B self（停机） */
    };

    /* ARM9 程序：读 RECV → 写底屏 */
    static const uint32_t prog9[] = {
        /* 0x00 */ 0xE3A00402, /* MOV r0, #0x02000000    r0 = Main RAM 基址 */
        /* 0x04 */ 0xE2800A01, /* ADD r0, r0, #0x1000    r0 = 0x02001000 */
        /* 0x08 */ 0xE5900000, /* LDR r0, [r0]           r0 = RECV 地址 */
        /* 0x0C */ 0xE5901000, /* LDR r1, [r0]           r1 = 收到的值 */
        /* 0x10 */ 0xE3A02406, /* MOV r2, #0x06000000    r2 = VRAM 基址 */
        /* 0x14 */ 0xE2822A18, /* ADD r2, r2, #0x18000   r2 = 底屏起点 */
        /* 0x18 */ 0xE5821000, /* STR r1, [r2]           写底屏 */
        /* 0x1C */ 0xEAFFFFFE, /* B self（停机） */
    };

    /* 使能两核 FIFO */
    nds->bus->active_is_arm7 = 0;
    bus_write16(nds->bus, IO_FIFO_CNT, FIFO_CNT_ENABLE);
    nds->bus->active_is_arm7 = 1;
    bus_write16(nds->bus, IO_FIFO_CNT, FIFO_CNT_ENABLE);
    nds->bus->active_is_arm7 = 0;

    /* 先跑 ARM7（发送），再跑 ARM9（接收写屏） */
    run_cpu7(nds, arm7_base, prog7, sizeof prog7 / sizeof prog7[0],
             arm7_base, arm7_base + 0x14, 32);
    run_program(nds, arm9_base, prog9, sizeof prog9 / sizeof prog9[0],
                arm9_base, arm9_base + 0x1C, 32);

    CHECK_EQ("fifo prog arm7 halt", nds->cpu7->r[15], arm7_base + 0x14);
    CHECK_EQ("fifo prog arm9 halt", nds->cpu->r[15],  arm9_base + 0x1C);

    /* 底屏第一个像素应为收到的红色 0x7C00 */
    CHECK_EQ("fifo prog bottom px", bus_read16(nds->bus, BUS_VRAM_BASE + TEST_VRAM_BOTTOM_OFFSET),
             0x7C00u);
}

/* 9.2：显示控制寄存器（DISPCNT 主/副、BGxCNT、滚动）与调色板 RAM 读写 */
static void test_disp_regs(nds_t *nds)
{
    bus_write32(nds->bus, IO_DISPCNT, 0x00010405u);
    CHECK_EQ("disp DISPCNT", bus_read32(nds->bus, IO_DISPCNT), 0x00010405u);
    bus_write16(nds->bus, IO_BGCNT_BASE, 0x1F0Au);
    CHECK_EQ("disp BG0CNT", bus_read16(nds->bus, IO_BGCNT_BASE), 0x1F0Au);
    bus_write16(nds->bus, IO_BGCNT_BASE + 2, 0x0010u);
    CHECK_EQ("disp BG1CNT", bus_read16(nds->bus, IO_BGCNT_BASE + 2), 0x0010u);
    /* 滚动：BG0HOFS/VOFS */
    bus_write16(nds->bus, IO_BG_SCROLL_BASE, 0x1234u);
    bus_write16(nds->bus, IO_BG_SCROLL_BASE + 2, 0x5678u);
    CHECK_EQ("disp BG0HOFS", bus_read16(nds->bus, IO_BG_SCROLL_BASE), 0x1234u);
    CHECK_EQ("disp BG0VOFS", bus_read16(nds->bus, IO_BG_SCROLL_BASE + 2), 0x5678u);
    /* 副引擎 */
    bus_write32(nds->bus, IO_DISPCNT_SUB, 0x00000003u);
    bus_write16(nds->bus, IO_BGCNT_SUB_BASE + 4, 0x0101u); /* 副 BG2CNT */
    CHECK_EQ("disp DISPCNT_SUB", bus_read32(nds->bus, IO_DISPCNT_SUB), 0x00000003u);
    CHECK_EQ("disp BG2CNT_SUB", bus_read16(nds->bus, IO_BGCNT_SUB_BASE + 4), 0x0101u);
    /* 调色板 RAM：主 BG 0x05000000，副 BG 0x05000400 */
    bus_write16(nds->bus, BUS_PALETTE_BASE, 0x7C00u);
    CHECK_EQ("pal entry 0", bus_read16(nds->bus, BUS_PALETTE_BASE), 0x7C00u);
    bus_write16(nds->bus, BUS_PALETTE_BASE + 0x400, 0x03E0u);
    CHECK_EQ("pal sub entry", bus_read16(nds->bus, BUS_PALETTE_BASE + 0x400), 0x03E0u);
    /* VRAM 窗口：副 BG 0x06200000 由默认映射落到 bank C；
       物理 bank 不直接暴露给总线，写入后应能从同一逻辑窗口读回。 */
    bus_write16(nds->bus, BUS_VRAM_SUB_BG_BASE, 0x001Fu);
    CHECK_EQ("vram sub bg win", bus_read16(nds->bus, BUS_VRAM_SUB_BG_BASE), 0x001Fu);
}

/* 21-B9wd：VRAMCNT 动态映射（FFXII 配置：D→A BG、C→B BG、E→A OBJ、H→B OBJ） */
static void test_vramcnt_mapping(nds_t *nds)
{
    bus_t *bus = nds->bus;
    nds->bus->active_is_arm7 = 0;

    for (int b = 0; b < 9; b++)
        bus_set_vramcnt(bus, b, 0x00u);      /* 先清掉全部映射 */
    bus_set_vramcnt(bus, 3, 0x81u);          /* D → Engine A BG */
    bus_set_vramcnt(bus, 2, 0x84u);          /* C → Engine B BG */
    bus_set_vramcnt(bus, 4, 0x82u);          /* E → Engine A OBJ */
    bus_set_vramcnt(bus, 7, 0x82u);          /* H → Engine B OBJ */

    bus_write16(bus, 0x06000000u, 0xABCDu);
    CHECK_EQ("vramcnt D->A BG read", bus_read16(bus, 0x06000000u), 0xABCDu);
    CHECK_EQ("vramcnt D phys", bus_read16(bus, 0x06000000u + 0x1FFFEu), 0x0000u);
    bus_write16(bus, 0x06200000u + 0x100u, 0x1234u);
    CHECK_EQ("vramcnt C->B BG read", bus_read16(bus, 0x06200000u + 0x100u), 0x1234u);
    bus_write16(bus, 0x06400000u, 0x1111u);
    CHECK_EQ("vramcnt E->A OBJ read", bus_read16(bus, 0x06400000u), 0x1111u);
    bus_write16(bus, 0x06600000u + 0x200u, 0x2222u);
    CHECK_EQ("vramcnt H->B OBJ read", bus_read16(bus, 0x06600000u + 0x200u), 0x2222u);

    bus_write8(bus, 0x04000243u, 0x81u);     /* 经 IO 写 VRAMCNT D */
    CHECK_EQ("vramcnt io read", bus_read8(bus, 0x04000243u), 0x81u);

    /* LCDC 窗口（21-B9we）：0x0689C000 是 bank H 的第 2 个 8KB 槽，
       H=0x80(LCDC) 时写入，H=0x82(BBG 扩展调色板) 后由渲染器读同一物理数据。 */
    bus_set_vramcnt(bus, 7, 0x80u);
    bus_write16(bus, 0x0689C000u, 0x7C1Fu);
    CHECK_EQ("lcdc H bank window", bus_read16(bus, 0x0689C000u), 0x7C1Fu);
    bus_set_vramcnt(bus, 7, 0x82u);
    CHECK_EQ("bbg ext pal read", bus_vram_extpal16(bus, 1, 2, 0, 0), 0x7C1Fu);

    bus_vram_reset_default(bus);             /* 恢复默认，避免污染后续渲染用例 */
}

/* 9.3：直色位图模式渲染（DISPCNT mode 5 + BG2 直色位图） */
static void test_bitmap_render(nds_t *nds)
{
    uint32_t fb_top[RENDER_SCREEN_W * RENDER_SCREEN_H];
    uint32_t fb_bot[RENDER_SCREEN_W * RENDER_SCREEN_H];

    /* mode 5 | BG2 开启 | 显示模式 1（正常） */
    bus_write32(nds->bus, IO_DISPCNT, 5u | DISPCNT_BG2 | (1u << DISPCNT_DISPLAY_MODE_SHIFT));
    /* BG2CNT：位图(bit7) + 直色(bit2) + 256×256(bit14)，位图基址块 0 */
    bus_write16(nds->bus, IO_BGCNT_BASE + 2 * 2, BGCNT_COLORS_256 | BGCNT_DIRECT_COLOR | (1u << 14));
    /* 位图在 0x06000000：写前两个像素 红/蓝 */
    bus_write16(nds->bus, BUS_VRAM_BASE, 0x7C00u);
    bus_write16(nds->bus, BUS_VRAM_BASE + 2, 0x001Fu);

    render_frame(nds->bus, fb_top, fb_bot);

    CHECK_EQ("bitmap px0 red",  fb_top[0], 0xFFFB0000u);
    CHECK_EQ("bitmap px1 blue", fb_top[1], 0xFF0000FBu);
    CHECK_EQ("bitmap px256 next row", fb_top[256], 0xFF000000u); /* 未写区域=0 → 黑色 */
}

/* 9.4：Mode 0 tile 图层（4bpp/8bpp + tilemap + 调色板 + 透明背景） */
static void test_tile_render(nds_t *nds)
{
    uint32_t fb_top[RENDER_SCREEN_W * RENDER_SCREEN_H];
    uint32_t fb_bot[RENDER_SCREEN_W * RENDER_SCREEN_H];

    /* 调色板：0=绿(背景) 1=红 5=红 */
    bus_write16(nds->bus, BUS_PALETTE_BASE + 0, 0x03E0u);
    bus_write16(nds->bus, BUS_PALETTE_BASE + 2, 0x7C00u);
    bus_write16(nds->bus, BUS_PALETTE_BASE + 10, 0x7C00u);

    /* mode 0 + BG0 开 + 显示模式 1 */
    bus_write32(nds->bus, IO_DISPCNT, DISPCNT_BG0 | (1u << DISPCNT_DISPLAY_MODE_SHIFT));
    /* BG0CNT：屏幕基址块 1(=0x800)，字符基址块 0，16 色 */
    bus_write16(nds->bus, IO_BGCNT_BASE, 0x0100u);
    /* tile0（4bpp）：只让像素(0,0)=索引1，其余索引0（透明） */
    bus_write8(nds->bus, BUS_VRAM_BASE, 0x80u);

    render_frame(nds->bus, fb_top, fb_bot);
    CHECK_EQ("tile4 px00 red",      fb_top[0],   0xFFFB0000u);
    CHECK_EQ("tile4 px10 backdrop", fb_top[1],   0xFF00FB00u);
    CHECK_EQ("tile4 row1 backdrop", fb_top[256], 0xFF00FB00u);

    /* 切 256 色（bit7=1），tile0(8bpp) 只让像素(0,0)=索引5 */
    bus_write16(nds->bus, IO_BGCNT_BASE, 0x0180u);
    bus_write8(nds->bus, BUS_VRAM_BASE, 5u);

    render_frame(nds->bus, fb_top, fb_bot);
    CHECK_EQ("tile8 px00 red",      fb_top[0],   0xFFFB0000u);
    CHECK_EQ("tile8 px10 backdrop", fb_top[1],   0xFF00FB00u);
}

/* 9.5：副引擎（Engine B）对称渲染——DISPCNT_SUB + 副 BGxCNT + 副调色板 + 副 VRAM */
static void test_engine_b(nds_t *nds)
{
    uint32_t fb_top[RENDER_SCREEN_W * RENDER_SCREEN_H];
    uint32_t fb_bot[RENDER_SCREEN_W * RENDER_SCREEN_H];

    /* 副引擎直色位图：mode 5 + BG2 + 显示模式 1，位图在 0x06200000（物理 vram[0x40000]） */
    bus_write32(nds->bus, IO_DISPCNT_SUB, 5u | DISPCNT_BG2 | (1u << DISPCNT_DISPLAY_MODE_SHIFT));
    bus_write16(nds->bus, IO_BGCNT_SUB_BASE + 4, BGCNT_COLORS_256 | BGCNT_DIRECT_COLOR | (1u << 14));
    bus_write16(nds->bus, BUS_VRAM_SUB_BG_BASE, 0x7C00u);
    render_frame(nds->bus, fb_top, fb_bot);
    CHECK_EQ("engB bmp red", fb_bot[0], 0xFFFB0000u);

    /* 副引擎 tile：副调色板 0x05000400（0=红背景, 1=绿） */
    bus_write16(nds->bus, BUS_PALETTE_BASE + 0x400, 0x7C00u);
    bus_write16(nds->bus, BUS_PALETTE_BASE + 0x402, 0x03E0u);
    bus_write32(nds->bus, IO_DISPCNT_SUB, DISPCNT_BG0 | (1u << DISPCNT_DISPLAY_MODE_SHIFT));
    bus_write16(nds->bus, IO_BGCNT_SUB_BASE, 0x0100u);
    /* tile0（4bpp）：清空位面1-3，仅位面0 让像素(0,0)=索引1 */
    bus_write8(nds->bus, BUS_VRAM_SUB_BG_BASE + 0, 0x80u);
    bus_write8(nds->bus, BUS_VRAM_SUB_BG_BASE + 1, 0x00u);
    bus_write8(nds->bus, BUS_VRAM_SUB_BG_BASE + 2, 0x00u);
    bus_write8(nds->bus, BUS_VRAM_SUB_BG_BASE + 3, 0x00u);
    render_frame(nds->bus, fb_top, fb_bot);
    CHECK_EQ("engB tile green",    fb_bot[0], 0xFF00FB00u); /* 索引1 → 绿 */
    CHECK_EQ("engB tile backdrop", fb_bot[1], 0xFFFB0000u); /* 透明 → 红背景 */
}

/* 9.6：OBJ 最小（一个 sprite：OAM 读取 + 1D tile 映射 + OBJ 调色板） */
static void test_obj_render(nds_t *nds)
{
    uint32_t fb_top[RENDER_SCREEN_W * RENDER_SCREEN_H];
    uint32_t fb_bot[RENDER_SCREEN_W * RENDER_SCREEN_H];

    /* 主 BG 调色板[0]=黑（作为背景/透明露出色） */
    bus_write16(nds->bus, BUS_PALETTE_BASE, 0x0000u);
    /* 主 OBJ 调色板（0x05000200）：16 色 pal_slot0，索引1=红 */
    bus_write16(nds->bus, BUS_PALETTE_BASE + 0x200 + 2, 0x7C00u);

    /* DISPCNT：mode 0（无 BG）+ OBJ 开启 + 显示模式 1 */
    bus_write32(nds->bus, IO_DISPCNT, DISPCNT_OBJ | (1u << DISPCNT_DISPLAY_MODE_SHIFT));

    /* OBJ 图形（0x06400000 主 OBJ 窗口）tile0（4bpp）：像素(0,0)=索引1，其余 0（透明） */
    bus_write8(nds->bus, BUS_VRAM_MAIN_OBJ_BASE + 0, 0x80u);
    bus_write8(nds->bus, BUS_VRAM_MAIN_OBJ_BASE + 1, 0x00u);
    bus_write8(nds->bus, BUS_VRAM_MAIN_OBJ_BASE + 2, 0x00u);
    bus_write8(nds->bus, BUS_VRAM_MAIN_OBJ_BASE + 3, 0x00u);

    /* OAM 默认全 0 = 一个「8×8 方块在 (0,0)」的可见 sprite（真机行为），
       先统一把所有条目 bit9 置 1（禁用），再单独配置 entry 0。 */
    for (int n = 0; n < 128; n++)
        bus_write16(nds->bus, BUS_OAM_BASE + 8u * n, 0x0200u);

    /* OAM[0]：属性0 Y=10/方形/16色；属性1 X=20/尺寸8×8；属性2 tile0/调色板0 */
    bus_write16(nds->bus, BUS_OAM_BASE + 0, 0x000Au);
    bus_write16(nds->bus, BUS_OAM_BASE + 2, 0x0014u);
    bus_write16(nds->bus, BUS_OAM_BASE + 4, 0x0000u);

    render_frame(nds->bus, fb_top, fb_bot);

    CHECK_EQ("obj px at (20,10) red", fb_top[10 * RENDER_SCREEN_W + 20], 0xFFFB0000u);
    CHECK_EQ("obj px transparent",     fb_top[10 * RENDER_SCREEN_W + 21], 0xFF000000u);
    CHECK_EQ("obj px outside",         fb_top[0], 0xFF000000u);

    /* 切 256 色 OBJ：a0 bit13=1，OBJ 图形 8bpp 像素(0,0)=索引5，OBJ 调色板[5]=绿 */
    bus_write16(nds->bus, BUS_PALETTE_BASE + 0x200 + 10, 0x03E0u);
    bus_write16(nds->bus, BUS_OAM_BASE + 0, 0x200Au); /* bit13=256色 */
    bus_write8(nds->bus, BUS_VRAM_MAIN_OBJ_BASE + 0, 5u);
    bus_write8(nds->bus, BUS_VRAM_MAIN_OBJ_BASE + 1, 0x00u);

    render_frame(nds->bus, fb_top, fb_bot);
    CHECK_EQ("obj 256c px green", fb_top[10 * RENDER_SCREEN_W + 20], 0xFF00FB00u);
}

/* 9.7：自造数据 2D 场景验收（不依赖旧「VRAM=屏幕 framebuffer」约定）。
   顶屏：8bpp tile + tilemap + 调色板 拼出「红 tile0 + 绿 tile1 + 透明底」，
   再叠一个 OBJ 蓝色方块；底屏：副引擎 8bpp tile 填纯青。
   整条链路只经 DISPCNT/BGxCNT/OAM 寄存器，无任何线性 FB 写入。 */
static void test_2d_scene(nds_t *nds)
{
    uint32_t fb_top[RENDER_SCREEN_W * RENDER_SCREEN_H];
    uint32_t fb_bot[RENDER_SCREEN_W * RENDER_SCREEN_H];

    /* 调色板：主 0=黑 1=红 2=绿；副 0=黑 1=青；OBJ 1=蓝 */
    bus_write16(nds->bus, BUS_PALETTE_BASE + 2,      0x7C00u);
    bus_write16(nds->bus, BUS_PALETTE_BASE + 4,      0x03E0u);
    bus_write16(nds->bus, BUS_PALETTE_BASE + 0x402,  0x03FFu);
    bus_write16(nds->bus, BUS_PALETTE_BASE + 0x202,  0x001Fu);

    /* 主 BG 字符块0：tile0(8bpp)=全红(索引1), tile1=全绿(索引2)；tile2 保持全 0=透明 */
    for (int i = 0; i < 64; i++) bus_write8(nds->bus, BUS_VRAM_BASE + 0u + i, 1u);
    for (int i = 0; i < 64; i++) bus_write8(nds->bus, BUS_VRAM_BASE + 64u + i, 2u);

    /* 主 tilemap（屏幕块1）：默认全 tile2（透明），再指定 map[0]=tile0、map[1]=tile1 */
    for (int ty = 0; ty < 32; ty++)
        for (int tx = 0; tx < 32; tx++)
            bus_write16(nds->bus, BUS_VRAM_BASE + 0x800u + (uint32_t)(ty * 32 + tx) * 2u, 2u);
    bus_write16(nds->bus, BUS_VRAM_BASE + 0x800u, 0u);
    bus_write16(nds->bus, BUS_VRAM_BASE + 0x802u, 1u);
    bus_write16(nds->bus, IO_BGCNT_BASE,
                BGCNT_COLORS_256 | (1u << BGCNT_SCREEN_BASE_SHIFT));

    /* OBJ：蓝色 8×8 方块 @ (16,16)，16 色，tile0 全索引1 */
    for (int r = 0; r < 8; r++) {
        bus_write8(nds->bus, BUS_VRAM_MAIN_OBJ_BASE + 4u * r + 0u, 0xFFu);
        bus_write8(nds->bus, BUS_VRAM_MAIN_OBJ_BASE + 4u * r + 1u, 0x00u);
        bus_write8(nds->bus, BUS_VRAM_MAIN_OBJ_BASE + 4u * r + 2u, 0x00u);
        bus_write8(nds->bus, BUS_VRAM_MAIN_OBJ_BASE + 4u * r + 3u, 0x00u);
    }
    for (int n = 0; n < 128; n++)
        bus_write16(nds->bus, BUS_OAM_BASE + 8u * n, 0x0200u);
    bus_write16(nds->bus, BUS_OAM_BASE + 0u, 16u);
    bus_write16(nds->bus, BUS_OAM_BASE + 2u, 16u);
    bus_write16(nds->bus, BUS_OAM_BASE + 4u, 0u);
    bus_write32(nds->bus, IO_DISPCNT,
                DISPCNT_BG0 | DISPCNT_OBJ | (1u << DISPCNT_DISPLAY_MODE_SHIFT));

    /* 副引擎：tile0(8bpp)=全青(索引1)，tilemap 全 0 → 纯青底屏 */
    for (int i = 0; i < 64; i++)
        bus_write8(nds->bus, BUS_VRAM_SUB_BG_BASE + i, 1u);
    for (int ty = 0; ty < 32; ty++)
        for (int tx = 0; tx < 32; tx++)
            bus_write16(nds->bus, BUS_VRAM_SUB_BG_BASE + 0x800u + (uint32_t)(ty * 32 + tx) * 2u, 0u);
    bus_write16(nds->bus, IO_BGCNT_SUB_BASE,
                BGCNT_COLORS_256 | (1u << BGCNT_SCREEN_BASE_SHIFT));
    bus_write32(nds->bus, IO_DISPCNT_SUB,
                DISPCNT_BG0 | (1u << DISPCNT_DISPLAY_MODE_SHIFT));

    render_frame(nds->bus, fb_top, fb_bot);

    CHECK_EQ("scene top tile0 red",    fb_top[0],              0xFFFB0000u);
    CHECK_EQ("scene top tile1 green",  fb_top[8],              0xFF00FB00u);
    CHECK_EQ("scene top transparent",  fb_top[16],             0xFF000000u); /* map[2]=透明→黑背景 */
    CHECK_EQ("scene obj blue",         fb_top[16 * 256 + 16],  0xFF0000FBu);
    CHECK_EQ("scene bot cyan",         fb_bot[0],              0xFF00FBFBu);
}

/* ---- 10.2 用例：移位操作数 LSL/LSR/ASR/ROR（立即数移位 + 寄存器移位） ---- */
static void test_shifts(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE;
    static const uint32_t prog[] = {
        /* 0x00 */ 0xE3A01010, /* MOV r1, #0x10        r1 = 16 */
        /* 0x04 */ 0xE3A02003, /* MOV r2, #3           r2 = 3 */
        /* 0x08 */ 0xE0810182, /* ADD r0, r1, r2, LSL #3  r0 = 16 + (3<<3) = 40 */
        /* 0x0C */ 0xE3A03080, /* MOV r3, #0x80        r3 = 0x80 */
        /* 0x10 */ 0xE1A04223, /* MOV r4, r3, LSR #4   r4 = 0x08 */
        /* 0x14 */ 0xE3A05480, /* MOV r5, #0x80000000  r5 = 0x80000000 */
        /* 0x18 */ 0xE1A06245, /* MOV r6, r5, ASR #4   r6 = 0xF8000000 */
        /* 0x1C */ 0xE3A07001, /* MOV r7, #1           r7 = 1 */
        /* 0x20 */ 0xE1A080E7, /* MOV r8, r7, ROR #1   r8 = 0x80000000 */
        /* 0x24 */ 0xE3A09004, /* MOV r9, #4           r9 = 4 */
        /* 0x28 */ 0xE081A912, /* ADD r10, r1, r2, LSL r9  r10 = 16 + (3<<4) = 64 */
        /* 0x2C */ 0xEAFFFFFE, /* B self */
    };
    run_program(nds, base, prog, sizeof prog / sizeof prog[0],
                base, base + 0x2C, 32);

    CHECK_EQ("shift ADD LSL#3",   nds->cpu->r[0],  40u);
    CHECK_EQ("shift MOV LSR#4",   nds->cpu->r[4],  0x08u);
    CHECK_EQ("shift MOV ASR#4",   nds->cpu->r[6],  0xF8000000u);
    CHECK_EQ("shift MOV ROR#1",   nds->cpu->r[8],  0x80000000u);
    CHECK_EQ("shift ADD LSL reg", nds->cpu->r[10], 64u);
}

/* ---- 10.3 用例：更多数据处理 MVN/BIC/ADC/SBC/RSB/RSC/TST/TEQ/CMN ---- */
static void test_more_dataop(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE;
    static const uint32_t prog[] = {
        /* 0x00 */ 0xE3A0000F, /* MOV r0, #0x0F         r0 = 0x0F */
        /* 0x04 */ 0xE1E01000, /* MVN r1, r0            r1 = 0xFFFFFFF0 */
        /* 0x08 */ 0xE3A020FF, /* MOV r2, #0xFF         r2 = 0xFF */
        /* 0x0C */ 0xE3C2300F, /* BIC r3, r2, #0x0F     r3 = 0xF0 */
        /* 0x10 */ 0xE3E04000, /* MVN r4, #0            r4 = 0xFFFFFFFF */
        /* 0x14 */ 0xE3A05001, /* MOV r5, #1            r5 = 1 */
        /* 0x18 */ 0xE0946005, /* ADDS r6, r4, r5       r6 = 0, C=1 */
        /* 0x1C */ 0xE0A47005, /* ADC r7, r4, r5        r7 = 1 (r4+r5+C) */
        /* 0x20 */ 0xE0C48005, /* SBC r8, r4, r5        r8 = 0xFFFFFFFE (C=1) */
        /* 0x24 */ 0xE3A00002, /* MOV r0, #2            r0 = 2 */
        /* 0x28 */ 0xE260900A, /* RSB r9, r0, #10       r9 = 8 */
        /* 0x2C */ 0xE1500000, /* CMP r0, r0            C=1, Z=1 */
        /* 0x30 */ 0xE2E0A00A, /* RSC r10, r0, #10      r10 = 10-2-0 = 8 (C=1) */
        /* 0x34 */ 0xE3A0000F, /* MOV r0, #0x0F         r0 = 0x0F */
        /* 0x38 */ 0xE31000F0, /* TST r0, #0xF0         Z=1 */
        /* 0x3C */ 0xE330000F, /* TEQ r0, #0x0F         Z=1 */
        /* 0x40 */ 0xE3E00004, /* MVN r0, #4            r0 = 0xFFFFFFFB (-5) */
        /* 0x44 */ 0xE3700005, /* CMN r0, #5            Z=1 */
        /* 0x48 */ 0xEAFFFFFE, /* B self */
    };
    run_program(nds, base, prog, sizeof prog / sizeof prog[0],
                base, base + 0x48, 64);

    CHECK_EQ("dop MVN",  nds->cpu->r[1],  0xFFFFFFF0u);
    CHECK_EQ("dop BIC",  nds->cpu->r[3],  0x000000F0u);
    CHECK_EQ("dop ADDS", nds->cpu->r[6],  0x00000000u);
    CHECK_EQ("dop ADC",  nds->cpu->r[7],  0x00000001u);
    CHECK_EQ("dop SBC",  nds->cpu->r[8],  0xFFFFFFFEu);
    CHECK_EQ("dop RSB",  nds->cpu->r[9],  0x00000008u);
    CHECK_EQ("dop RSC",  nds->cpu->r[10], 0x00000008u);

    /* TST/TEQ/CMN 都置 Z，最终 CMN r0,#5 结果 0 → Z=1 */
    CHECK_EQ("dop Z flag set", (nds->cpu->cpsr & CPSR_Z) ? 1u : 0u, 1u);
}

/* ---- 10.4/12.3 用例：MRS/MSR 读/写 CPSR/SPSR（含模式切换 + spsr[5] 分槽） ---- */
static void test_mrs_msr(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE;
    arm_cpu_t *cpu = nds->cpu;

    static const uint32_t prog[] = {
        /* 0x00 */ 0xE10F0000, /* MRS r0, CPSR         r0 = cpsr */
        /* 0x04 */ 0xE3A010D2, /* MOV r1, #0xD2        r1 = 控制字节（模式 IRQ + I=1 + F=1） */
        /* 0x08 */ 0xE121F001, /* MSR CPSR_c, r1       cpsr 控制字节(bit7-0)写入 → 切 IRQ */
        /* 0x0C */ 0xE10F2000, /* MRS r2, CPSR         r2 = 新 cpsr */
        /* 0x10 */ 0xE14F3000, /* MRS r3, SPSR         r3 = 当前模式(IRQ)的 spsr[1] */
        /* 0x14 */ 0xE3A04055, /* MOV r4, #0x55        r4 = 0x55 */
        /* 0x18 */ 0xE16FF004, /* MSR SPSR, r4         spsr[IRQ] = 0x55 */
        /* 0x1C */ 0xEAFFFFFE, /* B self */
    };

    /* 预置：N=0 Z=0 C=1 V=0 + 模式 0x13(SVC)；SPSR_IRQ[1] = 0x12345678 */
    cpu->cpsr = 0x20000013u;
    cpu->spsr[exec_spsr_index(ARM_MODE_IRQ)] = 0x12345678u;

    run_program(nds, base, prog, sizeof prog / sizeof prog[0],
                base, base + 0x1C, 32);

    CHECK_EQ("mrs r0 = cpsr", cpu->r[0], 0x20000013u);
    /* MSR CPSR_c：控制字节 0xD2 写入 → 标志位保留 0x20000000 + 模式 IRQ(0x12) + I/F 置位 */
    CHECK_EQ("mrs r2 = new cpsr", cpu->r[2], 0x200000D2u);
    CHECK_EQ("mrs r2 mode = IRQ", cpu->r[2] & CPSR_MODE_MASK, ARM_MODE_IRQ);
    CHECK_EQ("mrs r2 I set", (cpu->r[2] & CPSR_I) ? 1u : 0u, 1u);
    CHECK_EQ("mrs r2 F set", (cpu->r[2] & CPSR_F) ? 1u : 0u, 1u);
    CHECK_EQ("mrs r3 = spsr[IRQ]", cpu->r[3], 0x12345678u);
    CHECK_EQ("msr spsr[IRQ] written", cpu->spsr[exec_spsr_index(ARM_MODE_IRQ)], 0x55u);
    CHECK_EQ("msr spsr[SVC] untouched", cpu->spsr[exec_spsr_index(ARM_MODE_SVC)], 0u);
}

/* ---- 10.5 用例：LDM/STM（含 PUSH/POP 别名，DB/IA 模式 + 写回） ---- */
static void test_block_transfer(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE;
    const uint32_t stack = 0x02001000u;

    static const uint32_t prog[] = {
        /* 0x00 */ 0xE3A04011, /* MOV r4, #0x11 */
        /* 0x04 */ 0xE3A05022, /* MOV r5, #0x22 */
        /* 0x08 */ 0xE3A06033, /* MOV r6, #0x33 */
        /* 0x0C */ 0xE3A07044, /* MOV r7, #0x44 */
        /* 0x10 */ 0xE3A00402, /* MOV r0, #0x02000000 */
        /* 0x14 */ 0xE2800A01, /* ADD r0, r0, #0x1000   r0 = 0x02001000 */
        /* 0x18 */ 0xE92000F0, /* STMDB r0!, {r4-r7}    push */
        /* 0x1C */ 0xE3A04000, /* MOV r4, #0 (clobber) */
        /* 0x20 */ 0xE3A05000, /* MOV r5, #0 */
        /* 0x24 */ 0xE3A06000, /* MOV r6, #0 */
        /* 0x28 */ 0xE3A07000, /* MOV r7, #0 */
        /* 0x2C */ 0xE8B000F0, /* LDMIA r0!, {r4-r7}    pop */
        /* 0x30 */ 0xEAFFFFFE, /* B self */
    };

    run_program(nds, base, prog, sizeof prog / sizeof prog[0],
                base, base + 0x30, 64);

    CHECK_EQ("ldm/stm r4 restored", nds->cpu->r[4], 0x11u);
    CHECK_EQ("ldm/stm r5 restored", nds->cpu->r[5], 0x22u);
    CHECK_EQ("ldm/stm r6 restored", nds->cpu->r[6], 0x33u);
    CHECK_EQ("ldm/stm r7 restored", nds->cpu->r[7], 0x44u);
    CHECK_EQ("ldm/stm sp restored", nds->cpu->r[0], stack);

    /* STMDB 从高地址往低写：r4 在最低地址 */
    CHECK_EQ("stmdb mem r4", bus_read32(nds->bus, stack - 16), 0x11u);
    CHECK_EQ("stmdb mem r5", bus_read32(nds->bus, stack - 12), 0x22u);
    CHECK_EQ("stmdb mem r6", bus_read32(nds->bus, stack - 8), 0x33u);
    CHECK_EQ("stmdb mem r7", bus_read32(nds->bus, stack - 4), 0x44u);
}

/* ---- 21-B9zg 用例：ARM7 STM 基址寄存器在列表内保存当前写地址 ---- */
static void test_arm7_stm_base_in_list(nds_t *nds)
{
    arm_cpu_t *cpu = nds->cpu7;
    const uint32_t pc = 0x03808000u;
    bus_write32(nds->bus, pc, 0xE8817FFFu); /* STMIA r1, {r0-r14} */
    cpu->cpsr = ARM_MODE_SYS;
    cpu_reset(cpu, pc);
    cpu->r[0] = 0x11111111u;
    cpu->r[1] = 0x0380A4FCu; /* STMIA 基址：真实保存时 cpsr 已写入 A4F8 */
    cpu->r[2] = 0x8000009Fu;
    exec_set_trace(0);
    cpu_step(cpu);
    CHECK_EQ("arm7 stm base r0", bus_read32(nds->bus, 0x0380A4FCu),
             0x11111111u);
    CHECK_EQ("arm7 stm base self", bus_read32(nds->bus, 0x0380A500u),
             0x0380A500u);
    CHECK_EQ("arm7 stm base r2", bus_read32(nds->bus, 0x0380A504u),
             0x8000009Fu);
    CHECK_EQ("arm7 stm no writeback", cpu->r[1], 0x0380A4FCu);
}

/* ---- 21-B9zn 用例：同一 melonDS A_STM 口径同样作用于 ARM9 ---- */
static void test_arm9_stm_base_in_list(nds_t *nds)
{
    arm_cpu_t *cpu = nds->cpu;
    const uint32_t pc = 0x02008000u;
    bus_write32(nds->bus, pc, 0xE8817FFFu); /* STMIA r1, {r0-r14} */
    cpu->cpsr = ARM_MODE_SYS;
    cpu_reset(cpu, pc);
    cpu->r[0] = 0x11111111u;
    cpu->r[1] = 0x0200A4FCu; /* STMIA 基址：上下文保存的当前写地址槽 */
    cpu->r[2] = 0x8000009Fu;
    exec_set_trace(0);
    cpu_step(cpu);
    CHECK_EQ("arm9 stm base r0", bus_read32(nds->bus, 0x0200A4FCu),
             0x11111111u);
    CHECK_EQ("arm9 stm base self", bus_read32(nds->bus, 0x0200A500u),
             0x0200A500u);
    CHECK_EQ("arm9 stm base r2", bus_read32(nds->bus, 0x0200A504u),
             0x8000009Fu);
    CHECK_EQ("arm9 stm no writeback", cpu->r[1], 0x0200A4FCu);
}

/* ---- 21-B9vv：周期成本骨架：step_cycles 与等待分离 ---- */
static void test_step_cycles(nds_t *nds)
{
    arm_cpu_t *cpu = nds->cpu;
    const uint32_t base = BUS_MAIN_RAM_BASE;

    nds->io->irq[0].ifl = 0;
    nds->io->irq[0].ime = 0;
    nds->io->irq[0].ie = 0;

    /* 无挂起中断时 WFI 不消耗周期：step_cycles=0 */
    bus_write32(nds->bus, base, 0xEE070F90u); /* MCR p15,0,r0,c7,c0,4 */
    cpu_reset(cpu, base);
    cpu_step(cpu);
    CHECK_EQ("wfi step_cycles", cpu->step_cycles, 0u);
    CHECK_EQ("wfi no instruction", (uint32_t)cpu->cycles, 0u);

    /* 21-B9yh：普通指令的 step_cycles 现在含「按取指区域计费」——
       ARM9 主存的**非顺序取指**（分支/跳转后）3、顺序取指与 ITCM/WRAM/IO
       等其它区域 1；ARM7 不额外计费。 */
    bus_write32(nds->bus, base, 0xE3A00001u);     /* mov r0,#1（顺序） */
    bus_write32(nds->bus, base + 4u, 0xEAFFFFFEu); /* B self（非顺序） */
    cpu_reset(cpu, base);
    cpu_step(cpu);
    CHECK_EQ("main RAM seq step_cycles", cpu->step_cycles, 1u);
    CHECK_EQ("normal instruction", (uint32_t)cpu->cycles, 1u);
    cpu_step(cpu);                                 /* 进到 B self（顺序取指） */
    CHECK_EQ("main RAM seq2 step_cycles", cpu->step_cycles, 1u);
    cpu_step(cpu);                                 /* 跳回自身（非顺序取指） */
    CHECK_EQ("main RAM branch step_cycles", cpu->step_cycles, 1u); /* 默认口径 1 */

    /* 21-B9yi：非顺序取指代价可调（NDS_ARM9_NOSEQ / cpu_set_nonseq_cost）。
       同一段代码在代价=3 时应该贵 3 倍——这也是「主存取指加价」口径的单测。 */
    cpu_set_nonseq_cost(3);
    cpu_reset(cpu, base);
    cpu_step(cpu);                                 /* mov r0,#1（顺序） */
    cpu_step(cpu);                                 /* B self（顺序取到分支） */
    cpu_step(cpu);                                 /* 跳回自身：非顺序取指 */
    CHECK_EQ("main RAM branch cost=3", cpu->step_cycles, 3u);
    cpu_set_nonseq_cost(1);
    cpu_reset(cpu, base);

    bus_write32(nds->bus, BUS_ARM9_ITCM_BASE, 0xEAFFFFFEu); /* B self（ITCM） */
    cpu_reset(cpu, BUS_ARM9_ITCM_BASE);
    cpu_step(cpu);
    CHECK_EQ("ITCM step_cycles", cpu->step_cycles, 1u);

    /* ARM7：WRAM 与主存都不额外计费（代码主要在 WRAM，实测指令数已低于参考核） */
    bus_write32(nds->bus, BUS_ARM7_WRAM_BASE + 0x100u, 0xEAFFFFFEu);
    cpu_reset(nds->cpu7, BUS_ARM7_WRAM_BASE + 0x100u);
    cpu_step(nds->cpu7);
    CHECK_EQ("arm7 WRAM step_cycles", nds->cpu7->step_cycles, 1u);

    bus_write32(nds->bus, base + 0x1000u, 0xEAFFFFFEu);
    cpu_reset(nds->cpu7, base + 0x1000u);
    cpu_step(nds->cpu7);
    CHECK_EQ("arm7 main RAM step_cycles", nds->cpu7->step_cycles, 1u);
}

/* ---- 21-B9vw：事件目标调度最小事件表 ---- */
static int timing_hit_a, timing_hit_b;
static void timing_cb_a(void *ctx) { (void)ctx; timing_hit_a++; }
static void timing_cb_b(void *ctx) { (void)ctx; timing_hit_b++; }

static void test_timing_events(nds_t *nds)
{
    (void)nds;
    timing_t t;
    timing_init(&t);
    timing_hit_a = 0;
    timing_hit_b = 0;

    timing_arm(&t, 0, 20, timing_cb_a, NULL);
    timing_arm(&t, 1, 10, timing_cb_b, NULL);
    CHECK_EQ("timing next earliest", (uint32_t)timing_next(&t), 10u);
    timing_advance(&t, 5);
    CHECK_EQ("timing before b", timing_hit_b, 0);
    timing_advance(&t, 10);
    CHECK_EQ("timing hit b", timing_hit_b, 1);
    CHECK_EQ("timing not hit a", timing_hit_a, 0);
    timing_advance(&t, 20);
    CHECK_EQ("timing hit a", timing_hit_a, 1);
    CHECK_EQ("timing no armed", (uint32_t)timing_next(&t), 0xFFFFFFFFu);

    timing_arm(&t, 2, 5, timing_cb_a, NULL);
    timing_advance(&t, 26);
    CHECK_EQ("timing rearm hit", timing_hit_a, 2);
}

/* ---- 10.6 用例：乘法 MUL/MLA + 长乘 UMULL/UMLAL/SMULL ---- */
static void test_mul(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE;
    static const uint32_t prog[] = {
        /* 0x00 */ 0xE3A00006, /* MOV r0, #6            r0 = 6 */
        /* 0x04 */ 0xE3A01007, /* MOV r1, #7            r1 = 7 */
        /* 0x08 */ 0xE0020091, /* MUL r2, r1, r0        r2 = 42 */
        /* 0x0C */ 0xE3A03005, /* MOV r3, #5            r3 = 5 */
        /* 0x10 */ 0xE0241390, /* MLA r4, r0, r3, r1    r4 = 6*5+7 = 37 */
        /* 0x14 */ 0xE0865190, /* UMULL r5, r6, r0, r1  r6:r5 = 42 */
        /* 0x18 */ 0xE3A0702A, /* MOV r7, #42           预置累加基数 */
        /* 0x1C */ 0xE3A08000, /* MOV r8, #0 */
        /* 0x20 */ 0xE0A87190, /* UMLAL r7, r8, r0, r1  r8:r7 = 42 + 42 = 84 */
        /* 0x24 */ 0xE3E00000, /* MVN r0, #0            r0 = -1 */
        /* 0x28 */ 0xE3A01001, /* MOV r1, #1            r1 = 1 */
        /* 0x2C */ 0xE0CA9190, /* SMULL r9, r10, r0, r1 r10:r9 = -1 */
        /* 0x30 */ 0xEAFFFFFE, /* B self */
    };
    run_program(nds, base, prog, sizeof prog / sizeof prog[0],
                base, base + 0x30, 32);

    CHECK_EQ("mul MUL",      nds->cpu->r[2],  42u);
    CHECK_EQ("mul MLA",      nds->cpu->r[4],  37u);
    CHECK_EQ("mul UMULL lo", nds->cpu->r[5],  42u);
    CHECK_EQ("mul UMULL hi", nds->cpu->r[6],  0u);
    CHECK_EQ("mul UMLAL lo", nds->cpu->r[7],  84u);
    CHECK_EQ("mul UMLAL hi", nds->cpu->r[8],  0u);
    CHECK_EQ("mul SMULL lo", nds->cpu->r[9],  0xFFFFFFFFu);
    CHECK_EQ("mul SMULL hi", nds->cpu->r[10], 0xFFFFFFFFu);
}

/* ---- 10.7 用例：字节/半字访存 + 前/后变址 + 寄存器偏移 + 符号扩展 ---- */
static void test_byte_halfword(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE;
    const uint32_t data = 0x02001000u;

    bus_write8(nds->bus, data + 0, 0xAB);
    bus_write8(nds->bus, data + 1, 0x80);
    bus_write16(nds->bus, data + 2, 0x1234);
    bus_write16(nds->bus, data + 4, 0x8000);
    bus_write32(nds->bus, data + 8, 0xDEADBEEF);

    static const uint32_t prog[] = {
        /* 0x00 */ 0xE3A00402, /* MOV r0, #0x02000000 */
        /* 0x04 */ 0xE2800A01, /* ADD r0, r0, #0x1000   r0 = data */
        /* 0x08 */ 0xE5D01000, /* LDRB r1, [r0]         r1 = 0xAB */
        /* 0x0C */ 0xE1D020D1, /* LDRSB r2, [r0, #1]    r2 = 0xFFFFFF80 */
        /* 0x10 */ 0xE1D030B2, /* LDRH r3, [r0, #2]     r3 = 0x1234 */
        /* 0x14 */ 0xE1D040F4, /* LDRSH r4, [r0, #4]    r4 = 0xFFFF8000 */
        /* 0x18 */ 0xE3A0505A, /* MOV r5, #0x5A         r5 = 0x5A */
        /* 0x1C */ 0xE5C05008, /* STRB r5, [r0, #8]     mem[8]=0x5A → 0xDEADBE5A */
        /* 0x20 */ 0xE3A08008, /* MOV r8, #8            r8 = 8 */
        /* 0x24 */ 0xE7907008, /* LDR r7, [r0, r8]      r7 = 0xDEADBE5A */
        /* 0x28 */ 0xE4909004, /* LDR r9, [r0], #4      r9 = 0x123480AB, r0+=4 */
        /* 0x2C */ 0xEAFFFFFE, /* B self */
    };

    run_program(nds, base, prog, sizeof prog / sizeof prog[0],
                base, base + 0x2C, 64);

    CHECK_EQ("bhw LDRB",          nds->cpu->r[1], 0xABu);
    CHECK_EQ("bhw LDRSB",         nds->cpu->r[2], 0xFFFFFF80u);
    CHECK_EQ("bhw LDRH",          nds->cpu->r[3], 0x1234u);
    CHECK_EQ("bhw LDRSH",         nds->cpu->r[4], 0xFFFF8000u);
    CHECK_EQ("bhw STRB mem",      bus_read8(nds->bus, data + 8), 0x5Au);
    CHECK_EQ("bhw LDR regoff",    nds->cpu->r[7], 0xDEADBE5Au);
    CHECK_EQ("bhw LDR post r9",   nds->cpu->r[9], 0x123480ABu);
    CHECK_EQ("bhw LDR post r0",   nds->cpu->r[0], data + 4);
}

/* ---- 10.8 用例：SWI 记录软件中断号、继续执行（已知号走 BIOS HLE） ---- */
static void test_swi(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE;
    static const uint32_t prog[] = {
        /* 0x00 */ 0xEF030000, /* SWI 0x03 (WaitByLoop，已知号 → HLE 处理后继续) */
        /* 0x04 */ 0xE3A00005, /* MOV r0, #5（应继续执行） */
        /* 0x08 */ 0xEAFFFFFE, /* B self */
    };
    run_program(nds, base, prog, sizeof prog / sizeof prog[0],
                base, base + 0x08, 16);

    CHECK_EQ("swi num recorded", nds->cpu->swi_num, 0x030000u);
    CHECK_EQ("swi continues", nds->cpu->r[0], 5u);
    CHECK_EQ("swi PC at halt", nds->cpu->r[15], base + 0x08);
}

/* ---- 10.9 用例：SWP 寄存器与内存交换 ---- */
static void test_swp(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE;
    const uint32_t data = 0x02001000u;
    bus_write32(nds->bus, data, 0xDEADBEEF);

    static const uint32_t prog[] = {
        /* 0x00 */ 0xE3A00402, /* MOV r0, #0x02000000 */
        /* 0x04 */ 0xE2800A01, /* ADD r0, r0, #0x1000   r0 = data */
        /* 0x08 */ 0xE3A01011, /* MOV r1, #0x11         r1 = 0x11 */
        /* 0x0C */ 0xE1002091, /* SWP r2, r1, [r0]      r2 = 旧值, mem = 0x11 */
        /* 0x10 */ 0xEAFFFFFE, /* B self */
    };
    run_program(nds, base, prog, sizeof prog / sizeof prog[0],
                base, base + 0x10, 16);

    CHECK_EQ("swp r2 = old mem", nds->cpu->r[2], 0xDEADBEEFu);
    CHECK_EQ("swp mem = new", bus_read32(nds->bus, data), 0x11u);
}

/* ---- 10.10 用例：MRC/MCR 协处理器访问（CP15 桩） ---- */
static void test_coprocessor(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE;
    static const uint32_t prog[] = {
        /* 0x00 */ 0xE3A00055, /* MOV r0, #0x55 */
        /* 0x04 */ 0xEE010F10, /* MCR p15, 0, r0, c1, c0, 0  → cp15[1] = 0x55 */
        /* 0x08 */ 0xE3A01000, /* MOV r1, #0（clobber） */
        /* 0x0C */ 0xEE111F10, /* MRC p15, 0, r1, c1, c0, 0  → r1 = cp15[1] */
        /* 0x10 */ 0xEAFFFFFE, /* B self */
    };
    run_program(nds, base, prog, sizeof prog / sizeof prog[0],
                base, base + 0x10, 16);

    CHECK_EQ("mcr cp15 written", nds->cpu->cp15[1], 0x55u);
    CHECK_EQ("mrc cp15 read", nds->cpu->r[1], 0x55u);
}

/* ---- 10.11 用例：综合（栈保存 + SWI + MUL + 半字循环搬 VRAM） ---- */
static void test_stage10_integration(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE;
    const uint32_t vram = BUS_VRAM_BASE;

    static const uint32_t prog[] = {
        /* 0x00 */ 0xE3A00402, /* MOV r0, #0x02000000 */
        /* 0x04 */ 0xE2800A02, /* ADD r0, r0, #0x2000   r0 = 0x02002000（栈） */
        /* 0x08 */ 0xE1A0D000, /* MOV sp, r0            sp = r0 */
        /* 0x0C */ 0xE3A04011, /* MOV r4, #0x11 */
        /* 0x10 */ 0xE3A05022, /* MOV r5, #0x22 */
        /* 0x14 */ 0xE3A06033, /* MOV r6, #0x33 */
        /* 0x18 */ 0xE92D00F0, /* PUSH {r4-r7}          STMDB sp!, {r4-r7} */
        /* 0x1C */ 0xE3A0000A, /* MOV r0, #10 */
        /* 0x20 */ 0xE3A01014, /* MOV r1, #20 */
        /* 0x24 */ 0xE0020091, /* MUL r2, r1, r0        r2 = 200 */
        /* 0x28 */ 0xEF030000, /* SWI 0x03 (WaitByLoop，已知号 → HLE 继续) */
        /* 0x2C */ 0xE3A03406, /* MOV r3, #0x06000000   r3 = VRAM */
        /* 0x30 */ 0xE3A04C7C, /* MOV r4, #0x7C00       r4 = 红 */
        /* 0x34 */ 0xE3A05010, /* MOV r5, #16           r5 = 16 半字 */
        /* 0x38 */ 0xE1C340B0, /* STRH r4, [r3]         写半字 */
        /* 0x3C */ 0xE2833002, /* ADD r3, r3, #2        前进 2 字节 */
        /* 0x40 */ 0xE2555001, /* SUBS r5, r5, #1       计数-1 */
        /* 0x44 */ 0x1AFFFFFB, /* BNE 0x38             循环 */
        /* 0x48 */ 0xE8BD00F0, /* POP {r4-r7}           LDMIA sp!, {r4-r7} */
        /* 0x4C */ 0xEAFFFFFE, /* B self */
    };

    run_program(nds, base, prog, sizeof prog / sizeof prog[0],
                base, base + 0x4C, 256);

    CHECK_EQ("itg r2 (MUL)", nds->cpu->r[2], 200u);
    CHECK_EQ("itg swi_num", nds->cpu->swi_num, 0x030000u);
    CHECK_EQ("itg r4 restored", nds->cpu->r[4], 0x11u);
    CHECK_EQ("itg r5 restored", nds->cpu->r[5], 0x22u);

    int ok = 1;
    for (int i = 0; i < 16; i++)
        if (bus_read16(nds->bus, vram + 2u * i) != 0x7C00u) { ok = 0; break; }
    CHECK_EQ("itg vram fill 16px", ok, 1);
}

/* ---- 11.3 用例：Div 有符号除法 + Sqrt 开方 ---- */
static void test_bios_div_sqrt(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE;
    static const uint32_t prog_div[] = {
        /* 0x00 */ 0xEF090000, /* SWI 0x09 (Div) */
        /* 0x04 */ 0xEAFFFFFE, /* B self */
    };
    static const uint32_t prog_sqrt[] = {
        /* 0x00 */ 0xEF0D0000, /* SWI 0x0D (Sqrt) */
        /* 0x04 */ 0xEAFFFFFE, /* B self */
    };

    /* 正数除法：1234 / 10 → 123 r 4 */
    nds->cpu->r[0] = 1234u;
    nds->cpu->r[1] = 10u;
    run_program(nds, base, prog_div, 2, base, base + 4, 8);
    CHECK_EQ("div quot", nds->cpu->r[0], 123u);
    CHECK_EQ("div rem",  nds->cpu->r[1], 4u);
    CHECK_EQ("div abs",  nds->cpu->r[3], 123u);

    /* 负数除法：-1234 / 10 → -123 r -4, |商|=123 */
    nds->cpu->r[0] = (uint32_t)(int32_t)(-1234);
    nds->cpu->r[1] = 10u;
    run_program(nds, base, prog_div, 2, base, base + 4, 8);
    CHECK_EQ("div neg quot", nds->cpu->r[0], (uint32_t)(int32_t)(-123));
    CHECK_EQ("div neg rem",  nds->cpu->r[1], (uint32_t)(int32_t)(-4));
    CHECK_EQ("div neg abs",  nds->cpu->r[3], 123u);

    /* 开方：100 → 10、2 → 1、最大 → 65535 */
    nds->cpu->r[0] = 100u;
    run_program(nds, base, prog_sqrt, 2, base, base + 4, 8);
    CHECK_EQ("sqrt 100", nds->cpu->r[0], 10u);
    nds->cpu->r[0] = 2u;
    run_program(nds, base, prog_sqrt, 2, base, base + 4, 8);
    CHECK_EQ("sqrt 2", nds->cpu->r[0], 1u);
    nds->cpu->r[0] = 0xFFFFFFFFu;
    run_program(nds, base, prog_sqrt, 2, base, base + 4, 8);
    CHECK_EQ("sqrt max", nds->cpu->r[0], 65535u);
}

/* ---- 11.4 用例：CpuSet / CpuFastSet 搬移与填充 ---- */
static void test_bios_cpuset(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE;
    uint32_t src = 0x02001000u;
    uint32_t dst = 0x02002000u;

    bus_write32(nds->bus, src + 0, 0x11111111u);
    bus_write32(nds->bus, src + 4, 0x22222222u);
    bus_write32(nds->bus, src + 8, 0x33333333u);
    bus_write32(nds->bus, src + 12, 0x44444444u);

    static const uint32_t prog_cpuset[] = {
        /* 0x00 */ 0xEF0B0000, /* SWI 0x0B (CpuSet) */
        /* 0x04 */ 0xEAFFFFFE, /* B self */
    };
    static const uint32_t prog_fast[] = {
        /* 0x00 */ 0xEF0C0000, /* SWI 0x0C (CpuFastSet) */
        /* 0x04 */ 0xEAFFFFFE, /* B self */
    };

    /* 32 位拷贝 4 字：ctl = bit26(32bit) | 4 */
    nds->cpu->r[0] = src;
    nds->cpu->r[1] = dst;
    nds->cpu->r[2] = (1u << 26) | 4u;
    run_program(nds, base, prog_cpuset, 2, base, base + 4, 8);
    CHECK_EQ("cpuset copy w0", bus_read32(nds->bus, dst + 0), 0x11111111u);
    CHECK_EQ("cpuset copy w1", bus_read32(nds->bus, dst + 4), 0x22222222u);
    CHECK_EQ("cpuset copy w2", bus_read32(nds->bus, dst + 8), 0x33333333u);
    CHECK_EQ("cpuset copy w3", bus_read32(nds->bus, dst + 12), 0x44444444u);

    /* 16 位填充 8 半字：ctl = bit24(固定源) | 8 */
    nds->cpu->r[0] = src;         /* src 低半字 0x1111 */
    nds->cpu->r[1] = dst;
    nds->cpu->r[2] = (1u << 24) | 8u;
    run_program(nds, base, prog_cpuset, 2, base, base + 4, 8);
    int ok = 1;
    for (int i = 0; i < 8; i++)
        if (bus_read16(nds->bus, dst + 2u * i) != 0x1111u) { ok = 0; break; }
    CHECK_EQ("cpuset fill 16bit", ok, 1);

    /* CpuFastSet 拷贝 4 字 */
    nds->cpu->r[0] = src;
    nds->cpu->r[1] = dst;
    nds->cpu->r[2] = 4u;
    run_program(nds, base, prog_fast, 2, base, base + 4, 8);
    CHECK_EQ("fastset copy w0", bus_read32(nds->bus, dst + 0), 0x11111111u);
    CHECK_EQ("fastset copy w3", bus_read32(nds->bus, dst + 12), 0x44444444u);
}

/* ---- 11.5 用例：BitUnPack / LZ77 / RL / Huffman 解压 ---- */
static void test_bios_decompress(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE;
    uint32_t src = 0x02001000u;
    uint32_t dst = 0x02002000u;

    /* --- BitUnPack：src_w=4, dst_w=8，源 {0x12,0x34} → 0x01,0x02,0x03,0x04 --- */
    uint32_t info = 0x02003000u;
    bus_write8(nds->bus, src + 0, 0x12);
    bus_write8(nds->bus, src + 1, 0x34);
    bus_write16(nds->bus, info + 0, 2);   /* 源长 2 字节 */
    bus_write8(nds->bus, info + 2, 4);    /* 源位宽 4 */
    bus_write8(nds->bus, info + 3, 8);    /* 目的位宽 8 */
    bus_write32(nds->bus, info + 4, 0);   /* 偏移 0，零标志 0 */

    static const uint32_t prog_unpack[] = {
        /* 0x00 */ 0xEF100000, /* SWI 0x10 (BitUnPack) */
        /* 0x04 */ 0xEAFFFFFE,
    };
    nds->cpu->r[0] = src;
    nds->cpu->r[1] = dst;
    nds->cpu->r[2] = info;
    run_program(nds, base, prog_unpack, 2, base, base + 4, 8);
    CHECK_EQ("unpack b0", bus_read8(nds->bus, dst + 0), 0x01u);
    CHECK_EQ("unpack b1", bus_read8(nds->bus, dst + 1), 0x02u);
    CHECK_EQ("unpack b2", bus_read8(nds->bus, dst + 2), 0x03u);
    CHECK_EQ("unpack b3", bus_read8(nds->bus, dst + 3), 0x04u);

    /* 带偏移 + 零标志：源 {0x12,0x03} → 0x31,0x32,0x30,0x33 */
    bus_write8(nds->bus, src + 0, 0x12);
    bus_write8(nds->bus, src + 1, 0x03);
    bus_write32(nds->bus, info + 4, 0x80000030u); /* 零标志 + 偏移 0x30 */
    run_program(nds, base, prog_unpack, 2, base, base + 4, 8);
    CHECK_EQ("unpack off b0", bus_read8(nds->bus, dst + 0), 0x31u);
    CHECK_EQ("unpack off b1", bus_read8(nds->bus, dst + 1), 0x32u);
    CHECK_EQ("unpack off b2", bus_read8(nds->bus, dst + 2), 0x30u);
    CHECK_EQ("unpack off b3", bus_read8(nds->bus, dst + 3), 0x33u);

    /* --- LZ77：解压出 "ABCABCABC" --- */
    bus_write32(nds->bus, src + 0, 0x00000910u); /* size=9, type=1 */
    bus_write8(nds->bus, src + 4, 0x1C);          /* 块 0-2 字面量、3-5 回溯 */
    bus_write8(nds->bus, src + 5, 'A');
    bus_write8(nds->bus, src + 6, 'B');
    bus_write8(nds->bus, src + 7, 'C');
    bus_write16(nds->bus, src + 8, 0x0200);       /* disp=2, len=3 */
    bus_write16(nds->bus, src + 10, 0x0200);
    bus_write16(nds->bus, src + 12, 0x0200);

    static const uint32_t prog_lz77[] = {
        /* 0x00 */ 0xEF110000, /* SWI 0x11 (LZ77 Wram) */
        /* 0x04 */ 0xEAFFFFFE,
    };
    nds->cpu->r[0] = src;
    nds->cpu->r[1] = dst;
    run_program(nds, base, prog_lz77, 2, base, base + 4, 8);
    CHECK_EQ("lz77 b0", bus_read8(nds->bus, dst + 0), (uint8_t)'A');
    CHECK_EQ("lz77 b2", bus_read8(nds->bus, dst + 2), (uint8_t)'C');
    CHECK_EQ("lz77 b3", bus_read8(nds->bus, dst + 3), (uint8_t)'A');
    CHECK_EQ("lz77 b8", bus_read8(nds->bus, dst + 8), (uint8_t)'C');

    /* --- RL：解压出 "AAAAABBBBB" --- */
    bus_write32(nds->bus, src + 0, 0x00000A30u); /* size=10, type=3 */
    bus_write8(nds->bus, src + 4, 0x82);          /* 压缩 5 字节 */
    bus_write8(nds->bus, src + 5, 'A');
    bus_write8(nds->bus, src + 6, 0x82);
    bus_write8(nds->bus, src + 7, 'B');

    static const uint32_t prog_rl[] = {
        /* 0x00 */ 0xEF140000, /* SWI 0x14 (RL Wram) */
        /* 0x04 */ 0xEAFFFFFE,
    };
    nds->cpu->r[0] = src;
    nds->cpu->r[1] = dst;
    run_program(nds, base, prog_rl, 2, base, base + 4, 8);
    CHECK_EQ("rl b0", bus_read8(nds->bus, dst + 0), (uint8_t)'A');
    CHECK_EQ("rl b4", bus_read8(nds->bus, dst + 4), (uint8_t)'A');
    CHECK_EQ("rl b5", bus_read8(nds->bus, dst + 5), (uint8_t)'B');
    CHECK_EQ("rl b9", bus_read8(nds->bus, dst + 9), (uint8_t)'B');

    /* --- Huffman：解压出 "ABBA" --- */
    bus_write32(nds->bus, src + 0, 0x00000428u); /* 数据位宽8, type=2, size=4 */
    bus_write8(nds->bus, src + 4, 0x01);          /* 树长字节 = (表字节数/2)-1 = 1 */
    bus_write8(nds->bus, src + 5, 0xC0);          /* 根：node0/node1 都是数据 */
    bus_write8(nds->bus, src + 6, 'A');           /* node0 数据 */
    bus_write8(nds->bus, src + 7, 'B');           /* node1 数据 */
    bus_write8(nds->bus, src + 8, 0x00);          /* 填充，使树表 4 字节 */
    bus_write32(nds->bus, src + 9, 0x60000000u);  /* 位流 0,1,1,0 = A,B,B,A */

    static const uint32_t prog_huff[] = {
        /* 0x00 */ 0xEF130000, /* SWI 0x13 (Huff) */
        /* 0x04 */ 0xEAFFFFFE,
    };
    nds->cpu->r[0] = src;
    nds->cpu->r[1] = dst;
    run_program(nds, base, prog_huff, 2, base, base + 4, 8);
    CHECK_EQ("huff b0", bus_read8(nds->bus, dst + 0), (uint8_t)'A');
    CHECK_EQ("huff b1", bus_read8(nds->bus, dst + 1), (uint8_t)'B');
    CHECK_EQ("huff b2", bus_read8(nds->bus, dst + 2), (uint8_t)'B');
    CHECK_EQ("huff b3", bus_read8(nds->bus, dst + 3), (uint8_t)'A');
}

/* ---- 11.6 用例：Halt / IntrWait / VBlankIntrWait ---- */
static void test_bios_wait(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE;

    /* VBlankIntrWait：SWI 0x05 等 VBlank，未置位时 PC 停在 SWI，置位后前进并清位 */
    static const uint32_t prog_vb[] = {
        /* 0x00 */ 0xEF050000, /* SWI 0x05 (VBlankIntrWait) */
        /* 0x04 */ 0xE3A00005, /* MOV r0, #5 */
        /* 0x08 */ 0xEAFFFFFE, /* B self */
    };
    bus_write32(nds->bus, base + 0, prog_vb[0]);
    bus_write32(nds->bus, base + 4, prog_vb[1]);
    bus_write32(nds->bus, base + 8, prog_vb[2]);
    exec_set_trace(0);
    cpu_reset(nds->cpu, base);
    for (int i = 0; i < 4; i++)
        cpu_step(nds->cpu);
    CHECK_EQ("vbwait stuck PC", nds->cpu->r[15], base);
    io_set_vblank(nds->io);
    cpu_step(nds->cpu);   /* SWI 看到 VBlank → 清位 → 前进 */
    cpu_step(nds->cpu);   /* MOV r0,#5 */
    exec_set_trace(1);
    CHECK_EQ("vbwait r0", nds->cpu->r[0], 5u);
    CHECK_EQ("vbwait PC", nds->cpu->r[15], base + 8);
    CHECK_EQ("vbwait flag cleared", nds->io->irq[0].ifl & IO_IF_VBLANK, 0u);

    /* IntrWait：SWI 0x04 等 r1 指定的位，置位后前进并清位 */
    static const uint32_t prog_iw[] = {
        /* 0x00 */ 0xEF040000, /* SWI 0x04 (IntrWait) */
        /* 0x04 */ 0xE3A00006, /* MOV r0, #6 */
        /* 0x08 */ 0xEAFFFFFE,
    };
    bus_write32(nds->bus, base + 0, prog_iw[0]);
    bus_write32(nds->bus, base + 4, prog_iw[1]);
    bus_write32(nds->bus, base + 8, prog_iw[2]);
    cpu_reset(nds->cpu, base);
    nds->cpu->r[1] = IO_IF_VBLANK; /* 等 VBlank 位 */
    exec_set_trace(0);
    cpu_step(nds->cpu);
    cpu_step(nds->cpu);
    CHECK_EQ("intrwait stuck PC", nds->cpu->r[15], base);
    CHECK_EQ("intrwait ime=1", nds->io->irq[0].ime, 1u);
    io_set_vblank(nds->io);
    cpu_step(nds->cpu);
    cpu_step(nds->cpu);
    exec_set_trace(1);
    CHECK_EQ("intrwait r0", nds->cpu->r[0], 6u);
    CHECK_EQ("intrwait flag cleared", nds->io->irq[0].ifl & IO_IF_VBLANK, 0u);

    /* Halt：SWI 0x06 等 (IE & IF) != 0 */
    static const uint32_t prog_halt[] = {
        /* 0x00 */ 0xEF060000, /* SWI 0x06 (Halt) */
        /* 0x04 */ 0xE3A00007, /* MOV r0, #7 */
        /* 0x08 */ 0xEAFFFFFE,
    };
    bus_write32(nds->bus, base + 0, prog_halt[0]);
    bus_write32(nds->bus, base + 4, prog_halt[1]);
    bus_write32(nds->bus, base + 8, prog_halt[2]);
    cpu_reset(nds->cpu, base);
    /* 无中断时 Halt 等待 */
    exec_set_trace(0);
    cpu_step(nds->cpu);
    cpu_step(nds->cpu);
    CHECK_EQ("halt stuck PC", nds->cpu->r[15], base);
    /* 置 IE & IF 后 Halt 返回（Halt 的 HLE 直接轮询 IE&IF；IME=0 使真实 IRQ 向量不介入，
       便于单独验证 Halt 的「满足即返回」语义） */
    nds->io->irq[0].ime = 0;
    nds->io->irq[0].ie = IO_IF_VBLANK;
    nds->io->irq[0].ifl = IO_IF_VBLANK;
    cpu_step(nds->cpu);
    cpu_step(nds->cpu);
    exec_set_trace(1);
    CHECK_EQ("halt r0", nds->cpu->r[0], 7u);
    CHECK_EQ("halt PC", nds->cpu->r[15], base + 8);
}

/* ---- 21-B9wt 用例一：ARM7 SWI 走真机低地址路径（分发器 + WaitByLoop）----
   Thumb `swi 3` 的真机流程：
     1) SWI 异常：SPSR_svc=调用方 CPSR、lr_svc=返回地址、PC=0x00000008；
     2) 向量 b 0x1080；
     3) 0x1080 分发器：SVC 栈压 r4/r12/lr/SPSR 四字、切 System（I 取自 SPSR）、
        压 System lr、r12=编号、按表跳 0x115C；
     4) 0x115C subs/bgt 逐轮减 r0；5) 0x112C swi_complete 恢复现场返回。 */
static void test_bios7_low_wait(nds_t *nds)
{
    arm_cpu_t *cpu7 = nds->cpu7;
    const uint32_t base = BUS_MAIN_RAM_BASE;
    const uint32_t sp_sys = 0x0380FD80u, sp_svc = 0x0380FFC0u;

    bus_write16(nds->bus, base + 0, 0xDF03u);   /* Thumb swi 3 */
    bus_write16(nds->bus, base + 2, 0xE7FEu);   /* 占位（B self） */
    nds->io->irq[1].ime = 0;
    nds->io->irq[1].ie = 0;
    nds->io->irq[1].ifl = 0;
    power_halt_wake(&nds->io->power);
    cpu7->cpsr = ARM_MODE_SYS | CPSR_T | CPSR_I;
    cpu7->r[0] = 3;
    cpu7->r[4] = 0xDEADBEEFu;                   /* 分发器要把调用方的 r4 压栈 */
    cpu7->r[12] = 0x9ABCu;                      /* 同上：调用方的 r12 */
    cpu7->r[14] = base + 2u;                    /* System lr（调用方返回地址） */
    cpu7->r13_sys = sp_sys; cpu7->r[13] = sp_sys;
    cpu7->r13_bank[2] = sp_svc;                 /* SVC 栈（分发器压帧用） */
    cpu_reset(cpu7, base);

    exec_set_trace(0);
    cpu_step(cpu7);                             /* SWI 异常 */
    CHECK_EQ("a7 swi vec pc", cpu7->r[15], 0x00000008u);
    CHECK_EQ("a7 swi mode", cpu7->cpsr & CPSR_MODE_MASK, ARM_MODE_SVC);
    CHECK_EQ("a7 swi lr (thumb +2)", cpu7->r[14], base + 2);
    CHECK_EQ("a7 swi spsr", cpu7->spsr[2], ARM_MODE_SYS | CPSR_T | CPSR_I);

    cpu_step(cpu7);                             /* 向量 → 0x1080 */
    CHECK_EQ("a7 vec to handler", cpu7->r[15], 0x00001080u);

    /* 21-B9ye：分发器逐条执行（0x1080-0x10A8，共 11 条）。此前把整段合成
       一步执行、PC 直接跳到函数体；参考核直方图显示它逐条走这 11 个地址，
       且 IRQ 能插在 0x109C-0x10A8 之间。下面逐条断言副作用。 */
    cpu_step(cpu7);                             /* 0x1080 push {r4,r12,lr} */
    CHECK_EQ("a7 disp1 pc", cpu7->r[15], 0x00001084u);
    CHECK_EQ("a7 disp1 sp_svc -12", cpu7->r[13], sp_svc - 12u); /* 仍在 SVC 模式 */
    CHECK_EQ("a7 disp1 stk r4", a7_read32(nds, sp_svc - 12u), 0xDEADBEEFu);
    CHECK_EQ("a7 disp1 stk r12", a7_read32(nds, sp_svc - 8u), 0x9ABCu);
    CHECK_EQ("a7 disp1 stk svc lr", a7_read32(nds, sp_svc - 4u), base + 2);
    CHECK_EQ("a7 disp1 cost", cpu7->step_cycles, 3u);
    cpu_step(cpu7);                             /* 0x1084 mrs r4, spsr */
    CHECK_EQ("a7 disp2 pc", cpu7->r[15], 0x00001088u);
    CHECK_EQ("a7 disp2 r4=spsr", cpu7->r[4], ARM_MODE_SYS | CPSR_T | CPSR_I);
    cpu_step(cpu7);                             /* 0x1088 push {r4} */
    CHECK_EQ("a7 disp3 sp_svc -16", cpu7->r[13], sp_svc - 16u);
    CHECK_EQ("a7 disp3 stk spsr", a7_read32(nds, sp_svc - 16u),
             ARM_MODE_SYS | CPSR_T | CPSR_I);
    cpu_step(cpu7);                             /* 0x108C and r4,#0x80 */
    CHECK_EQ("a7 disp4 r4 and", cpu7->r[4], CPSR_I);
    cpu_step(cpu7);                             /* 0x1090 orr r4,#0x1f */
    CHECK_EQ("a7 disp5 r4 orr", cpu7->r[4], CPSR_I | ARM_MODE_SYS);
    cpu_step(cpu7);                             /* 0x1094 ldrb r12,[lr,#-2] */
    CHECK_EQ("a7 disp6 pc", cpu7->r[15], 0x00001098u);
    CHECK_EQ("a7 disp6 swi num", cpu7->r[12], 3u);
    cpu_step(cpu7);                             /* 0x1098 msr cpsr_fc,r4 */
    CHECK_EQ("a7 disp7 mode sys", cpu7->cpsr & CPSR_MODE_MASK, ARM_MODE_SYS);
    CHECK_EQ("a7 disp7 svc bank kept", cpu7->r13_bank[2], sp_svc - 16u);
    CHECK_EQ("a7 disp7 t cleared", cpu7->cpsr & CPSR_T, 0u);
    CHECK_EQ("a7 disp7 i from spsr", cpu7->cpsr & CPSR_I, CPSR_I);
    cpu_step(cpu7);                             /* 0x109C push {lr}（System 栈） */
    CHECK_EQ("a7 disp8 sp_sys -4", cpu7->r[13], sp_sys - 4u);
    CHECK_EQ("a7 disp8 stk sys lr", a7_read32(nds, sp_sys - 4u), 0x02000002u);
    cpu_step(cpu7);                             /* 0x10A0 cmp r12,#0x20 */
    CHECK_EQ("a7 disp9 n set", (cpu7->cpsr >> 31) & 1u, 1u);
    cpu_step(cpu7);                             /* 0x10A4 movge（N!=V → 不执行） */
    CHECK_EQ("a7 disp10 num kept", cpu7->r[12], 3u);
    cpu_step(cpu7);                             /* 0x10A8 ldr pc,[pc,r12,lsl#2] */
    CHECK_EQ("a7 disp body pc", cpu7->r[15], 0x0000115Cu);

    cpu_step(cpu7);                             /* subs r0,#1 */
    CHECK_EQ("a7 loop r0-1", cpu7->r[0], 2u);
    cpu_step(cpu7);                             /* bgt（取）*/
    CHECK_EQ("a7 loop take pc", cpu7->r[15], 0x0000115Cu);
    CHECK_EQ("a7 loop cost", cpu7->step_cycles, 2u); /* subs 1 + bgt 2 = 3/轮 */
    cpu_step(cpu7);                             /* subs */
    cpu_step(cpu7);                             /* bgt（取）*/
    cpu_step(cpu7);                             /* subs → 0 */
    CHECK_EQ("a7 loop r0-3", cpu7->r[0], 0u);
    cpu_step(cpu7);                             /* bgt（不取，Z=1）*/
    CHECK_EQ("a7 loop fall pc", cpu7->r[15], 0x00001164u);

    for (int i = 0; i < 32 && cpu7->r[15] != base + 2; i++)
        cpu_step(cpu7);
    CHECK_EQ("a7 swi ret pc", cpu7->r[15], base + 2);
    CHECK_EQ("a7 swi ret cpsr", cpu7->cpsr, ARM_MODE_SYS | CPSR_T | CPSR_I);
    CHECK_EQ("a7 swi ret sp_sys", cpu7->r[13], sp_sys);
    CHECK_EQ("a7 swi ret sp_svc", cpu7->r13_bank[2], sp_svc);
    exec_set_trace(1);
}

/* ---- 21-B9wt 用例二：ARM7 Halt 暂停 + IRQ 打断顺序（参考级关键点）----
   真机 Halt 执行到 0x1154 写完 HALTCNT 后暂停，被打断的是 0x1158（b
   swi_complete），不是调用方代码；IRQ 入口在 0x1FB0 压六字帧并跳到
   [0x03FFFFFC] 的 game handler，返回后 SWI 才在 0x112C 收尾。 */
static void test_bios7_low_halt_irq(nds_t *nds)
{
    arm_cpu_t *cpu7 = nds->cpu7;
    const uint32_t base = BUS_MAIN_RAM_BASE;
    const uint32_t sp_sys = 0x0380FD80u, sp_svc = 0x0380FFC0u;
    const uint32_t sp_irq = 0x0380FF80u;
    const uint32_t handler = base + 0x40u;

    bus_write16(nds->bus, base + 0, 0xDF06u);   /* Thumb swi 6（Halt） */
    bus_write16(nds->bus, base + 2, 0xE7FEu);
    a7_write32(nds, 0x03FFFFFCu, handler);      /* 用户 IRQ handler 槽 */
    a7_write32(nds, 0x03FFFFF8u, 0);            /* 软件中断标志字 */
    nds->io->irq[1].ime = 1;
    nds->io->irq[1].ie = 0;
    nds->io->irq[1].ifl = 0;
    power_halt_wake(&nds->io->power);
    cpu7->cpsr = ARM_MODE_SYS | CPSR_T;         /* I=0：Halt 期间允许 IRQ */
    cpu7->r[0] = 0x1234u;
    cpu7->r[1] = 0x5678u;
    cpu7->r[12] = 0x9ABCu;
    cpu7->r13_sys = sp_sys; cpu7->r[13] = sp_sys;
    cpu7->r13_bank[1] = sp_irq;
    cpu7->r13_bank[2] = sp_svc;
    cpu_reset(cpu7, base);

    exec_set_trace(0);
    for (int i = 0; i < 32 && cpu7->r[15] != 0x00001158u; i++)
        cpu_step(cpu7);
    CHECK_EQ("a7 halt pc", cpu7->r[15], 0x00001158u);
    CHECK_EQ("a7 halt pending", power_halt_pending(&nds->io->power), 1);
    CHECK_EQ("a7 halt mode sys", cpu7->cpsr & CPSR_MODE_MASK, ARM_MODE_SYS);

    cpu_step(cpu7);                             /* 无中断：保持暂停、不耗周期 */
    CHECK_EQ("a7 halt wait pc", cpu7->r[15], 0x00001158u);
    CHECK_EQ("a7 halt wait cost", cpu7->step_cycles, 0u);

    nds->io->irq[1].ie = IO_IF_VBLANK;
    nds->io->irq[1].ifl = IO_IF_VBLANK;
    cpu_step(cpu7);                             /* 唤醒：IRQ 先于 BIOS 收尾 */
    CHECK_EQ("a7 halt wake pc", cpu7->r[15], 0x00000018u);
    CHECK_EQ("a7 halt wake mode", cpu7->cpsr & CPSR_MODE_MASK, ARM_MODE_IRQ);
    CHECK_EQ("a7 halt irq lr", cpu7->r[14], 0x0000115Cu); /* 打断的是 0x1158 */
    CHECK_EQ("a7 halt flag cleared", power_halt_pending(&nds->io->power), 0);

    cpu_step(cpu7);                             /* 向量 → 0x1FB0 */
    CHECK_EQ("a7 irq handler pc", cpu7->r[15], 0x00001FB0u);
    cpu_step(cpu7);                             /* 压六字帧 */
    CHECK_EQ("a7 irq push pc", cpu7->r[15], 0x00001FB4u);
    /* IRQ 模式下可见 r13 就是 IRQ 栈指针（bank 值只在切模式时同步） */
    CHECK_EQ("a7 irq sp -0x18", cpu7->r[13], sp_irq - 0x18u);
    CHECK_EQ("a7 irq frame r0", a7_read32(nds, sp_irq - 0x18u), 0x04000000u);
    CHECK_EQ("a7 irq frame r12", a7_read32(nds, sp_irq - 8u), 6u);
    CHECK_EQ("a7 irq frame lr", a7_read32(nds, sp_irq - 4u), 0x0000115Cu);
    cpu_step(cpu7);                             /* mov r0,#0x04000000 */
    cpu_step(cpu7);                             /* mov lr,pc → 0x1FC0 */
    CHECK_EQ("a7 irq ret stub", cpu7->r[14], 0x00001FC0u);
    cpu_step(cpu7);                             /* ldr pc,[0x03FFFFFC] */
    CHECK_EQ("a7 irq to handler", cpu7->r[15], handler);

    cpu7->r[15] = 0x00001FC0u;                  /* 模拟 handler 弹回 BIOS 桩 */
    cpu_step(cpu7);
    CHECK_EQ("a7 irq pop pc", cpu7->r[15], 0x00001FC4u);
    cpu_step(cpu7);                             /* subs pc,lr,#4 → 0x1158 */
    CHECK_EQ("a7 irq ret pc", cpu7->r[15], 0x00001158u);
    CHECK_EQ("a7 irq ret mode", cpu7->cpsr & CPSR_MODE_MASK, ARM_MODE_SYS);
    CHECK_EQ("a7 irq ret sp_sys", cpu7->r[13], sp_sys - 4u); /* lr 还压在 System 栈 */

    nds->io->irq[1].ie = 0;
    nds->io->irq[1].ifl = 0;
    for (int i = 0; i < 32 && cpu7->r[15] != base + 2; i++)
        cpu_step(cpu7);
    CHECK_EQ("a7 halt swi done", cpu7->r[15], base + 2);
    CHECK_EQ("a7 halt swi cpsr", cpu7->cpsr, ARM_MODE_SYS | CPSR_T);
    exec_set_trace(1);
}

/* ---- 21-B9wu 用例：DMA 搬运期间按属主核分流 IO ----
   真机 0x04000400 在 ARM9 视角是 GX 命令 FIFO、在 ARM7 视角是音频通道寄存器。
   本地旧实现搬运时不切 bus->active_is_arm7，若上一步恰好是 ARM7，ARM9 的显示
   列表 DMA 会被当成音频写丢掉（3D 画面全黑）。 */
static void test_dma_owner_routing(nds_t *nds)
{
    const uint32_t src = 0x02004000u;
    const uint32_t dma0 = IO_DMA0_BASE;

    /* 两条 GX 命令字：MTX_MODE(0x10)+参数、MTX_IDENTITY(0x15) */
    bus_write32(nds->bus, src + 0, 0x00000010u);
    bus_write32(nds->bus, src + 4, 0x00000002u);   /* 参数：position 矩阵 */
    bus_write32(nds->bus, src + 8, 0x00000015u);
    bus_write32(nds->bus, src + 12, 0x00000000u);
    nds->io->gx.mt_mode = 0xAAu;                   /* 记号值 */
    uint32_t cmds_before = nds->io->gx.cmd_count;

    /* ARM9 的 DMA0：源=RAM、目的=GXFIFO、VBlank 触发、4 字 */
    nds->bus->active_is_arm7 = 0;
    bus_write32(nds->bus, dma0 + 0, src);
    bus_write32(nds->bus, dma0 + 4, 0x04000400u);
    bus_write16(nds->bus, dma0 + 8, 4u);
    bus_write16(nds->bus, dma0 + 10,
                DMA_CNT_32BIT | (DMA_START_VBLANK << DMA_CNT_MODE_SHIFT)
                | DMA_CNT_ENABLE);

    /* 关键：搬运前把「当前访问者」污染成 ARM7（模拟上一步来自 ARM7） */
    nds->bus->active_is_arm7 = 1;
    dma_fire(&nds->io->dma[0], nds->bus, DMA_START_VBLANK, 0);
    CHECK_EQ("dma gx cmds", nds->io->gx.cmd_count - cmds_before, 2u);
    /* 21-B9yi(续24)：GX 命令改为「按命令成本消费」（不立即执行），
       测试里显式推进一次引擎时钟。 */
    gx_advance(&nds->io->gx, 100000u);
    CHECK_EQ("dma gx mt_mode", nds->io->gx.mt_mode, 2u);
    CHECK_EQ("dma restores core flag", nds->bus->active_is_arm7, 1); /* 原样恢复 */
    nds->bus->active_is_arm7 = 0;
}

/* ---- 21-B9wu 用例：无头模式按帧推进 SPU，单发通道到末尾清 start 位 ----
   FFXII 开场 ARM7 会轮询声音通道的播放状态；无头模式没有 SDL 回调推进
  混音器，旧实现里通道永远“播不完”，游戏就卡在轮询里。 */
/* ---- 21-B9wx 用例：NDS RTC 串行协议（0x04000138）----
   协议：bit2=片选、bit1=时钟、bit0=数据、bit4=方向（1=主机写、0=主机读）；
   片选拉高后每个“时钟=0”的写入触发一次采样/移出，低位先出。 */
static void rtc_send_byte(nds_t *nds, uint8_t val)
{
    for (int i = 0; i < 8; i++) {
        uint8_t bit = (uint8_t)((val >> i) & 1u);
        io_write8(nds->io, 0x04000138u, (uint8_t)(0x14u | bit), 1); /* 写方向+数据, 时钟低 */
        io_write8(nds->io, 0x04000138u, (uint8_t)(0x16u | bit), 1); /* 时钟高 */
    }
}

static uint8_t rtc_recv_byte(nds_t *nds)
{
    uint8_t out = 0;
    for (int i = 0; i < 8; i++) {
        io_write8(nds->io, 0x04000138u, 0x04u, 1);                  /* 读方向, 时钟低 */
        uint8_t v = io_read8(nds->io, 0x04000138u, 1);
        if (v & 1u) out |= (uint8_t)(1u << i);
        io_write8(nds->io, 0x04000138u, 0x06u, 1);                  /* 时钟高 */
    }
    return out;
}

static void test_rtc(nds_t *nds)
{
    /* 写状态寄存器 1：命令 0x06（写）、数据 0x02（24 小时制） */
    io_write8(nds->io, 0x04000138u, 0x04u, 1);      /* 片选 */
    rtc_send_byte(nds, 0x06u);
    rtc_send_byte(nds, 0x02u);
    io_write8(nds->io, 0x04000138u, 0x00u, 1);      /* 撤片选 */

    /* 读状态寄存器 1：命令 0x61（真机高半字节按位反转 → 0x86 = 读状态 1） */
    io_write8(nds->io, 0x04000138u, 0x04u, 1);
    rtc_send_byte(nds, 0x61u);
    uint8_t st = rtc_recv_byte(nds);
    io_write8(nds->io, 0x04000138u, 0x00u, 1);
    CHECK_EQ("rtc status1 24h", st & 0x02u, 0x02u);

    /* 读日期时间：真机字节 0x65 → 反转表 → 命令 0xA6 = 读 7 字节
       （年/月/日/星期/时/分/秒，BCD）；bit7=1 才是读命令。 */
    io_write8(nds->io, 0x04000138u, 0x04u, 1);
    rtc_send_byte(nds, 0x65u);
    uint8_t t[7];
    for (int i = 0; i < 7; i++) t[i] = rtc_recv_byte(nds);
    io_write8(nds->io, 0x04000138u, 0x00u, 1);
    CHECK_EQ("rtc datetime year bcd", (t[0] >> 4) & 0xF, 2u);
    CHECK_EQ("rtc datetime month bcd", t[1], 0x09u);
    CHECK_EQ("rtc datetime day bcd", t[2], 0x12u);

    /* RCnt（0x04000134）按字节可读写 */
    io_write8(nds->io, 0x04000134u, 0x5Au, 1);
    io_write8(nds->io, 0x04000135u, 0xA5u, 1);
    CHECK_EQ("rcnt write/read", io_read8(nds->io, 0x04000134u, 1), 0x5Au);
    CHECK_EQ("rcnt high byte", io_read8(nds->io, 0x04000135u, 1), 0xA5u);
}

/* ---- 21-B9xi 用例：ARM7 BIOS 保护值 + SOUNDBIAS（对照参考核帧 1500+）----
   参考核：ARM7IORead16(0x04000308) = ARM7BIOSProt（直启 0x1204），
   地址只对 ARM7 存在（NDS9 侧未映射）；SOUNDBIAS(0x04000504) 是 10 位寄存器，
   真机上电 0x200，melonDS 写半字时整体覆盖（Bias = val & 0x3FF）。 */
static void test_arm7_biosprot_soundbias(nds_t *nds)
{
    /* BIOS 保护值：本地直启 = 0x1204（低字节 0x04 在 0x308、高字节 0x12 在 0x309） */
    CHECK_EQ("biosprot7 low", io_read8(nds->io, 0x04000308u, 1), 0x04u);
    CHECK_EQ("biosprot7 high", io_read8(nds->io, 0x04000309u, 1), 0x12u);
    CHECK_EQ("biosprot9 unmapped", io_read8(nds->io, 0x04000308u, 0), 0x00u);
    nds->bus->active_is_arm7 = 1;
    CHECK_EQ("biosprot7 bus16", bus_read16(nds->bus, 0x04000308u), 0x1204u);
    /* 已非 0：ARM7 再写不生效（melonDS：if (ARM7BIOSProt == 0) 才接受） */
    bus_write16(nds->bus, 0x04000308u, 0x0000u);
    CHECK_EQ("biosprot7 write keep", bus_read16(nds->bus, 0x04000308u), 0x1204u);
    nds->bus->active_is_arm7 = 0;
    CHECK_EQ("biosprot9 bus16", bus_read16(nds->bus, 0x04000308u), 0x0000u);

    /* SOUNDBIAS：上电 0x200 → 半字写整体覆盖 → 只有 bit0-9 有效 */
    nds->bus->active_is_arm7 = 1;
    CHECK_EQ("sndbias reset", bus_read16(nds->bus, 0x04000504u), 0x200u);
    bus_write16(nds->bus, 0x04000504u, 0x00A5u);
    CHECK_EQ("sndbias write16", bus_read16(nds->bus, 0x04000504u), 0x00A5u);
    bus_write16(nds->bus, 0x04000504u, 0xFFFFu);
    CHECK_EQ("sndbias 10bit mask", bus_read16(nds->bus, 0x04000504u), 0x03FFu);
    nds->bus->active_is_arm7 = 0;
}

/* ---- 21-B9xq 用例：GBA 扩展槽空槽读 0xFF（对齐参考核 open-bus）----
   FFXII 启动用 DMA1 从 0x08000080 取 0x40 字节填 0x02079920-5F；
   空槽若读 0，签名表会变成 0，进而让 0x027FFC30 与 ARM7 调度器闸门走偏。 */
static void test_gba_slot_open_bus(nds_t *nds)
{
    CHECK_EQ("gba slot read8", bus_read8(nds->bus, 0x08000080u), 0xFFu);
    CHECK_EQ("gba slot read16", bus_read16(nds->bus, 0x08000080u), 0xFFFFu);
    CHECK_EQ("gba sram read16", bus_read16(nds->bus, 0x0A000000u), 0xFFFFu);
    nds->bus->active_is_arm7 = 1;
    CHECK_EQ("gba slot arm7 read32", bus_read32(nds->bus, 0x0B000000u), 0xFFFFFFFFu);
    nds->bus->active_is_arm7 = 0;
}

/* ---- 21-B9xj 用例：DISPSTAT 每核一套（ARM7 不再污染 ARM9）----
   参考核：ARM9IORead16(0x04000004)=DispStat[0]、ARM7IORead16(0x04000004)=
   DispStat[1]；只读位 0-2/6，VCount 比较值 = bit8-15 | (bit7<<8)，
   匹配是边沿触发，且只有使能位（bit5）才挂 IF bit2。 */
static void test_dispstat_per_core(nds_t *nds)
{
    /* ARM7 写自己的：VCount=0x20 + VBlank IRQ 使能（bit3） */
    nds->bus->active_is_arm7 = 1;
    bus_write16(nds->bus, 0x04000004u, 0x2008u);
    CHECK_EQ("dispstat7 readback", bus_read16(nds->bus, 0x04000004u), 0x2008u);
    nds->bus->active_is_arm7 = 0;
    CHECK_EQ("dispstat9 untouched", bus_read16(nds->bus, 0x04000004u), 0x0000u);

    /* ARM9 写自己的：VCount=0x20 + VCount IRQ 使能（bit5） */
    bus_write16(nds->bus, 0x04000004u, 0x2020u);
    CHECK_EQ("dispstat9 write", bus_read16(nds->bus, 0x04000004u), 0x2020u);
    nds->bus->active_is_arm7 = 1;
    CHECK_EQ("dispstat7 still", bus_read16(nds->bus, 0x04000004u), 0x2008u);
    nds->bus->active_is_arm7 = 0;

    /* 只读位（0-2/6）写不进去：写全 1 后低字节只剩可写位（0xB8） */
    bus_write16(nds->bus, 0x04000004u, 0xFFFFu);
    CHECK_EQ("dispstat ro bits", bus_read16(nds->bus, 0x04000004u) & 0x0047u,
             0x0000u);
    /* 复位成 VCount=0x20 + VCount IRQ 使能（bit5）后再验匹配 */
    bus_write16(nds->bus, 0x04000004u, 0x2020u);

    /* VCount 匹配：扫描线 0x20 命中两核各自的 VMatch */
    nds->io->irq[0].ifl = 0;
    nds->io->irq[1].ifl = 0;
    nds->io->vcount = 0x1Fu;
    io_advance_scanline(nds->io);
    CHECK_EQ("vcount match flag9", nds->io->disp.dispstat & 4u, 4u);
    CHECK_EQ("vcount match irq9", nds->io->irq[0].ifl & 4u, 4u);
    /* ARM7 的 VMatch 也是 0x20，但它没使能 bit5 → 只置标志、不挂中断 */
    CHECK_EQ("vcount match flag7", nds->io->disp.dispstat7 & 4u, 4u);
    CHECK_EQ("vcount match irq7", nds->io->irq[1].ifl & 4u, 0u);
}

/* ---- 21-B9wy 用例：GXSTAT 的 FIFO 状态位（bit25/26）----
   参考核 GPU3D::Read32(0x04000600) 在 FIFO 空时同时置 bit25（不足半满）与
   bit26（空）；游戏常靠 bit25 判断“还能不能往 FIFO 塞命令”。本地旧实现只有
   bit26，GXSTAT 读数和参考不一致（0x84000000 vs 0x86000000）。 */
static void test_gxstat_fifo_bits(nds_t *nds)
{
    uint32_t v = bus_read32(nds->bus, GX_GXSTAT);
    CHECK_EQ("gxstat less-half", v & GXSTAT_FIFO_LESS_HALF, GXSTAT_FIFO_LESS_HALF);
    CHECK_EQ("gxstat empty", v & GXSTAT_FIFO_EMPTY, GXSTAT_FIFO_EMPTY);

    bus_write8(nds->bus, GX_GXSTAT + 3u, 0x80u);   /* FIFO IRQ 模式 = 2 */
    v = bus_read32(nds->bus, GX_GXSTAT);
    CHECK_EQ("gxstat irq mode", v & GXSTAT_IRQ_MODE, 0x80000000u);
    CHECK_EQ("gxstat fifo bits kept", v & 0x06000000u, 0x06000000u);
}

/* ---- 21-B9xc 用例：VBlank 时序（第 192 行开始、帧边界结束）----
   真机一帧 263 行，第 192-262 行为 VBlank：IRQ 与 DISPSTAT bit0 都在第 192 行
   置位，到帧边界（VCOUNT 回 0）清除。旧实现把 VBlank 挂在帧边界上，
   游戏每帧的工作窗口整体错位。 */
static void test_vblank_timing(nds_t *nds)
{
    nds->io->irq[0].ifl = 0;
    nds->io->irq[1].ifl = 0;
    nds->io->disp.dispstat = 0;
    nds->io->disp.dispstat_sub = 0;

    io_set_vblank(nds->io);
    CHECK_EQ("vblank irq9", nds->io->irq[0].ifl & IO_IF_VBLANK, IO_IF_VBLANK);
    CHECK_EQ("vblank irq7", nds->io->irq[1].ifl & IO_IF_VBLANK, IO_IF_VBLANK);
    CHECK_EQ("vblank flag main", nds->io->disp.dispstat & 1u, 1u);
    CHECK_EQ("vblank flag sub", nds->io->disp.dispstat_sub & 1u, 1u);

    nds->io->vcount = 262u;
    io_frame_boundary(nds->io);
    CHECK_EQ("frame boundary vcount", nds->io->vcount, 0u);
    CHECK_EQ("frame boundary flag cleared", nds->io->disp.dispstat & 1u, 0u);
}

/* ---- 21-B9ww 用例：ARM9 DMA 模式 7 = GX 命令 FIFO（显示列表 DMA）----
   真机显示列表不靠 CPU 逐字写端口，而是把主存里的列表用 DMA 送到
   0x04000400（目的地址固定）。本地旧实现只认立即模式与 VBlank/卡带触发，
   模式 7 的搬运从不发生 → GX 永远收不到几何命令（3D 画面全黑）。 */
static void test_dma_gx_fifo_mode(nds_t *nds)
{
    const uint32_t src = 0x02006000u;
    const uint32_t dma0 = IO_DMA0_BASE;

    bus_write32(nds->bus, src + 0, 0x00000010u);   /* MTX_MODE */
    bus_write32(nds->bus, src + 4, 0x00000002u);   /* 参数：position */
    bus_write32(nds->bus, src + 8, 0x00000015u);   /* MTX_IDENTITY */
    bus_write32(nds->bus, src + 12, 0x00000000u);
    nds->bus->active_is_arm7 = 0;
    uint32_t before = nds->io->gx.cmd_count;

    bus_write32(nds->bus, dma0 + 0, src);
    bus_write32(nds->bus, dma0 + 4, 0x04000400u);  /* GXFIFO */
    bus_write16(nds->bus, dma0 + 8, 4u);
    bus_write16(nds->bus, dma0 + 10,
                DMA_CNT_32BIT | DMA_CNT_DST_FIX
                | (DMA_START_GXFIFO << DMA_CNT_MODE_SHIFT) | DMA_CNT_ENABLE);

    /* 21-B9yi(续27)：GX-FIFO DMA 现在**每次只推一个字**（按 melonDS CmdPIPE 112 条
       条目的容量节流），由引擎消费 FIFO 后经 `dma_gx_resume()` 续推。
       测试按游戏环境的方式泵：推进引擎 + 续跑 DMA，直到搬运完成。 */
    for (int spin = 0; spin < 200 &&
         (bus_read16(nds->bus, dma0 + 10) & DMA_CNT_ENABLE) != 0u; spin++) {
        gx_advance(&nds->io->gx, 4000u);
        dma_gx_resume(&nds->io->dma[0], nds->bus, 0);
    }
    CHECK_EQ("gxfifo dma cmds", nds->io->gx.cmd_count - before, 2u);
    CHECK_EQ("gxfifo dma mt_mode", nds->io->gx.mt_mode, 2u);
    CHECK_EQ("gxfifo dma enable cleared",
             bus_read16(nds->bus, dma0 + 10) & DMA_CNT_ENABLE, 0u);
}

static void test_snd_advance_headless(nds_t *nds)
{
    const uint32_t ch0 = SND_BASE;
    uint8_t wave[32];
    for (size_t i = 0; i < sizeof wave; i++)
        wave[i] = (uint8_t)(0x80 + i);
    for (size_t i = 0; i < sizeof wave; i++)
        bus_write8(nds->bus, 0x02005000u + (uint32_t)i, wave[i]);

    nds->bus->active_is_arm7 = 1;                  /* 声音寄存器走 ARM7 视角 */
    bus_write16(nds->bus, SND_SOUNDCNT, 0x8000u);  /* SOUNDCNT bit15：主输出开 */
    bus_write32(nds->bus, ch0 + 0x00, 0x02005000u);/* SOUND0SAD */
    bus_write16(nds->bus, ch0 + 0x04, 0);          /* SOUND0TMR=0 → 65536 */
    bus_write16(nds->bus, ch0 + 0x08, 0);          /* SOUND0PNT */
    bus_write32(nds->bus, ch0 + 0x0C, 4u);         /* SOUND0LEN = 4 单元 */
    bus_write32(nds->bus, ch0 + 0x00, 0x02005000u);
    uint32_t cnt = (uint32_t)SND_FORMAT_PCM8 << SNDCNT_FORMAT_SHIFT;
    cnt |= (uint32_t)SND_REPEAT_ONESHOT << SNDCNT_REPEAT_SHIFT;
    cnt |= 0x7Fu;                                  /* 音量 127 */
    cnt |= 64u << SNDCNT_PAN_SHIFT;
    cnt |= SNDCNT_START;
    bus_write32(nds->bus, ch0 + 0x00, 0x02005000u);/* 重新写 SAD（写 CNT 前） */
    nds->io->snd.ch[0].sad = 0x02005000u;
    nds->io->snd.ch[0].cnt = cnt;
    CHECK_EQ("snd starts busy", (nds->io->snd.ch[0].cnt & SNDCNT_START) != 0, 1);

    /* 一帧 ≈ 547 样本；4 个 PCM8 单元 = 16 字节 ≈ 1024 样本 → 两帧内播完 */
    snd_advance(&nds->io->snd, nds->bus, 2048);
    CHECK_EQ("snd one-shot cleared start",
             (nds->io->snd.ch[0].cnt & SNDCNT_START) != 0, 0);
    nds->bus->active_is_arm7 = 0;
}

/* ---- 11.7 用例：综合（LZ77 解压到 VRAM + Div + Sqrt 串行） ---- */
static void test_stage11_integration(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE;
    uint32_t src = 0x02001000u;
    uint32_t dst = BUS_VRAM_BASE;

    /* LZ77 压缩数据：解压出 "ABCABCABC" */
    bus_write32(nds->bus, src + 0, 0x00000910u);
    bus_write8(nds->bus, src + 4, 0x1C);
    bus_write8(nds->bus, src + 5, 'A');
    bus_write8(nds->bus, src + 6, 'B');
    bus_write8(nds->bus, src + 7, 'C');
    bus_write16(nds->bus, src + 8, 0x0200);
    bus_write16(nds->bus, src + 10, 0x0200);
    bus_write16(nds->bus, src + 12, 0x0200);

    static const uint32_t prog[] = {
        /* 0x00 */ 0xEF110000, /* SWI 0x11 (LZ77)  r0=src, r1=VRAM 预置 */
        /* 0x04 */ 0xE3A0007B, /* MOV r0, #123 */
        /* 0x08 */ 0xE3A0100A, /* MOV r1, #10 */
        /* 0x0C */ 0xEF090000, /* SWI 0x09 (Div)  r0=12, r1=3, r3=12 */
        /* 0x10 */ 0xE3A00064, /* MOV r0, #100 */
        /* 0x14 */ 0xEF0D0000, /* SWI 0x0D (Sqrt) r0=10 */
        /* 0x18 */ 0xEAFFFFFE, /* B self */
    };
    nds->cpu->r[0] = src;
    nds->cpu->r[1] = dst;
    run_program(nds, base, prog, sizeof prog / sizeof prog[0],
                base, base + 0x18, 64);

    CHECK_EQ("itg11 lz77[0]", bus_read8(nds->bus, dst + 0), (uint8_t)'A');
    CHECK_EQ("itg11 lz77[8]", bus_read8(nds->bus, dst + 8), (uint8_t)'C');
    CHECK_EQ("itg11 div r1", nds->cpu->r[1], 3u);
    CHECK_EQ("itg11 div r3", nds->cpu->r[3], 12u);
    CHECK_EQ("itg11 sqrt r0", nds->cpu->r[0], 10u);
}

/* ---- 12.2 用例：未定义指令 → 未定义异常向量 0x04 ---- */
static void test_exception_vector_undef(nds_t *nds)
{
    arm_cpu_t *cpu = nds->cpu;
    const uint32_t base = BUS_MAIN_RAM_BASE;
    cpu->vector_base = 0x02004000u;              /* 向量表放 Main RAM，便于装 handler */
    bus_write32(nds->bus, cpu->vector_base + EXC_UNDEF_OFF, 0xEAFFFFFE); /* B self */

    cpu->cpsr = 0x00000013u;                     /* SVC、I=0，作为待保存的 CPSR */
    cpu->r[15] = base;
    bus_write32(nds->bus, base, 0xEC000000u);    /* LDC 类：模拟器未实现 → 未定义异常 */
    exec_set_trace(0);
    cpu_step(cpu);
    exec_set_trace(1);

    CHECK_EQ("undef PC=vec+0x04", cpu->r[15], cpu->vector_base + EXC_UNDEF_OFF);
    CHECK_EQ("undef mode=UND", cpu->cpsr & CPSR_MODE_MASK, ARM_MODE_UND);
    CHECK_EQ("undef spsr saved", cpu->spsr[exec_spsr_index(ARM_MODE_UND)], 0x00000013u);
    CHECK_EQ("undef lr=pc+4", cpu->r[14], base + 4);
}

/* ---- 12.2 用例：未知 SWI → SWI 异常向量 0x08 ---- */
static void test_exception_vector_swi(nds_t *nds)
{
    arm_cpu_t *cpu = nds->cpu;
    const uint32_t base = BUS_MAIN_RAM_BASE;
    cpu->vector_base = 0x02004000u;
    bus_write32(nds->bus, cpu->vector_base + EXC_SWI_OFF, 0xEAFFFFFE); /* B self */

    cpu->cpsr = 0x00000010u;                     /* User 模式，验证切到 SVC */
    cpu->r[15] = base;
    bus_write32(nds->bus, base, 0xEF000000u);    /* SWI 0（SoftReset，未知号） */
    exec_set_trace(0);
    cpu_step(cpu);
    exec_set_trace(1);

    CHECK_EQ("swi PC=vec+0x08", cpu->r[15], cpu->vector_base + EXC_SWI_OFF);
    CHECK_EQ("swi mode=SVC", cpu->cpsr & CPSR_MODE_MASK, ARM_MODE_SVC);
    CHECK_EQ("swi spsr saved", cpu->spsr[exec_spsr_index(ARM_MODE_SVC)], 0x00000010u);
    CHECK_EQ("swi lr=pc+4", cpu->r[14], base + 4);
    CHECK_EQ("swi num recorded", cpu->swi_num, 0u);
}

/* ---- 12.3 用例：LDM sp!, {pc}^ 用 SPSR 恢复 CPSR（异常返回） ---- */
static void test_spsr_restore(nds_t *nds)
{
    arm_cpu_t *cpu = nds->cpu;
    const uint32_t base = BUS_MAIN_RAM_BASE;
    const uint32_t stack = 0x02001000u;
    bus_write32(nds->bus, stack, base + 0x20);   /* 栈顶 = 返回地址 */

    static const uint32_t prog[] = {
        /* 0x00 */ 0xE8FD8000, /* LDMIA sp!, {pc}^：加载 PC + SPSR→CPSR */
        /* 0x04 */ 0xEAFFFFFE, /* B self（不应执行到） */
    };
    cpu->cpsr = ARM_MODE_IRQ;
    cpu->spsr[exec_spsr_index(ARM_MODE_IRQ)] = 0x60000010u; /* N,Z 置位 + User 模式 */
    cpu->r[13] = stack;
    run_program(nds, base, prog, 2, base, base + 0x20, 8);

    CHECK_EQ("spsr restore PC", cpu->r[15], base + 0x20);
    CHECK_EQ("spsr restore CPSR", cpu->cpsr, 0x60000010u);
}

/* ---- 12.4 用例：CP15 c1 的 V 位联动异常向量基址 ---- */
static void test_cp15_control(nds_t *nds)
{
    arm_cpu_t *cpu = nds->cpu;
    const uint32_t base = BUS_MAIN_RAM_BASE;

    /* 写 c1 = 0（V=0）→ vector_base 变低 0x00000000 */
    static const uint32_t prog_low[] = {
        /* 0x00 */ 0xE3A00000, /* MOV r0, #0 */
        /* 0x04 */ 0xEE010F10, /* MCR p15, c1 ← r0 */
        /* 0x08 */ 0xEAFFFFFE, /* B self */
    };
    cpu->vector_base = 0xFFFF0000u;
    run_program(nds, base, prog_low, 3, base, base + 0x08, 16);
    CHECK_EQ("cp15 c1=0", cpu->cp15[1], 0u);
    CHECK_EQ("cp15 vector low", cpu->vector_base, 0x00000000u);

    /* 写 c1 = 0x2000（V=1）→ vector_base 变高 0xFFFF0000 */
    static const uint32_t prog_high[] = {
        /* 0x00 */ 0xE3A00C20, /* MOV r0, #0x2000 */
        /* 0x04 */ 0xEE010F10, /* MCR p15, c1 ← r0 */
        /* 0x08 */ 0xEE111F10, /* MRC p15, r1 ← c1 */
        /* 0x0C */ 0xEAFFFFFE, /* B self */
    };
    run_program(nds, base, prog_high, 4, base, base + 0x0C, 16);
    CHECK_EQ("cp15 c1=0x2000", cpu->cp15[1], 0x2000u);
    CHECK_EQ("cp15 mrc r1", cpu->r[1], 0x2000u);
    CHECK_EQ("cp15 vector high", cpu->vector_base, 0xFFFF0000u);
}

/* ---- 12.5 用例：IRQ 真实响应：进 handler、SUBS pc,lr,#4 返回 ---- */
static void test_irq_response(nds_t *nds)
{
    arm_cpu_t *cpu = nds->cpu;
    const uint32_t base = BUS_MAIN_RAM_BASE;
    cpu->vector_base = 0x02004000u;
    /* IRQ handler：SUBS pc, lr, #4（返回被打断处并恢复 CPSR） */
    bus_write32(nds->bus, cpu->vector_base + EXC_IRQ_OFF, 0xE25EF004);

    static const uint32_t prog[] = {
        /* 0x00 */ 0xE3A00005, /* MOV r0, #5 */
        /* 0x04 */ 0xE3A01007, /* MOV r1, #7 */
        /* 0x08 */ 0xEAFFFFFE, /* B self */
    };
    bus_write32(nds->bus, base + 0, prog[0]);
    bus_write32(nds->bus, base + 4, prog[1]);
    bus_write32(nds->bus, base + 8, prog[2]);

    cpu->cpsr = ARM_MODE_USER;                  /* I=0 允许 IRQ */
    nds->io->irq[0].ie  = IO_IF_VBLANK;
    nds->io->irq[0].ifl = IO_IF_VBLANK;
    nds->io->irq[0].ime = 1;

    cpu_reset(cpu, base);
    exec_set_trace(0);
    cpu_step(cpu);   /* 第一条：IRQ 触发 → 进 handler（PC=vec+0x18） */
    exec_set_trace(1);

    CHECK_EQ("irq PC=vec+0x18", cpu->r[15], cpu->vector_base + EXC_IRQ_OFF);
    CHECK_EQ("irq mode=IRQ", cpu->cpsr & CPSR_MODE_MASK, ARM_MODE_IRQ);
    CHECK_EQ("irq I set", (cpu->cpsr & CPSR_I) ? 1u : 0u, 1u);
    CHECK_EQ("irq spsr saved", cpu->spsr[exec_spsr_index(ARM_MODE_IRQ)], ARM_MODE_USER);
    CHECK_EQ("irq lr=pc+4", cpu->r[14], base + 4);

    exec_set_trace(0);
    cpu_step(cpu);   /* 第二条：SUBS pc, lr, #4 → 返回 base，恢复 CPSR */
    exec_set_trace(1);
    CHECK_EQ("irq return PC", cpu->r[15], base);
    CHECK_EQ("irq return mode", cpu->cpsr & CPSR_MODE_MASK, ARM_MODE_USER);
    CHECK_EQ("irq return I", (cpu->cpsr & CPSR_I) ? 1u : 0u, 0u);
}

/* ---- 阶段 13.3 数据处理（移位/立即数/ALU/高寄存器） ---- */
static void test_thumb_dataproc(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE;
    static const uint16_t prog[] = {
        0x200F, /* MOV r0, #15       → r0=15 */
        0x210C, /* MOV r1, #12       → r1=12 */
        0x2206, /* MOV r2, #6        → r2=6 */
        0x230F, /* MOV r3, #15 */
        0x400B, /* AND r3, r1        → r3 = 15 & 12 = 12 */
        0x240F, /* MOV r4, #15 */
        0x404C, /* EOR r4, r1        → r4 = 15 ^ 12 = 3 */
        0x2500, /* MOV r5, #0 */
        0x4305, /* ORR r5, r0        → r5 = 0 | 15 = 15 */
        0x260F, /* MOV r6, #15 */
        0x434E, /* MUL r6, r1        → r6 = 15 * 12 = 180 */
        0x270F, /* MOV r7, #15 */
        0x4247, /* NEG r7, r0        → r7 = -15 */
        0x4688, /* MOV r8, r1        → r8 = 12（高寄存器 MOV） */
        0x4480, /* ADD r8, r0        → r8 = 12 + 15 = 27（高寄存器 ADD） */
    };
    thumb_write(nds, base, prog, sizeof prog / sizeof prog[0]);
    thumb_start(nds, base);
    for (size_t i = 0; i < sizeof prog / sizeof prog[0]; i++)
        cpu_step(nds->cpu);
    thumb_stop(nds);

    CHECK_EQ("thumb mov r0", nds->cpu->r[0], 15);
    CHECK_EQ("thumb mov r1", nds->cpu->r[1], 12);
    CHECK_EQ("thumb mov r2", nds->cpu->r[2], 6);
    CHECK_EQ("thumb AND", nds->cpu->r[3], 12);
    CHECK_EQ("thumb EOR", nds->cpu->r[4], 3);
    CHECK_EQ("thumb ORR", nds->cpu->r[5], 15);
    CHECK_EQ("thumb MUL", nds->cpu->r[6], 180);
    CHECK_EQ("thumb NEG", nds->cpu->r[7], 0xFFFFFFF1u);
    CHECK_EQ("thumb hi-ADD", nds->cpu->r[8], 27);
    CHECK_EQ("thumb PC", nds->cpu->r[15], base + 2 * 15);
}

/* ---- 阶段 13.4 访存（字/字节/半字/SP 相对/寄存器偏移/字面量池） ---- */
static void test_thumb_memory(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE;
    const uint32_t data = base + 0x100;
    arm_cpu_t *cpu = nds->cpu;

    /* STR 字 */
    bus_write32(nds->bus, data, 0);
    cpu->r[0] = data; cpu->r[1] = 0x12345678u;
    thumb_start(nds, base); bus_write16(nds->bus, base, 0x6001); cpu_step(cpu);
    CHECK_EQ("thumb STR word", bus_read32(nds->bus, data), 0x12345678u);
    thumb_stop(nds);

    /* LDR 字（#imm5*4 = 4） */
    bus_write32(nds->bus, data + 4, 0xDEADBEEFu);
    cpu->r[0] = data; cpu->r[2] = 0;
    thumb_start(nds, base); bus_write16(nds->bus, base, 0x6842); cpu_step(cpu);
    CHECK_EQ("thumb LDR word", cpu->r[2], 0xDEADBEEFu);
    thumb_stop(nds);

    /* STRB 字节（#imm5 = 1） */
    bus_write8(nds->bus, data + 1, 0);
    cpu->r[0] = data; cpu->r[1] = 0xAB;
    thumb_start(nds, base); bus_write16(nds->bus, base, 0x7041); cpu_step(cpu);
    CHECK_EQ("thumb STRB", bus_read8(nds->bus, data + 1), 0xAB);
    thumb_stop(nds);

    /* LDRB 字节（无符号） */
    bus_write8(nds->bus, data + 1, 0xCD);
    cpu->r[0] = data; cpu->r[2] = 0;
    thumb_start(nds, base); bus_write16(nds->bus, base, 0x7842); cpu_step(cpu);
    CHECK_EQ("thumb LDRB", cpu->r[2], 0xCD);
    thumb_stop(nds);

    /* STRH 半字（#imm5*2 = 2） */
    bus_write16(nds->bus, data + 2, 0);
    cpu->r[0] = data; cpu->r[1] = 0xBEEF;
    thumb_start(nds, base); bus_write16(nds->bus, base, 0x8041); cpu_step(cpu);
    CHECK_EQ("thumb STRH", bus_read16(nds->bus, data + 2), 0xBEEF);
    thumb_stop(nds);

    /* LDRH 半字 */
    bus_write16(nds->bus, data + 2, 0xCAFE);
    cpu->r[0] = data; cpu->r[2] = 0;
    thumb_start(nds, base); bus_write16(nds->bus, base, 0x8842); cpu_step(cpu);
    CHECK_EQ("thumb LDRH", cpu->r[2], 0xCAFE);
    thumb_stop(nds);

    /* STR 字（SP 相对，#imm8*4 = 4） */
    bus_write32(nds->bus, data + 4, 0);
    cpu->r[13] = data; cpu->r[1] = 0x13579BDFu;
    thumb_start(nds, base); bus_write16(nds->bus, base, 0x9101); cpu_step(cpu);
    CHECK_EQ("thumb STR SP-rel", bus_read32(nds->bus, data + 4), 0x13579BDFu);
    thumb_stop(nds);

    /* LDR 字（SP 相对） */
    bus_write32(nds->bus, data + 4, 0x02468ACEu);
    cpu->r[13] = data; cpu->r[2] = 0;
    thumb_start(nds, base); bus_write16(nds->bus, base, 0x9A01); cpu_step(cpu);
    CHECK_EQ("thumb LDR SP-rel", cpu->r[2], 0x02468ACEu);
    thumb_stop(nds);

    /* STR 字（寄存器偏移 r0+r2） */
    bus_write32(nds->bus, data + 8, 0);
    cpu->r[0] = data; cpu->r[1] = 0x11223344u; cpu->r[2] = 8;
    thumb_start(nds, base); bus_write16(nds->bus, base, 0x5081); cpu_step(cpu);
    CHECK_EQ("thumb STR reg-off", bus_read32(nds->bus, data + 8), 0x11223344u);
    thumb_stop(nds);

    /* LDR 字（寄存器偏移） */
    bus_write32(nds->bus, data + 8, 0x55667788u);
    cpu->r[0] = data; cpu->r[2] = 8; cpu->r[3] = 0;
    thumb_start(nds, base); bus_write16(nds->bus, base, 0x5883); cpu_step(cpu);
    CHECK_EQ("thumb LDR reg-off", cpu->r[3], 0x55667788u);
    thumb_stop(nds);

    /* LDR 字面量池：[PC,#0] 读 (pc&~3)+0 = base+4 */
    bus_write32(nds->bus, base + 4, 0x01020304u);
    cpu->r[4] = 0;
    thumb_start(nds, base); bus_write16(nds->bus, base, 0x4C00); cpu_step(cpu);
    CHECK_EQ("thumb LDR literal", cpu->r[4], 0x01020304u);
    thumb_stop(nds);
}

/* ---- 阶段 13.5 分支/切换/软中断 + PUSH/POP + STMIA/LDMIA ---- */
static void test_thumb_branch(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE;
    arm_cpu_t *cpu = nds->cpu;

    /* BX r0（偶地址）→ 切 ARM（T=0） */
    cpu->r[0] = base + 0x20;
    thumb_start(nds, base); bus_write16(nds->bus, base, 0x4700); cpu_step(cpu);
    CHECK_EQ("thumb BX even T=0", (cpu->cpsr & CPSR_T) ? 1u : 0u, 0u);
    CHECK_EQ("thumb BX even PC", cpu->r[15], base + 0x20);
    thumb_stop(nds);

    /* BX r1（奇地址）→ 保持 Thumb（T=1），PC = 目标 & ~1 */
    cpu->r[1] = (base + 0x20) | 1u;
    thumb_start(nds, base); bus_write16(nds->bus, base, 0x4708); cpu_step(cpu);
    CHECK_EQ("thumb BX odd T=1", (cpu->cpsr & CPSR_T) ? 1u : 0u, 1u);
    CHECK_EQ("thumb BX odd PC", cpu->r[15], base + 0x20);
    thumb_stop(nds);

    /* BX lr（0x4770，Rm=14）：高寄存器返回路径。21-B5 真 ROM 暴露——
       旧解码把 Rm 按 bit2:0+bit6 拼成 r8，导致 0x4770 跳到 r8 的 0x04000180。 */
    cpu->r[14] = base + 0x20;
    thumb_start(nds, base); bus_write16(nds->bus, base, 0x4770); cpu_step(cpu);
    CHECK_EQ("thumb BX lr T=0", (cpu->cpsr & CPSR_T) ? 1u : 0u, 0u);
    CHECK_EQ("thumb BX lr PC", cpu->r[15], base + 0x20);
    thumb_stop(nds);

    /* 条件分支 BEQ 命中：Z=1 → PC=base+8 */
    thumb_start(nds, base);
    cpu->cpsr |= CPSR_Z;
    bus_write16(nds->bus, base, 0xD002); cpu_step(cpu);
    CHECK_EQ("thumb BEQ taken", cpu->r[15], base + 8);
    thumb_stop(nds);

    /* 条件分支 BEQ 未命中：Z=0 → PC=base+2 */
    thumb_start(nds, base);
    cpu->cpsr &= ~CPSR_Z;
    bus_write16(nds->bus, base, 0xD002); cpu_step(cpu);
    CHECK_EQ("thumb BEQ not-taken", cpu->r[15], base + 2);
    thumb_stop(nds);

    /* 无条件分支 B +8：0xE004 → PC = base+4+8 = base+0x0C */
    thumb_start(nds, base); bus_write16(nds->bus, base, 0xE004); cpu_step(cpu);
    CHECK_EQ("thumb B", cpu->r[15], base + 0x0C);
    thumb_stop(nds);

    /* BL：第一半字 0xF000（off=0 → LR=base+4），第二半字 0xF80E（off=14 → PC=base+0x20） */
    static const uint16_t bl[] = { 0xF000, 0xF80E };
    thumb_write(nds, base, bl, 2);
    thumb_start(nds, base);
    cpu_step(cpu); cpu_step(cpu);
    CHECK_EQ("thumb BL PC", cpu->r[15], base + 0x20);
    CHECK_EQ("thumb BL LR", cpu->r[14], (base + 6) | 1u);
    CHECK_EQ("thumb BL T", (cpu->cpsr & CPSR_T) ? 1u : 0u, 1u);
    thumb_stop(nds);

    /* BLX：第一半字 0xF000，第二半字 0xE80E → PC=(LR+28)&~1=base+0x20，切 ARM */
    static const uint16_t blx[] = { 0xF000, 0xE80E };
    thumb_write(nds, base, blx, 2);
    thumb_start(nds, base);
    cpu_step(cpu); cpu_step(cpu);
    CHECK_EQ("thumb BLX PC", cpu->r[15], base + 0x20);
    CHECK_EQ("thumb BLX LR", cpu->r[14], (base + 6) | 1u);
    CHECK_EQ("thumb BLX T", (cpu->cpsr & CPSR_T) ? 1u : 0u, 0u);
    thumb_stop(nds);

    /* SWI #0x09 Div：r0=20, r1=6 → r0=3, r1=2, r3=3 */
    cpu->r[0] = 20; cpu->r[1] = 6;
    thumb_start(nds, base); bus_write16(nds->bus, base, 0xDF09); cpu_step(cpu);
    CHECK_EQ("thumb SWI div quot", cpu->r[0], 3);
    CHECK_EQ("thumb SWI div rem", cpu->r[1], 2);
    CHECK_EQ("thumb SWI div abs", cpu->r[3], 3);
    thumb_stop(nds);
}

/* ---- 阶段 13.5 PUSH/POP 与 STMIA/LDMIA ---- */
static void test_thumb_stack(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE;
    arm_cpu_t *cpu = nds->cpu;

    /* PUSH {r0-r3, lr}：STMDB sp!（先减后存） */
    cpu->r[13] = base + 0x200;
    cpu->r[0] = 1; cpu->r[1] = 2; cpu->r[2] = 3; cpu->r[3] = 4; cpu->r[14] = 5;
    thumb_start(nds, base); bus_write16(nds->bus, base, 0xB50F); cpu_step(cpu);
    CHECK_EQ("thumb PUSH sp", cpu->r[13], base + 0x200 - 20);
    CHECK_EQ("thumb PUSH r0", bus_read32(nds->bus, base + 0x200 - 20), 1);
    CHECK_EQ("thumb PUSH r3", bus_read32(nds->bus, base + 0x200 - 8), 4);
    CHECK_EQ("thumb PUSH lr", bus_read32(nds->bus, base + 0x200 - 4), 5);
    thumb_stop(nds);

    /* POP {r0-r3}：LDMIA sp!（先读后加） */
    bus_write32(nds->bus, base + 0x200 + 0, 0xAAu);
    bus_write32(nds->bus, base + 0x200 + 4, 0xBBu);
    bus_write32(nds->bus, base + 0x200 + 8, 0xCCu);
    bus_write32(nds->bus, base + 0x200 + 12, 0xDDu);
    cpu->r[13] = base + 0x200;
    thumb_start(nds, base); bus_write16(nds->bus, base, 0xBC0F); cpu_step(cpu);
    CHECK_EQ("thumb POP r0", cpu->r[0], 0xAAu);
    CHECK_EQ("thumb POP r3", cpu->r[3], 0xDDu);
    CHECK_EQ("thumb POP sp", cpu->r[13], base + 0x200 + 16);
    thumb_stop(nds);

    /* STMIA r0!, {r1,r2} */
    cpu->r[0] = base + 0x300; cpu->r[1] = 0x1111; cpu->r[2] = 0x2222;
    thumb_start(nds, base); bus_write16(nds->bus, base, 0xC006); cpu_step(cpu);
    CHECK_EQ("thumb STMIA [0]", bus_read32(nds->bus, base + 0x300), 0x1111);
    CHECK_EQ("thumb STMIA [1]", bus_read32(nds->bus, base + 0x304), 0x2222);
    CHECK_EQ("thumb STMIA wb", cpu->r[0], base + 0x308);
    thumb_stop(nds);

    /* LDMIA r0!, {r1,r2} */
    bus_write32(nds->bus, base + 0x300, 0x3333);
    bus_write32(nds->bus, base + 0x304, 0x4444);
    cpu->r[0] = base + 0x300;
    thumb_start(nds, base); bus_write16(nds->bus, base, 0xC806); cpu_step(cpu);
    CHECK_EQ("thumb LDMIA r1", cpu->r[1], 0x3333);
    CHECK_EQ("thumb LDMIA r2", cpu->r[2], 0x4444);
    CHECK_EQ("thumb LDMIA wb", cpu->r[0], base + 0x308);
    thumb_stop(nds);
}

/* ---- 阶段 13.7 综合：Thumb 真码写 VRAM + 调 SWI ---- */
static void test_thumb_vram(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE;
    static const uint16_t prog[] = {
        0x4804, /* LDR r0, [PC, #16] → r0 = VRAM 基址（字面量在 base+0x14） */
        0x211F, /* MOV r1, #0x1F（红） */
        0x8001, /* STRH r1, [r0, #0] → VRAM[0] = 0x001F */
        0x2214, /* MOV r2, #20 */
        0x2306, /* MOV r3, #6 */
        0x4610, /* MOV r0, r2 → r0=20 */
        0x4619, /* MOV r1, r3 → r1=6 */
        0xDF09, /* SWI #0x09 Div → r0=3, r1=2, r3=3 */
        0xE7FE, /* B .（保活，测试只跑到它之前） */
    };
    bus_write32(nds->bus, base + 0x14, 0x06000000u); /* 字面量：VRAM 基址 */
    thumb_write(nds, base, prog, sizeof prog / sizeof prog[0]);
    thumb_start(nds, base);
    for (int i = 0; i < 8; i++)   /* 执行前 8 条，B . 之前停下 */
        cpu_step(nds->cpu);
    thumb_stop(nds);

    CHECK_EQ("thumb vram px", bus_read16(nds->bus, BUS_VRAM_BASE), 0x001Fu);
    CHECK_EQ("thumb swi quot", nds->cpu->r[0], 3);
    CHECK_EQ("thumb swi rem", nds->cpu->r[1], 2);
    CHECK_EQ("thumb swi abs", nds->cpu->r[3], 3);
}

/* ---- 阶段 14 KEY1 辅助：测试用小端写 32 位 ---- */
static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* ---- 阶段 14.2 KEY1 (Blowfish) 块加解密 + 密钥表自检 ---- */
static void test_key1_roundtrip(nds_t *nds)
{
    (void)nds;

    /* 密钥表自检：累加和固定；bswap32 已知值。 */
    CHECK_EQ("key1 table checksum", key1_table_checksum(), 0x000803BAu);
    CHECK_EQ("key1 bswap32", key1_bswap32(0x12345678u), 0x78563412u);

    /* 三级密钥各做一次「加密→解密=还原」往返，覆盖不同调度深度 */
    for (uint32_t level = 1; level <= 3; level++) {
        uint32_t keybuf[0x412];
        key1_init_keycode(keybuf, 0x4D534141u /* "AASM" */, level);

        uint32_t blk[2] = {0x01234567u, 0x89ABCDEFu};
        uint32_t orig0 = blk[0], orig1 = blk[1];
        key1_encrypt64(keybuf, blk);
        int changed = (blk[0] != orig0) || (blk[1] != orig1);
        key1_decrypt64(keybuf, blk);

        char lo[32], hi[32], ch[32];
        snprintf(lo, sizeof lo, "key1 L%u rt lo", level);
        snprintf(hi, sizeof hi, "key1 L%u rt hi", level);
        snprintf(ch, sizeof ch, "key1 L%u changed", level);
        CHECK_EQ(lo, blk[0], orig0);
        CHECK_EQ(hi, blk[1], orig1);
        CHECK_EQ(ch, changed, 1);
    }
}

/* ---- 阶段 14.2 安全区「加密→解密」完整往返 + "encryObj" 魔数 ---- */
static void test_key1_secure_area(nds_t *nds)
{
    (void)nds;

    uint8_t sec[0x800];
    for (uint32_t i = 0; i < 0x800; i++)
        sec[i] = (uint8_t)(i * 7u + 3u);   /* 全填可辨认模式 */

    uint8_t plain[0x800];
    memcpy(plain, sec, sizeof sec);

    const uint32_t gamecode = 0x4D534141u; /* "AASM" */

    key1_encrypt_secure_area(gamecode, sec);
    /* 加密后头 8 字节不再是明文，body 也应改变 */
    CHECK_EQ("key1 sec head changed", memcmp(sec, plain, 8) != 0, 1);
    CHECK_EQ("key1 sec body changed", memcmp(sec + 8, plain + 8, 0x800 - 8) != 0, 1);

    int ok = key1_decrypt_secure_area(gamecode, sec);
    CHECK_EQ("key1 sec decrypt ok", ok, 1);
    CHECK_EQ("key1 sec magic e", sec[0], (uint8_t)'e');
    CHECK_EQ("key1 sec magic j", sec[7], (uint8_t)'j');
    CHECK_EQ("key1 sec body match", memcmp(sec + 8, plain + 8, 0x800 - 8) == 0, 1);
}

/* ---- 阶段 14.5 自制含加密安全区的 ROM 走完整装载流程 ---- */
static void test_secure_area_load(nds_t *nds)
{
    const uint32_t gamecode = 0x4D534141u; /* "AASM" */
    const uint32_t arm9_off = 0x4000;
    const uint32_t arm9_ram = 0x02000000;  /* Main RAM 基址 */
    const uint32_t arm9_entry = 0x02000800;
    const uint32_t arm9_size = 0x1000;

    /* 造一份最小 ROM：头(0x200) + 安全区(0x4000..0x47FF) + 少量 ARM9 代码区 */
    size_t rom_size = 0x4000 + arm9_size;
    cart_t cart;
    cart.data = (unsigned char *)calloc(1, rom_size);
    cart.size = rom_size;
    if (cart.data == NULL) {
        CHECK_EQ("cart alloc", 0, 1);
        return;
    }

    /* 头：gamecode @0x00C，ARM9 四字段 @0x020..0x02F */
    cart.data[0] = 'N'; cart.data[1] = 'T'; cart.data[2] = 'R'; cart.data[3] = 'J';
    put_le32(cart.data + 0x00C, gamecode);
    put_le32(cart.data + 0x020, arm9_off);
    put_le32(cart.data + 0x024, arm9_entry);
    put_le32(cart.data + 0x028, arm9_ram);
    put_le32(cart.data + 0x02C, arm9_size);

    /* 安全区明文：body 填模式，头 8 字节由加密函数写 "encryObj" */
    for (uint32_t i = 0; i < 0x800; i++)
        cart.data[arm9_off + i] = (uint8_t)(i * 5u + 1u);
    uint8_t plain_body[0x800 - 8];
    memcpy(plain_body, cart.data + arm9_off + 8, sizeof plain_body);

    /* 加密安全区，再走 cart 层解密 */
    key1_encrypt_secure_area(gamecode, cart.data + arm9_off);
    int decrypted = cart_decrypt_secure_area(&cart);
    CHECK_EQ("cart sec decrypted", decrypted, 1);
    CHECK_EQ("cart sec magic e", cart.data[arm9_off + 0], (uint8_t)'e');
    CHECK_EQ("cart sec magic j", cart.data[arm9_off + 7], (uint8_t)'j');
    CHECK_EQ("cart sec body match",
             memcmp(cart.data + arm9_off + 8, plain_body, sizeof plain_body) == 0, 1);

    /* 把解密后的 ARM9 镜像逐字节拷进 Main RAM（模拟 main.c 装载） */
    for (uint32_t i = 0; i < arm9_size; i++)
        bus_write8(nds->bus, arm9_ram + i, cart.data[arm9_off + i]);

    /* 读回验证：RAM 里的安全区头 8 字节已是明文魔数 */
    CHECK_EQ("ram sec magic e", bus_read8(nds->bus, arm9_ram + 0), (uint8_t)'e');
    CHECK_EQ("ram sec magic j", bus_read8(nds->bus, arm9_ram + 7), (uint8_t)'j');

    /* 从 entry 启动：PC 落在正确入口 */
    cpu_reset(nds->cpu, arm9_entry);
    CHECK_EQ("arm9 entry pc", nds->cpu->r[15], arm9_entry);

    free(cart.data);
}

/* 21-B9yi：真机只在 ROMCTRL bit31 **由 0 变 1** 时启动传输（melonDS
   `xferstart = (val & ~ROMCnt) & (1<<31)`）。测试统一用这个辅助函数：
   先写 0 清忙位、再写目标值触发，等价于真机/游戏代码的「等空闲→再启动」。 */
static void cart_activate(nds_t *nds, uint8_t hi)
{
    bus_write8(nds->bus, CART_ROMCTRL + 3, (uint8_t)(hi & 0x7Fu));
    bus_write8(nds->bus, CART_ROMCTRL + 3, hi);
}

/* 21-B9yi：让「CPU 程序直接写 ROMCTRL」的用例满足真机前置条件——
   卡槽使能（AUXSPICNT bit15=1、bit13=0）且 ROMCTRL 忙位先清零。 */
static void cart_prepare(nds_t *nds)
{
    bus_write16(nds->bus, CART_AUXSPICNT, AUXSPICNT_ENABLE);
    bus_write8(nds->bus, CART_ROMCTRL + 3, 0x00u);
}

/* ---- 阶段 15.2 卡带命令读：写命令 + 激活 + 从 CARD_DATA 按序读回 ---- */
static void test_cartbus_read(nds_t *nds)
{
    uint8_t rom[0x10000];
    for (uint32_t i = 0; i < sizeof rom; i++)
        rom[i] = (uint8_t)i;   /* rom[i]=i&0xFF */
    io_attach_cart(nds->io, rom, sizeof rom);

    /* 未激活命令时读数据端口：无数据 → 0xFFFFFFFF */
    CHECK_EQ("cart data before cmd", bus_read32(nds->bus, BUS_CARD_DATA), 0xFFFFFFFFu);

    /* 21-B9yi：真机/AUXSPICNT 口径——槽未使能（bit15=0）时写 ROMCTRL bit31
       不应启动传输（melonDS `WriteROMCnt` 的前置检查）。 */
    bus_write16(nds->bus, CART_AUXSPICNT, 0u);
    cart_activate(nds, 0x81u);
    CHECK_EQ("cart xfer blocked w/o slot",
             bus_read32(nds->bus, CART_ROMCTRL) & CART_ROMCTRL_ACTIVATE, 0u);
    bus_write8(nds->bus, CART_ROMCTRL + 3, 0x01); /* 清 bit31（保持块字段=1） */

    /* 使能槽（bit15=1，bit13=0 → ROM 模式）；下面照常写命令与激活 */
    bus_write16(nds->bus, CART_AUXSPICNT, AUXSPICNT_ENABLE);

    /* 命令 B7 读地址 0x8100（melonDS 会把 <0x8000 的请求重定向） */
    static const uint8_t cmd[8] = {0xB7, 0x00, 0x00, 0x81, 0x00, 0x00, 0x00, 0x00};
    for (int i = 0; i < 8; i++)
        bus_write8(nds->bus, CART_COMMAND + i, cmd[i]);

    /* 激活 ROMCTRL（写 bit31，块字段=1 → 0x200，melonDS 口径） */
    cart_activate(nds, 0x81u);

    /* 21-B9zb: 激活后要等 melonDS 首字延迟 DRQ 才就绪 */
    CHECK_EQ("cart romctrl not yet DRQ",
             bus_read32(nds->bus, CART_ROMCTRL) & CART_ROMCTRL_DRQ, 0u);
    cartbus_advance(&nds->io->cartbus, 100000u);
    /* DRQ 就绪 */
    CHECK_EQ("cart romctrl DRQ", bus_read32(nds->bus, CART_ROMCTRL) & CART_ROMCTRL_DRQ,
             CART_ROMCTRL_DRQ);
    /* 21-B9k：bit31 = 块传输忙，块读完前保持 1（FFXII 手动轮询该位） */
    CHECK_EQ("cart romctrl busy",
             bus_read32(nds->bus, CART_ROMCTRL) & CART_ROMCTRL_ACTIVATE,
             CART_ROMCTRL_ACTIVATE);

    /* 从 0x8100 读 4 字：每次读自动 +4 */
    for (int i = 0; i < 4; i++) {
        cartbus_advance(&nds->io->cartbus, 100000u);
        uint32_t want = (uint32_t)rom[0x8100 + 4 * i]
                      | ((uint32_t)rom[0x8100 + 4 * i + 1] << 8)
                      | ((uint32_t)rom[0x8100 + 4 * i + 2] << 16)
                      | ((uint32_t)rom[0x8100 + 4 * i + 3] << 24);
        char nm[32];
        snprintf(nm, sizeof nm, "cart data word[%d]", i);
        CHECK_EQ(nm, bus_read32(nds->bus, BUS_CARD_DATA), want);
    }

    /* 21-B9yi：真机口径——上一笔传输读完之前 busy 不会落 0，新的 ROMCTRL
       写入也不会启动新传输。这里把 0x200 块剩下的 124 字读完，让 busy 清零。 */
    for (int i = 4; i < 128; i++) {
        cartbus_advance(&nds->io->cartbus, 100000u);
        (void)bus_read32(nds->bus, BUS_CARD_DATA);
    }
    CHECK_EQ("cart block done busy",
             bus_read32(nds->bus, CART_ROMCTRL) & CART_ROMCTRL_ACTIVATE, 0u);

    /* 块大小=4（ROMCTRL bit24-26=7）：读完 1 字后 DRQ 与 busy 同时回落 */
    cart_activate(nds, 0x87u);
    cartbus_advance(&nds->io->cartbus, 100000u);
    CHECK_EQ("cart 4B busy",
             bus_read32(nds->bus, CART_ROMCTRL) & CART_ROMCTRL_ACTIVATE,
             CART_ROMCTRL_ACTIVATE);
    bus_read32(nds->bus, BUS_CARD_DATA);
    CHECK_EQ("cart 4B done busy",
             bus_read32(nds->bus, CART_ROMCTRL) & CART_ROMCTRL_ACTIVATE, 0u);
    CHECK_EQ("cart 4B done DRQ",
             bus_read32(nds->bus, CART_ROMCTRL) & CART_ROMCTRL_DRQ, 0u);

    /* 越界读：命令读 0x8FFC（rom 只有 0x10000），块外再读 → 0xFFFFFFFF */
    static const uint8_t cmd2[8] = {0xB7, 0x00, 0x00, 0x8F, 0xFC, 0x00, 0x00, 0x00};
    for (int i = 0; i < 8; i++)
        bus_write8(nds->bus, CART_COMMAND + i, cmd2[i]);
    cart_activate(nds, 0x87u); /* 4 字节块，读完即回落 */
    cartbus_advance(&nds->io->cartbus, 100000u);
    uint32_t w0 = (uint32_t)rom[0x8FFC] | ((uint32_t)rom[0x8FFD] << 8)
                | ((uint32_t)rom[0x8FFE] << 16) | ((uint32_t)rom[0x8FFF] << 24);
    CHECK_EQ("cart tail word", bus_read32(nds->bus, BUS_CARD_DATA), w0);
    CHECK_EQ("cart beyond end", bus_read32(nds->bus, BUS_CARD_DATA), 0xFFFFFFFFu);

    /* 21-B9w: B8 命令读芯片 ID（melonDS CartCommon ROMCommandReceive 直接返回
       ChipID，不是 ROM 数据）。0x400 字节 ROM 补成 2 的幂后仍 <1MB，
       芯片 ID = 0xC2 | ((0x100 - size>>28)<<8) = 0x100C2。 */
    static const uint8_t chipcmd[8] = {0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    for (int i = 0; i < 8; i++)
        bus_write8(nds->bus, CART_COMMAND + i, chipcmd[i]);
    cart_activate(nds, 0x80u);
    cartbus_advance(&nds->io->cartbus, 100000u);
    CHECK_EQ("cart chipid DRQ",
             bus_read32(nds->bus, CART_ROMCTRL) & CART_ROMCTRL_DRQ,
             CART_ROMCTRL_DRQ);
    CHECK_EQ("cart chipid word", bus_read32(nds->bus, BUS_CARD_DATA), 0x000100C2u);
    CHECK_EQ("cart chipid done busy",
             bus_read32(nds->bus, CART_ROMCTRL) & CART_ROMCTRL_ACTIVATE, 0u);
    CHECK_EQ("cart chipid second", bus_read32(nds->bus, BUS_CARD_DATA), 0xFFFFFFFFu);
}

/* ---- 阶段 15.3 用例：DMA 4 通道 + 地址递减 + VBlank 触发 ---- */
static void test_dma_channels(nds_t *nds)
{
    const uint32_t dma2 = IO_DMA0_BASE + 2 * IO_DMA_STRIDE; /* 0x040000C8 */
    const uint32_t dma3 = IO_DMA0_BASE + 3 * IO_DMA_STRIDE; /* 0x040000D4 */
    const uint32_t src  = 0x02001000u;
    const uint32_t vram = BUS_VRAM_BASE;

    /* a) 多通道独立：第 2/3 通道寄存器读写回 */
    bus_write32(nds->bus, dma2 + 0, 0x02001234u);
    CHECK_EQ("dma2 sad", bus_read32(nds->bus, dma2 + 0), 0x02001234u);
    bus_write32(nds->bus, dma3 + 4, 0x06000100u);
    CHECK_EQ("dma3 dad", bus_read32(nds->bus, dma3 + 4), 0x06000100u);

    /* b) 源递减：DMA2 源从 src+8 递减拷 3 字到 VRAM */
    bus_write32(nds->bus, src + 0, 0x11111111u);
    bus_write32(nds->bus, src + 4, 0x22222222u);
    bus_write32(nds->bus, src + 8, 0x33333333u);
    bus_write32(nds->bus, dma2 + 0, src + 8);
    bus_write32(nds->bus, dma2 + 4, vram + 0x200);
    bus_write16(nds->bus, dma2 + 8, 3u);
    bus_write16(nds->bus, dma2 + 10, DMA_CNT_32BIT | DMA_CNT_SRC_DEC | DMA_CNT_ENABLE);
    CHECK_EQ("dma2 dec [0]", bus_read32(nds->bus, vram + 0x200), 0x33333333u);
    CHECK_EQ("dma2 dec [1]", bus_read32(nds->bus, vram + 0x204), 0x22222222u);
    CHECK_EQ("dma2 dec [2]", bus_read32(nds->bus, vram + 0x208), 0x11111111u);

    /* c) VBlank 触发：DMA3 配好后不搬，io_set_vblank 后才搬 */
    bus_write32(nds->bus, src + 0, 0xDEADBEEFu);
    bus_write32(nds->bus, dma3 + 0, src);
    bus_write32(nds->bus, dma3 + 4, vram + 0x300);
    bus_write16(nds->bus, dma3 + 8, 1u);
    bus_write16(nds->bus, dma3 + 10,
                DMA_CNT_32BIT | (DMA_START_VBLANK << DMA_CNT_MODE_SHIFT) | DMA_CNT_ENABLE);
    CHECK_EQ("dma3 vblank not yet", bus_read32(nds->bus, vram + 0x300), 0x00000000u);
    io_set_vblank(nds->io);
    CHECK_EQ("dma3 vblank fired", bus_read32(nds->bus, vram + 0x300), 0xDEADBEEFu);

    /* d) DMA0 搬完置完成中断：IF bit8（21-B9p） */
    bus_write32(nds->bus, src + 0x100, 0xCAFEBABEu);
    bus_write32(nds->bus, IO_DMA0_BASE, src + 0x100);
    bus_write32(nds->bus, IO_DMA0_BASE + 4, vram + 0x400);
    bus_write16(nds->bus, IO_DMA0_BASE + 8, 1u);
    nds->io->irq[0].ifl = 0;
    bus_write16(nds->bus, IO_DMA0_BASE + 10,
                DMA_CNT_IRQ | DMA_CNT_32BIT | DMA_CNT_ENABLE);
    CHECK_EQ("dma0 irq mem", bus_read32(nds->bus, vram + 0x400), 0xCAFEBABEu);
    CHECK_EQ("dma0 irq IF bit8", nds->io->irq[0].ifl & (1u << 8), 1u << 8);
}

/* ---- 阶段 15.4 用例：卡带 DMA——DMA 从 ROM（CARD_DATA）搬数据到 RAM ---- */
static void test_card_dma(nds_t *nds)
{
    uint8_t rom[0x10000];
    for (uint32_t i = 0; i < sizeof rom; i++)
        rom[i] = (uint8_t)i;
    io_attach_cart(nds->io, rom, sizeof rom);

    const uint32_t dma0 = IO_DMA0_BASE;
    const uint32_t dst  = 0x02002000u;

    /* DMA0：源=CARD_DATA（固定），目的=RAM，32 位，卡带触发，先武装 1 字 */
    bus_write32(nds->bus, dma0 + 0, BUS_CARD_DATA);
    bus_write32(nds->bus, dma0 + 4, dst);
    bus_write16(nds->bus, dma0 + 8, 1u);
    bus_write16(nds->bus, dma0 + 10,
                DMA_CNT_32BIT | DMA_CNT_SRC_FIX
                | (DMA_START_CARD << DMA_CNT_MODE_SHIFT) | DMA_CNT_ENABLE);

    /* 卡带命令尚未激活：不该搬 */
    CHECK_EQ("card dma not yet", bus_read32(nds->bus, dst), 0x00000000u);
    bus_write16(nds->bus, dma0 + 10, 0u); /* 撤下武装：后面逐字重新武装 */

    /* 命令 B7 读 0x8100 + 激活 ROMCTRL */
    static const uint8_t cmd[8] = {0xB7, 0x00, 0x00, 0x81, 0x00, 0x00, 0x00, 0x00};
    for (int i = 0; i < 8; i++)
        bus_write8(nds->bus, CART_COMMAND + i, cmd[i]);
    bus_write16(nds->bus, CART_AUXSPICNT, AUXSPICNT_ENABLE);
    cart_activate(nds, 0x81u); /* 块字段=1 → 0x200 */
    /* 21-B9zb: 等卡带首字就绪；就绪边沿会让卡带 DMA 自动触发 */
    for (int spin = 0; spin < 200000 && !cartbus_ready(&nds->io->cartbus); spin++)
        io_advance_cart(nds->io, 0, 1u);

    /* 21-B9yi：卡带 DMA 按真机节奏逐字推进（每字由 DRQ 触发、由卡带取数事件推动）。
       逐字武装 count=1 并推进卡带时钟，把 8 个字依次搬到 dest —— 这也正是游戏
       代码的用法（`AF000001`：模式 5、count=1、重复位）。 */
    for (int i = 0; i < 8; i++) {
        uint32_t want = (uint32_t)rom[0x8100 + 4 * i]
                      | ((uint32_t)rom[0x8100 + 4 * i + 1] << 8)
                      | ((uint32_t)rom[0x8100 + 4 * i + 2] << 16)
                      | ((uint32_t)rom[0x8100 + 4 * i + 3] << 24);
        char nm[32];
        bus_write32(nds->bus, dma0 + 4, dst + 4u * i);
        bus_write16(nds->bus, dma0 + 8, 1u);
        bus_write16(nds->bus, dma0 + 10,
                    DMA_CNT_32BIT | DMA_CNT_SRC_FIX
                    | (DMA_START_CARD << DMA_CNT_MODE_SHIFT) | DMA_CNT_ENABLE);
        for (int spin = 0; spin < 200000 &&
             bus_read32(nds->bus, dst + 4u * i) != want; spin++)
            io_advance_cart(nds->io, 0, 1u);
        snprintf(nm, sizeof nm, "card dma word[%d]", i);
        CHECK_EQ(nm, bus_read32(nds->bus, dst + 4u * i), want);
    }

    /* 搬完自动清使能 */
    CHECK_EQ("card dma enable cleared", bus_read16(nds->bus, dma0 + 10) & DMA_CNT_ENABLE, 0u);
    /* 21-B9yi(续12)：melonDS 口径——读走一个字就清 DRQ（`ReadROMData` 里的
       `ROMCnt &= ~(1<<23)`），卡带取到下一个字才由 `RaiseDRQ()` 重新置位。
       所以「刚搬完」这一刻 DRQ 必须是 0；推进卡带时钟取到下一字后回到 1。 */
    CHECK_EQ("card romctrl DRQ after dma",
             bus_read32(nds->bus, CART_ROMCTRL) & CART_ROMCTRL_DRQ, 0u);
    for (int spin = 0; spin < 200000 && !cartbus_ready(&nds->io->cartbus); spin++)
        io_advance_cart(nds->io, 0, 1u);
    CHECK_EQ("card romctrl DRQ refilled",
             bus_read32(nds->bus, CART_ROMCTRL) & CART_ROMCTRL_DRQ,
             CART_ROMCTRL_DRQ);
    /* 21-B9yi：melonDS 口径——数据就绪（DRQ）只触发 DMA，**不**挂卡带中断；
       卡带中断只在 AUXSPICNT bit14 使能时由「传输结束」挂出。 */
    CHECK_EQ("card no irq on DRQ", bus_read32(nds->bus, IO_IF_ADDR) & IO_IF_CARD_DONE, 0u);
    /* 打开 AUXSPICNT bit14（传输完成中断使能）后把本块读完 → 传输结束挂 IRQ */
    bus_write16(nds->bus, CART_AUXSPICNT, AUXSPICNT_ENABLE | 0x4000u);
    for (int i = 8; i < 128; i++) {
        cartbus_advance(&nds->io->cartbus, 100000u);
        (void)bus_read32(nds->bus, BUS_CARD_DATA);
        io_advance_cart(nds->io, 0, 1u);
    }
    CHECK_EQ("card irq on transfer end",
             bus_read32(nds->bus, IO_IF_ADDR) & IO_IF_CARD_DONE, IO_IF_CARD_DONE);
}

/* ---- 21-B9wr 用例：NDS7（ARM7）卡带 DMA 触发模式 ----
   ARM9 的卡带触发模式号是 5（CNT 高半字 bit11-13）；NDS7 的卡带 DMA 模式
   定义不同：melonDS CheckDMAs(1,0x12) 用 CNT 高半字 bit13-12 = 2（即 0x2000）
   表示 DS 卡带 DRQ。本地旧实现只按 ARM9 的 5 匹配，ARM7 的卡带 DMA 永远
   不触发——FFXII 的 ARM7 正是用 0x12 从 CARD_DATA 搬 ROM 数据。 */
static void test_card_dma7(nds_t *nds)
{
    uint8_t rom[0x10000];
    for (uint32_t i = 0; i < sizeof rom; i++)
        rom[i] = (uint8_t)(i ^ 0x5Au);
    io_attach_cart(nds->io, rom, sizeof rom);

    const uint32_t dma0 = IO_DMA0_BASE;   /* ARM7 与 ARM9 的 DMA 寄存器同址、不同基 */
    const uint32_t dst  = 0x02003000u;

    /* 以 ARM7 视角写寄存器 → 落到 dma[1]，触发模式 bits13-12=2（=0x12） */
    nds->bus->active_is_arm7 = 1;
    bus_write32(nds->bus, dma0 + 0, BUS_CARD_DATA);
    bus_write32(nds->bus, dma0 + 4, dst);
    /* 21-B9yi(续12)：真机用法是 CNT_L=1 + 重复位（游戏写 `AF000001`），
       每个 DRQ 搬 1 字、重复位保持使能；一次武装 4 字会把空 FIFO 读成
       0xFFFFFFFF（旧用例的口径不符合真机节奏）。 */
    bus_write16(nds->bus, dma0 + 8, 1u);
    bus_write16(nds->bus, dma0 + 10,
                DMA_CNT_32BIT | DMA_CNT_SRC_FIX | DMA_CNT_REPEAT
                | (2u << 12) | DMA_CNT_ENABLE);
    nds->bus->active_is_arm7 = 0;

    CHECK_EQ("card7 mode field", (nds->io->dma[1].ch[0].cnt_h >> 12) & 3u, 2u);
    CHECK_EQ("card7 dma not yet", bus_read32(nds->bus, dst), 0u);

    /* 命令 B7 读 0x8100 + 激活 ROMCTRL；就绪边沿应触发 ARM7 的卡带 DMA */
    static const uint8_t cmd[8] = {0xB7, 0x00, 0x00, 0x81, 0x00, 0x00, 0x00, 0x00};
    for (int i = 0; i < 8; i++)
        bus_write8(nds->bus, CART_COMMAND + i, cmd[i]);
    cart_prepare(nds);
    cart_activate(nds, 0x81u);
    for (int spin = 0; spin < 200000 && !cartbus_ready(&nds->io->cartbus); spin++)
        io_advance_cart(nds->io, 0, 1u);

    /* 21-B9yi(续12)：卡带每取到一个字（DRQ 边沿）才把该字交给 DMA；
       循环推进卡带时钟，让 4 个字依次落进 dest（真机用法 CNT_L=1 + 重复位）。 */
    for (int i = 0; i < 4; i++) {
        uint32_t want = (uint32_t)rom[0x8100 + 4 * i]
                      | ((uint32_t)rom[0x8100 + 4 * i + 1] << 8)
                      | ((uint32_t)rom[0x8100 + 4 * i + 2] << 16)
                      | ((uint32_t)rom[0x8100 + 4 * i + 3] << 24);
        char nm[32];
        for (int spin = 0; spin < 200000 &&
             bus_read32(nds->bus, dst + 4u * i) != want; spin++)
            io_advance_cart(nds->io, 0, 1u);
        snprintf(nm, sizeof nm, "card7 dma word[%d]", i);
        CHECK_EQ(nm, bus_read32(nds->bus, dst + 4u * i), want);
    }
    CHECK_EQ("card7 dma enable stays (repeat)",
             nds->io->dma[1].ch[0].cnt_h & DMA_CNT_ENABLE, DMA_CNT_ENABLE);
    bus_write16(nds->bus, dma0 + 10, 0u);   /* 收尾：撤下武装 */
    /* ARM9（模式 5）不该被 ARM7 的 0x12 触发条件误伤 */
    CHECK_EQ("card7 arm9 untouched", nds->io->dma[0].ch[0].cnt_h, 0u);
}

/* ---- 阶段 15.5 用例：CPU 程序设 DMA + 激活卡带命令，DMA 从 ROM 搬数据到 RAM ---- */
static void test_card_program(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE;
    const uint32_t dst  = 0x02002000u;

    uint8_t rom[0x10000];
    for (uint32_t i = 0; i < sizeof rom; i++)
        rom[i] = (uint8_t)i;
    io_attach_cart(nds->io, rom, sizeof rom);

    /* 预写命令：B7 读地址 0x8100（命令字节大端） */
    static const uint8_t cmd[8] = {0xB7, 0x00, 0x00, 0x81, 0x00, 0x00, 0x00, 0x00};
    for (int i = 0; i < 8; i++)
        bus_write8(nds->bus, CART_COMMAND + i, cmd[i]);
    cart_prepare(nds); /* 21-B9yi：卡槽使能 + 忙位清零（真机前置条件） */

    /* 程序：设 DMA0（源=CARD_DATA 固定、目的=RAM、32 位、卡带触发、4 字）
       再写 ROMCTRL 激活 → 卡带就绪 → DMA 搬 4 字到 dest。 */
    static const uint32_t prog[] = {
        /* 0x00 */ 0xE3A00404, /* MOV r0, #0x04000000       r0 = IO 基址 */
        /* 0x04 */ 0xE28000B0, /* ADD r0, r0, #0xB0         r0 = 0x040000B0（DMA0） */
        /* 0x08 */ 0xE3A01404, /* MOV r1, #0x04000000       r1 = 0x04000000 */
        /* 0x0C */ 0xE2811810, /* ADD r1, r1, #0x100000     r1 = 0x04100000 */
        /* 0x10 */ 0xE2811010, /* ADD r1, r1, #0x10         r1 = 0x04100010（CARD_DATA） */
        /* 0x14 */ 0xE5801000, /* STR r1, [r0]              SAD = CARD_DATA */
        /* 0x18 */ 0xE3A02402, /* MOV r2, #0x02000000       r2 = Main RAM 基址 */
        /* 0x1C */ 0xE2822C20, /* ADD r2, r2, #0x2000       r2 = 0x02002000（dest） */
        /* 0x20 */ 0xE5802004, /* STR r2, [r0, #4]          DAD = dest */
        /* 0x24 */ 0xE3A034AF, /* MOV r3, #0xAF000000       r3 = CNT_H<<16
                                   （0xAF00：使能+模式5+32 位+源固定+**重复位 bit25**） */
        /* 0x28 */ 0xE3833001, /* ORR r3, r3, #0x01         r3 = 0xAF000001
                                   21-B9yi(续12)：真机/游戏用法（游戏写 `AF000001`）：
                                   每个 DRQ 搬 1 字、重复位让通道保持使能 */
        /* 0x2C */ 0xE5803008, /* STR r3, [r0, #8]          CNT=0xAF000001（卡带模式） */
        /* 0x30 */ 0xE3A01404, /* MOV r1, #0x04000000       r1 = 0x04000000 */
        /* 0x34 */ 0xE28110A4, /* ADD r1, r1, #0xA4         r1 = 0x040000A4 */
        /* 0x38 */ 0xE2811C01, /* ADD r1, r1, #0x100        r1 = 0x040001A4（ROMCTRL） */
        /* 0x3C */ 0xE3A00481, /* MOV r0, #0x81000000       activate + 块字段=1 */
        /* 0x40 */ 0xE5810000, /* STR r0, [r1]              ROMCTRL=activate → DMA 搬 */
        /* 0x44 */ 0xEAFFFFFE, /* B self（停机） */
    };

    run_program(nds, base, prog, sizeof prog / sizeof prog[0],
                base, base + 0x44, 64);
    CHECK_EQ("card prog PC halt", nds->cpu->r[15], base + 0x44);
    /* 21-B9yi：CPU 程序已停在自旋点，硬件侧继续推进卡带时钟——卡带每取一字
       触发一次 DMA（模式 5 + 重复位），逐字把 4 个字搬进 dest。 */
    for (int spin = 0; spin < 200000 && !cartbus_ready(&nds->io->cartbus); spin++)
        io_advance_cart(nds->io, 0, 1u);
    for (int spin = 0; spin < 200000 &&
         bus_read32(nds->bus, dst + 12u) == 0u; spin++)
        io_advance_cart(nds->io, 0, 1u);

    /* dest 收到 rom[0x8100..0x810F]（4 字） */
    for (int i = 0; i < 4; i++) {
        uint32_t want = (uint32_t)rom[0x8100 + 4 * i]
                      | ((uint32_t)rom[0x8100 + 4 * i + 1] << 8)
                      | ((uint32_t)rom[0x8100 + 4 * i + 2] << 16)
                      | ((uint32_t)rom[0x8100 + 4 * i + 3] << 24);
        char nm[32];
        snprintf(nm, sizeof nm, "card prog word[%d]", i);
        CHECK_EQ(nm, bus_read32(nds->bus, dst + 4u * i), want);
    }
    /* 21-B9yi(续12)：重复位置位时搬完不自动清使能——这正是游戏把整段
       512 字节交给卡带 DMA 逐字搬完的机制（旧用例断言「自动清使能」不符合）。 */
    CHECK_EQ("card prog enable stays (repeat)",
             bus_read16(nds->bus, IO_DMA0_BASE + 10) & DMA_CNT_ENABLE,
             DMA_CNT_ENABLE);
}

/* ---- 阶段 16.2 用例：存档芯片 SPI 状态机（EEPROM） ---- */
static void test_save_eeprom(nds_t *nds)
{
    (void)nds;
    save_t s;
    save_init(&s);
    CHECK_EQ("save eeprom cfg", save_configure(&s, SAVE_EEPROM_8K), 0);
    CHECK_EQ("save eeprom size", (uint32_t)s.size, 8192u);

    /* WREN 置写使能锁存 WEL */
    save_transfer(&s, 0x06);
    CHECK_EQ("save wen set", (s.status >> 1) & 1, 1u);

    /* RDSR 读回状态（bit1 WEL=1） */
    save_reset_cmd(&s);
    CHECK_EQ("save rdsr wel", save_transfer(&s, 0x05), 0x02u);

    /* 写地址 0x0100 4 字节 */
    save_reset_cmd(&s);
    save_transfer(&s, 0x06); /* WREN */
    save_reset_cmd(&s);
    save_transfer(&s, 0x02); /* WRITE */
    save_transfer(&s, 0x01); /* addr hi */
    save_transfer(&s, 0x00); /* addr lo */
    save_transfer(&s, 0xAA);
    save_transfer(&s, 0xBB);
    save_transfer(&s, 0xCC);
    save_transfer(&s, 0xDD);

    /* 读回 */
    save_reset_cmd(&s);
    save_transfer(&s, 0x03); /* READ */
    save_transfer(&s, 0x01);
    save_transfer(&s, 0x00);
    CHECK_EQ("save rd0", save_transfer(&s, 0x00), 0xAAu);
    CHECK_EQ("save rd1", save_transfer(&s, 0x00), 0xBBu);
    CHECK_EQ("save rd2", save_transfer(&s, 0x00), 0xCCu);
    CHECK_EQ("save rd3", save_transfer(&s, 0x00), 0xDDu);

    save_free(&s);
}

/* ---- 阶段 16.2 用例：Flash 存档（RDID + 擦除 + AND 写语义） ---- */
static void test_save_flash(nds_t *nds)
{
    (void)nds;
    save_t s;
    save_init(&s);
    CHECK_EQ("save flash cfg", save_configure(&s, SAVE_FLASH_256K), 0);
    CHECK_EQ("save flash size", (uint32_t)s.size, 262144u);

    /* RDID：命令字节后连续回 3 字节 JEDEC ID（0x20 0x20 0x12 = M45PE20） */
    save_reset_cmd(&s);
    save_transfer(&s, 0x9F);
    CHECK_EQ("flash id0", save_transfer(&s, 0x00), 0x20u);
    CHECK_EQ("flash id1", save_transfer(&s, 0x00), 0x20u);
    CHECK_EQ("flash id2", save_transfer(&s, 0x00), 0x12u);

    /* 页擦除 0x100（PE），擦后应全 0xFF */
    save_reset_cmd(&s);
    save_transfer(&s, 0x06); /* WREN */
    save_reset_cmd(&s);
    save_transfer(&s, 0xDB); /* PE */
    save_transfer(&s, 0x00); save_transfer(&s, 0x01); save_transfer(&s, 0x00);
    save_reset_cmd(&s);
    save_transfer(&s, 0x03); /* READ */
    save_transfer(&s, 0x00); save_transfer(&s, 0x01); save_transfer(&s, 0x00);
    CHECK_EQ("flash erased ff", save_transfer(&s, 0x00), 0xFFu);

    /* 页写 4 字节（0xFF & v = v） */
    save_reset_cmd(&s);
    save_transfer(&s, 0x06); /* WREN */
    save_reset_cmd(&s);
    save_transfer(&s, 0x02); /* PP */
    save_transfer(&s, 0x00); save_transfer(&s, 0x01); save_transfer(&s, 0x00);
    save_transfer(&s, 0x12);
    save_transfer(&s, 0x34);
    save_transfer(&s, 0x56);
    save_transfer(&s, 0x78);

    save_reset_cmd(&s);
    save_transfer(&s, 0x03);
    save_transfer(&s, 0x00); save_transfer(&s, 0x01); save_transfer(&s, 0x00);
    CHECK_EQ("flash rd0", save_transfer(&s, 0x00), 0x12u);
    CHECK_EQ("flash rd1", save_transfer(&s, 0x00), 0x34u);
    CHECK_EQ("flash rd2", save_transfer(&s, 0x00), 0x56u);
    CHECK_EQ("flash rd3", save_transfer(&s, 0x00), 0x78u);

    /* AND 写语义：再写 0x0F，0x12 & 0x0F = 0x02 */
    save_reset_cmd(&s);
    save_transfer(&s, 0x02); /* PP */
    save_transfer(&s, 0x00); save_transfer(&s, 0x01); save_transfer(&s, 0x00);
    save_transfer(&s, 0x0F);
    save_reset_cmd(&s);
    save_transfer(&s, 0x03);
    save_transfer(&s, 0x00); save_transfer(&s, 0x01); save_transfer(&s, 0x00);
    CHECK_EQ("flash and-sem", save_transfer(&s, 0x00), 0x02u);

    save_free(&s);
}

/* ---- 阶段 16.2/16.4 用例：经 AUXSPICNT/AUXSPIDATA 总线读写存档 ---- */
static void test_save_spi_regs(nds_t *nds)
{
    io_attach_save(nds->io, SAVE_EEPROM_8K);

    /* 选片：enable + SPI 模式 + 保持片选（0xA040），WREN 后撤片选 */
    bus_write16(nds->bus, CART_AUXSPICNT, 0xA040);
    bus_write8(nds->bus, CART_AUXSPIDATA, 0x06); /* WREN */
    bus_write16(nds->bus, CART_AUXSPICNT, 0x0000);

    /* 再选片：WRITE 地址 0x0020 = 0xAB，撤片选 */
    bus_write16(nds->bus, CART_AUXSPICNT, 0xA040);
    bus_write8(nds->bus, CART_AUXSPIDATA, 0x02); /* WRITE */
    bus_write8(nds->bus, CART_AUXSPIDATA, 0x00); /* addr hi */
    bus_write8(nds->bus, CART_AUXSPIDATA, 0x20); /* addr lo */
    bus_write8(nds->bus, CART_AUXSPIDATA, 0xAB); /* data */
    bus_write16(nds->bus, CART_AUXSPICNT, 0x0000);

    CHECK_EQ("spi chip wrote", io_get_save(nds->io)->data[0x20], 0xABu);

    /* 再选片：READ 地址 0x0020，哑元触发接收 */
    bus_write16(nds->bus, CART_AUXSPICNT, 0xA040);
    bus_write8(nds->bus, CART_AUXSPIDATA, 0x03); /* READ */
    bus_write8(nds->bus, CART_AUXSPIDATA, 0x00);
    bus_write8(nds->bus, CART_AUXSPIDATA, 0x20);
    bus_write8(nds->bus, CART_AUXSPIDATA, 0x00); /* dummy → 收 data[0x20] */
    CHECK_EQ("spi chip read", bus_read8(nds->bus, CART_AUXSPIDATA), 0xABu);
}

/* ---- 阶段 16.3 用例：存档持久化（写 .sav → 重新装载 → 读回一致） ---- */
static void test_save_persist(nds_t *nds)
{
    (void)nds;
    save_t s;
    save_init(&s);
    CHECK_EQ("persist cfg", save_configure(&s, SAVE_EEPROM_8K), 0);

    s.data[0] = 0x11;
    s.data[1] = 0x22;
    s.data[0x100] = 0x33;

    const char *path = "test_save_tmp.sav";
    CHECK_EQ("persist save", save_save_file(&s, path), 0);

    memset(s.data, 0x00, s.size); /* 破坏内存，模拟重新启动 */
    CHECK_EQ("persist load", save_load_file(&s, path), 0);
    CHECK_EQ("persist b0", s.data[0], 0x11u);
    CHECK_EQ("persist b1", s.data[1], 0x22u);
    CHECK_EQ("persist b100", s.data[0x100], 0x33u);

    remove(path);
    save_free(&s);
}

/* ---- 阶段 16.4 用例：CPU 程序经 AUXSPICNT/AUXSPIDATA 读写存档 ---- */
static void test_save_program(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE;
    io_attach_save(nds->io, SAVE_EEPROM_8K);

    /* 程序：选片(0xA040) → WREN → WRITE 0x20=0xAB → 撤片选 → 选片 →
       READ 0x20 → 收数据到 r3 → 停机。 */
    static const uint32_t prog[] = {
        /* 0x00 */ 0xE3A00404, /* MOV r0, #0x04000000 */
        /* 0x04 */ 0xE28000A0, /* ADD r0, r0, #0xA0        r0=0x040000A0 */
        /* 0x08 */ 0xE2800C01, /* ADD r0, r0, #0x100       r0=0x040001A0 AUXSPICNT */
        /* 0x0C */ 0xE2801002, /* ADD r1, r0, #2           r1=0x040001A2 AUXSPIDATA */
        /* 0x10 */ 0xE3A02040, /* MOV r2, #0x40 */
        /* 0x14 */ 0xE5C02000, /* STRB r2, [r0]            AUXSPICNT 低=0x40 */
        /* 0x18 */ 0xE3A020A0, /* MOV r2, #0xA0 */
        /* 0x1C */ 0xE5C02001, /* STRB r2, [r0, #1]        AUXSPICNT=0xA040 选片 */
        /* 0x20 */ 0xE3A02006, /* MOV r2, #0x06 */
        /* 0x24 */ 0xE5C12000, /* STRB r2, [r1]            WREN */
        /* 0x28 */ 0xE3A02002, /* MOV r2, #0x02 */
        /* 0x2C */ 0xE5C12000, /* STRB r2, [r1]            WRITE */
        /* 0x30 */ 0xE3A02000, /* MOV r2, #0 */
        /* 0x34 */ 0xE5C12000, /* STRB r2, [r1]            addr hi=0 */
        /* 0x38 */ 0xE3A02020, /* MOV r2, #0x20 */
        /* 0x3C */ 0xE5C12000, /* STRB r2, [r1]            addr lo=0x20 */
        /* 0x40 */ 0xE3A020AB, /* MOV r2, #0xAB */
        /* 0x44 */ 0xE5C12000, /* STRB r2, [r1]            data=0xAB */
        /* 0x48 */ 0xE3A02000, /* MOV r2, #0 */
        /* 0x4C */ 0xE5C02000, /* STRB r2, [r0]            AUXSPICNT 低=0 */
        /* 0x50 */ 0xE5C02001, /* STRB r2, [r0, #1]        AUXSPICNT=0 撤片选 */
        /* 0x54 */ 0xE3A02040, /* MOV r2, #0x40 */
        /* 0x58 */ 0xE5C02000, /* STRB r2, [r0] */
        /* 0x5C */ 0xE3A020A0, /* MOV r2, #0xA0 */
        /* 0x60 */ 0xE5C02001, /* STRB r2, [r0, #1]        再选片 */
        /* 0x64 */ 0xE3A02003, /* MOV r2, #0x03 */
        /* 0x68 */ 0xE5C12000, /* STRB r2, [r1]            READ */
        /* 0x6C */ 0xE3A02000, /* MOV r2, #0 */
        /* 0x70 */ 0xE5C12000, /* STRB r2, [r1]            addr hi=0 */
        /* 0x74 */ 0xE3A02020, /* MOV r2, #0x20 */
        /* 0x78 */ 0xE5C12000, /* STRB r2, [r1]            addr lo=0x20 */
        /* 0x7C */ 0xE3A02000, /* MOV r2, #0 */
        /* 0x80 */ 0xE5C12000, /* STRB r2, [r1]            dummy → 收 data[0x20] */
        /* 0x84 */ 0xE5D13000, /* LDRB r3, [r1]            r3 = AUXSPIDATA */
        /* 0x88 */ 0xEAFFFFFE, /* B self 停机 */
    };

    run_program(nds, base, prog, sizeof prog / sizeof prog[0],
                base, base + 0x88, 128);
    CHECK_EQ("save prog halt", nds->cpu->r[15], base + 0x88);
    CHECK_EQ("save prog r3", nds->cpu->r[3], 0xABu);
    CHECK_EQ("save prog chip", io_get_save(nds->io)->data[0x20], 0xABu);
}

/* ---- 阶段 17.2 用例：TSC 触摸屏 SPI 状态机（12/8 位、未按下、Hold 复位） ---- */
static void test_touch_unit(nds_t *nds)
{
    (void)nds;
    touch_t t;
    memset(&t, 0, sizeof t);

    /* 选中设备：enable(bit15) + touch(bit9) + hold(bit11) = 0x8A00 */
    touch_write8(&t, IO_SPICNT, 0x00);
    touch_write8(&t, IO_SPICNT + 1, 0x8A);
    CHECK_EQ("touch spicnt", t.spicnt, 0x8A00u);

    /* 未按下：X 读回 0x000、Y 读回 0xFFF */
    touch_write8(&t, IO_SPIDATA, TSC_CMD_X);
    CHECK_EQ("touch rel x b1", touch_read8(&t, IO_SPIDATA), 0x00u);
    touch_write8(&t, IO_SPIDATA, 0x00);
    CHECK_EQ("touch rel x b2", touch_read8(&t, IO_SPIDATA), 0x00u);

    touch_write8(&t, IO_SPIDATA, TSC_CMD_Y);
    CHECK_EQ("touch rel y b1", touch_read8(&t, IO_SPIDATA), 0x7Fu);
    touch_write8(&t, IO_SPIDATA, 0x00);
    CHECK_EQ("touch rel y b2", touch_read8(&t, IO_SPIDATA), 0xF8u);

    /* 按下 (adc_x=0x2AB, adc_y=0x2CD)：12 位拼装还原 */
    touch_set_pos(&t, 0x2AB, 0x2CD, 1);
    uint16_t x1, x2, y1, y2;
    touch_write8(&t, IO_SPIDATA, TSC_CMD_X);
    x1 = touch_read8(&t, IO_SPIDATA);
    touch_write8(&t, IO_SPIDATA, 0x00);
    x2 = touch_read8(&t, IO_SPIDATA);
    CHECK_EQ("touch x b1", x1, 0x15u);
    CHECK_EQ("touch x b2", x2, 0x58u);
    CHECK_EQ("touch x rec", ((x1 & 0x7F) << 5) | (x2 >> 3), 0x2ABu);

    touch_write8(&t, IO_SPIDATA, TSC_CMD_Y);
    y1 = touch_read8(&t, IO_SPIDATA);
    touch_write8(&t, IO_SPIDATA, 0x00);
    y2 = touch_read8(&t, IO_SPIDATA);
    CHECK_EQ("touch y b1", y1, 0x16u);
    CHECK_EQ("touch y b2", y2, 0x68u);
    CHECK_EQ("touch y rec", ((y1 & 0x7F) << 5) | (y2 >> 3), 0x2CDu);

    /* 8 位模式：X = 0xD8，取高 8 位 = 0x2AB >> 4 = 0x2A */
    touch_write8(&t, IO_SPIDATA, 0xD8);
    CHECK_EQ("touch 8bit b1", touch_read8(&t, IO_SPIDATA), 0x15u);
    touch_write8(&t, IO_SPIDATA, 0x00);
    CHECK_EQ("touch 8bit b2", touch_read8(&t, IO_SPIDATA), 0x00u);

    /* 清 Hold：传输后撤片选，后续非命令字节回传 0 */
    touch_write8(&t, IO_SPICNT + 1, 0x82);   /* enable + touch，无 hold */
    touch_write8(&t, IO_SPIDATA, TSC_CMD_X);
    touch_write8(&t, IO_SPIDATA, 0x00);      /* 无命令 → 回 0 */
    CHECK_EQ("touch nohold reset", touch_read8(&t, IO_SPIDATA), 0x00u);
}

/* ---- 阶段 17.2 用例：经 SPICNT/SPIDATA 总线读触摸坐标 ---- */
static void test_touch_spi_regs(nds_t *nds)
{
    io_set_touch(nds->io, 0x2AB, 0x2CD, 1);
    bus_write16(nds->bus, IO_SPICNT, 0x8A00);

    bus_write8(nds->bus, IO_SPIDATA, TSC_CMD_X);
    uint8_t x1 = bus_read8(nds->bus, IO_SPIDATA);
    bus_write8(nds->bus, IO_SPIDATA, 0x00);
    uint8_t x2 = bus_read8(nds->bus, IO_SPIDATA);
    CHECK_EQ("spi x rec", ((x1 & 0x7F) << 5) | (x2 >> 3), 0x2ABu);

    bus_write8(nds->bus, IO_SPIDATA, TSC_CMD_Y);
    uint8_t y1 = bus_read8(nds->bus, IO_SPIDATA);
    bus_write8(nds->bus, IO_SPIDATA, 0x00);
    uint8_t y2 = bus_read8(nds->bus, IO_SPIDATA);
    CHECK_EQ("spi y rec", ((y1 & 0x7F) << 5) | (y2 >> 3), 0x2CDu);

    /* 未按下：X=0、Y=0xFFF */
    io_set_touch(nds->io, 0, 0, 0);
    bus_write8(nds->bus, IO_SPIDATA, TSC_CMD_Y);
    uint8_t ry1 = bus_read8(nds->bus, IO_SPIDATA);
    bus_write8(nds->bus, IO_SPIDATA, 0x00);
    uint8_t ry2 = bus_read8(nds->bus, IO_SPIDATA);
    CHECK_EQ("spi y rel", ((ry1 & 0x7F) << 5) | (ry2 >> 3), 0xFFFu);
}

/* ---- 阶段 17.3 用例：CPU 程序经 SPICNT/SPIDATA 读出 X/Y 坐标 ---- */
static void test_touch_program(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE;
    io_set_touch(nds->io, 0x2AB, 0x2CD, 1);

    /* 程序：SPICNT=0x8A00 → 读 X（命令+哑元，拼 12 位）到 r6 → 读 Y 到 r7 → 停机。 */
    static const uint32_t prog[] = {
        /* 0x00 */ 0xE3A00404, /* MOV r0, #0x04000000 */
        /* 0x04 */ 0xE2800C01, /* ADD r0, r0, #0x100       r0=0x04000100 */
        /* 0x08 */ 0xE28000C0, /* ADD r0, r0, #0xC0        r0=0x040001C0 SPICNT */
        /* 0x0C */ 0xE2801002, /* ADD r1, r0, #2           r1=0x040001C2 SPIDATA */
        /* 0x10 */ 0xE3A0208A, /* MOV r2, #0x8A */
        /* 0x14 */ 0xE5C02001, /* STRB r2, [r0, #1]        SPICNT 高=0x8A */
        /* 0x18 */ 0xE3A02000, /* MOV r2, #0 */
        /* 0x1C */ 0xE5C02000, /* STRB r2, [r0]            SPICNT=0x8A00 选片 */
        /* 0x20 */ 0xE3A020D0, /* MOV r2, #0xD0 */
        /* 0x24 */ 0xE5C12000, /* STRB r2, [r1]            命令 X */
        /* 0x28 */ 0xE5D13000, /* LDRB r3, [r1]            b1 */
        /* 0x2C */ 0xE3A02000, /* MOV r2, #0 */
        /* 0x30 */ 0xE5C12000, /* STRB r2, [r1]            哑元 */
        /* 0x34 */ 0xE5D14000, /* LDRB r4, [r1]            b2 */
        /* 0x38 */ 0xE203307F, /* AND r3, r3, #0x7F */
        /* 0x3C */ 0xE1A03283, /* MOV r3, r3, LSL #5 */
        /* 0x40 */ 0xE1A041A4, /* MOV r4, r4, LSR #3 */
        /* 0x44 */ 0xE1833004, /* ORR r3, r3, r4           r3 = x */
        /* 0x48 */ 0xE1A06003, /* MOV r6, r3               x -> r6 */
        /* 0x4C */ 0xE3A02090, /* MOV r2, #0x90 */
        /* 0x50 */ 0xE5C12000, /* STRB r2, [r1]            命令 Y */
        /* 0x54 */ 0xE5D13000, /* LDRB r3, [r1]            b1 */
        /* 0x58 */ 0xE3A02000, /* MOV r2, #0 */
        /* 0x5C */ 0xE5C12000, /* STRB r2, [r1]            哑元 */
        /* 0x60 */ 0xE5D14000, /* LDRB r4, [r1]            b2 */
        /* 0x64 */ 0xE203307F, /* AND r3, r3, #0x7F */
        /* 0x68 */ 0xE1A03283, /* MOV r3, r3, LSL #5 */
        /* 0x6C */ 0xE1A041A4, /* MOV r4, r4, LSR #3 */
        /* 0x70 */ 0xE1833004, /* ORR r3, r3, r4           r3 = y */
        /* 0x74 */ 0xE1A07003, /* MOV r7, r3               y -> r7 */
        /* 0x78 */ 0xEAFFFFFE, /* B self 停机 */
    };

    run_program(nds, base, prog, sizeof prog / sizeof prog[0],
                base, base + 0x78, 128);
    CHECK_EQ("touch prog halt", nds->cpu->r[15], base + 0x78);
    CHECK_EQ("touch prog x", nds->cpu->r[6], 0x2ABu);
    CHECK_EQ("touch prog y", nds->cpu->r[7], 0x2CDu);
}

/* ---- 阶段 18.2 用例：音频寄存器（SOUNDCNT/SOUNDBIAS + 16 通道）读写 ---- */
static void test_snd_regs(nds_t *nds)
{
    nds->bus->active_is_arm7 = 1; /* 音频由 ARM7 控制（阶段 19 起与几何区按 CPU 分流） */
    /* 主控寄存器：主使能(bit15) + 主音量 127，bias 0x200 */
    bus_write16(nds->bus, SND_SOUNDCNT, 0x807F);
    CHECK_EQ("snd soundcnt", bus_read16(nds->bus, SND_SOUNDCNT), 0x807Fu);
    bus_write16(nds->bus, SND_SOUNDBIAS, 0x0200);
    CHECK_EQ("snd soundbias", bus_read16(nds->bus, SND_SOUNDBIAS), 0x0200u);

    /* 通道 0 全套寄存器写读回 */
    uint32_t b = SND_BASE;
    bus_write32(nds->bus, b + 0x0, 0x8000007F);   /* CNT: start + vol 127 */
    bus_write32(nds->bus, b + 0x4, 0x02000000);   /* SAD */
    bus_write16(nds->bus, b + 0x8, 0x1000);       /* TMR */
    bus_write16(nds->bus, b + 0xA, 0x0000);       /* PNT */
    bus_write32(nds->bus, b + 0xC, 0x10);         /* LEN */
    CHECK_EQ("snd ch0 cnt", bus_read32(nds->bus, b + 0x0), 0x8000007Fu);
    CHECK_EQ("snd ch0 sad", bus_read32(nds->bus, b + 0x4), 0x02000000u);
    CHECK_EQ("snd ch0 tmr", bus_read16(nds->bus, b + 0x8), 0x1000u);
    CHECK_EQ("snd ch0 pnt", bus_read16(nds->bus, b + 0xA), 0x0000u);
    CHECK_EQ("snd ch0 len", bus_read32(nds->bus, b + 0xC), 0x10u);

    /* 通道 1 独立（16 字节步进） */
    bus_write16(nds->bus, b + 0x10 + 0x8, 0x2000);
    CHECK_EQ("snd ch1 tmr", bus_read16(nds->bus, b + 0x10 + 0x8), 0x2000u);

    /* 越界（主控区上界之后，本阶段未实现 capture）读 0 */
    CHECK_EQ("snd oob", bus_read8(nds->bus, SND_END), 0x00u);
}

/* ---- 阶段 18.3 用例：混音器（PCM8/PCM16/ADPCM/PSG + 静音 + 主音量 + 单发停止） ---- */
static void test_snd_mix(nds_t *nds)
{
    int16_t L[8], R[8];
    uint32_t b = SND_BASE;
    nds->bus->active_is_arm7 = 1; /* 音频由 ARM7 控制 */

    /* 主使能 + 主音量 127，bias 0x200 */
    bus_write16(nds->bus, SND_SOUNDCNT, 0x807F);
    bus_write16(nds->bus, SND_SOUNDBIAS, 0x0200);

    /* 无通道：静音 → 输出 0 */
    snd_render(&nds->io->snd, nds->bus, L, R, 8);
    CHECK_EQ("snd silence L", (uint16_t)L[0], 0x0000u);
    CHECK_EQ("snd silence R", (uint16_t)R[0], 0x0000u);

    /* PCM8：样本 0x40 → raw=0x100 → 满偏右声道 out=0x4000 */
    uint32_t sram = BUS_MAIN_RAM_BASE + 0x1000;
    memset(nds->bus->main_ram + 0x1000, 0, 64);
    bus_write8(nds->bus, sram, 0x40);
    bus_write32(nds->bus, b + 0x0, 0x907F007Fu);  /* PCM8, vol127, pan127, oneshot, start */
    bus_write32(nds->bus, b + 0x4, sram);
    bus_write16(nds->bus, b + 0x8, 0x1000);
    bus_write32(nds->bus, b + 0xC, 4);
    snd_render(&nds->io->snd, nds->bus, L, R, 1);
    CHECK_EQ("pcm8 L", (uint16_t)L[0], 0x0000u);
    CHECK_EQ("pcm8 R", (uint16_t)R[0], 0x4000u);
    bus_write32(nds->bus, b + 0x0, 0);   /* 停掉 ch0，避免污染下一通道 */

    /* PCM16：样本 0x4000 → raw=0x100（通道 1） */
    uint32_t c1 = b + SND_CH_STRIDE;
    bus_write16(nds->bus, sram + 0x20, 0x4000);
    bus_write32(nds->bus, c1 + 0x0, 0xB07F007Fu); /* PCM16, vol127, pan127, oneshot, start */
    bus_write32(nds->bus, c1 + 0x4, sram + 0x20);
    bus_write16(nds->bus, c1 + 0x8, 0x1000);
    bus_write32(nds->bus, c1 + 0xC, 4);
    snd_render(&nds->io->snd, nds->bus, L, R, 1);
    CHECK_EQ("pcm16 R", (uint16_t)R[0], 0x4000u);
    bus_write32(nds->bus, c1 + 0x0, 0);   /* 停掉 ch1 */

    /* IMA-ADPCM：头 0x20（init=16384）+ 数据 nibble 0 → raw=0x100（通道 2） */
    uint32_t c2 = b + 2 * SND_CH_STRIDE;
    bus_write32(nds->bus, sram + 0x40, 0x00000020);  /* header: sample=32, index=0 */
    bus_write32(nds->bus, sram + 0x44, 0x00000000);  /* data: 全 0 nibble */
    bus_write32(nds->bus, c2 + 0x0, 0xD07F007Fu);    /* ADPCM, vol127, pan127, oneshot, start */
    bus_write32(nds->bus, c2 + 0x4, sram + 0x40);
    bus_write16(nds->bus, c2 + 0x8, 0x1000);
    bus_write32(nds->bus, c2 + 0xC, 2);
    snd_render(&nds->io->snd, nds->bus, L, R, 1);
    CHECK_EQ("adpcm R", (uint16_t)R[0], 0x4000u);
    bus_write32(nds->bus, c2 + 0x0, 0);   /* 停掉 ch2 */

    /* PSG 方波：duty=0 → 首个相位 +0x200 → 满偏 out=0x7FC0（通道 3） */
    uint32_t c3 = b + 3 * SND_CH_STRIDE;
    bus_write32(nds->bus, c3 + 0x0, 0xE07F007Fu);   /* PSG, vol127, pan127, start */
    snd_render(&nds->io->snd, nds->bus, L, R, 1);
    CHECK_EQ("psg R", (uint16_t)R[0], 0x7FC0u);
    bus_write32(nds->bus, c3 + 0x0, 0);   /* 停掉 ch3 */

    /* 主音量 0：仍有通道但整体静音 → 输出 0 */
    bus_write16(nds->bus, SND_SOUNDCNT, 0x8000);
    snd_render(&nds->io->snd, nds->bus, L, R, 1);
    CHECK_EQ("snd master0 L", (uint16_t)L[0], 0x0000u);
    CHECK_EQ("snd master0 R", (uint16_t)R[0], 0x0000u);

    /* 单发停止：PCM8, len=1, tmr=1 → 1 个采样后 pos 越界，bit31 清除（通道 4） */
    bus_write16(nds->bus, SND_SOUNDCNT, 0x807F);
    uint32_t c4 = b + 4 * SND_CH_STRIDE;
    bus_write32(nds->bus, c4 + 0x0, 0x907F007Fu);
    bus_write32(nds->bus, c4 + 0x4, sram);
    bus_write16(nds->bus, c4 + 0x8, 0x0001);
    bus_write32(nds->bus, c4 + 0xC, 1);
    snd_render(&nds->io->snd, nds->bus, L, R, 1);
    CHECK_EQ("snd oneshot stop", bus_read32(nds->bus, c4 + 0x0) & 0x80000000u, 0x0u);
}

/* ---- 阶段 18.4 用例：CPU 程序配置通道 0 并读回寄存器 ---- */
static void test_snd_program(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE;

    /* 程序：r0=0x04000400（通道0），逐字节写 CNT=0x907F007F，
       再写 SAD/TMR/LEN，读回 r4..r7，停机。 */
    static const uint32_t prog[] = {
        /* 0x00 */ 0xE3A00404, /* MOV r0, #0x04000000 */
        /* 0x04 */ 0xE2800B01, /* ADD r0, r0, #0x400      r0=0x04000400 */
        /* 0x08 */ 0xE3A0207F, /* MOV r2, #0x7F */
        /* 0x0C */ 0xE5C02000, /* STRB r2, [r0]           CNT[0]=0x7F */
        /* 0x10 */ 0xE3A02000, /* MOV r2, #0 */
        /* 0x14 */ 0xE5C02001, /* STRB r2, [r0,#1]        CNT[1]=0x00 */
        /* 0x18 */ 0xE3A0207F, /* MOV r2, #0x7F */
        /* 0x1C */ 0xE5C02002, /* STRB r2, [r0,#2]        CNT[2]=0x7F */
        /* 0x20 */ 0xE3A02090, /* MOV r2, #0x90 */
        /* 0x24 */ 0xE5C02003, /* STRB r2, [r0,#3]        CNT[3]=0x90 start */
        /* 0x28 */ 0xE3A02780, /* MOV r2, #0x02000000 */
        /* 0x2C */ 0xE5802004, /* STR r2, [r0,#4]         SAD */
        /* 0x30 */ 0xE3A02C10, /* MOV r2, #0x1000 */
        /* 0x34 */ 0xE5802008, /* STR r2, [r0,#8]         TMR */
        /* 0x38 */ 0xE3A02004, /* MOV r2, #4 */
        /* 0x3C */ 0xE580200C, /* STR r2, [r0,#0xC]       LEN */
        /* 0x40 */ 0xE5904000, /* LDR r4, [r0]            CNT */
        /* 0x44 */ 0xE5905004, /* LDR r5, [r0,#4]         SAD */
        /* 0x48 */ 0xE5906008, /* LDR r6, [r0,#8]         TMR(+PNT=0) */
        /* 0x4C */ 0xE590700C, /* LDR r7, [r0,#0xC]       LEN */
        /* 0x50 */ 0xEAFFFFFE, /* B self 停机 */
    };

    run_cpu7(nds, base, prog, sizeof prog / sizeof prog[0],
             base, base + 0x50, 128);
    CHECK_EQ("snd prog halt", nds->cpu7->r[15], base + 0x50);
    CHECK_EQ("snd prog cnt", nds->cpu7->r[4], 0x907F007Fu);
    CHECK_EQ("snd prog sad", nds->cpu7->r[5], 0x02000000u);
    CHECK_EQ("snd prog tmr", nds->cpu7->r[6], 0x1000u);
    CHECK_EQ("snd prog len", nds->cpu7->r[7], 0x4u);
}

/* ---- 阶段 19.3 用例：顶点变换（pos×proj → 透视除 → 视口映射） ---- */
static void test_gx_transform(nds_t *nds)
{
    gx_t *g = &nds->io->gx;
    int sx, sy;
    gx_reset(g); /* pos/proj=单位阵，视口=(0,0,255,191) */

    gx_transform_vertex(g, -GX_FP_ONE, -GX_FP_ONE, 0, &sx, &sy);
    CHECK_EQ("gx (-1,-1) sx", sx, 0);
    CHECK_EQ("gx (-1,-1) sy", sy, 191);

    gx_transform_vertex(g, GX_FP_ONE, -GX_FP_ONE, 0, &sx, &sy);
    CHECK_EQ("gx (1,-1) sx", sx, 255);
    CHECK_EQ("gx (1,-1) sy", sy, 191);

    gx_transform_vertex(g, -GX_FP_ONE, GX_FP_ONE, 0, &sx, &sy);
    CHECK_EQ("gx (-1,1) sx", sx, 0);
    CHECK_EQ("gx (-1,1) sy", sy, 0);

    gx_transform_vertex(g, 0, 0, 0, &sx, &sy);
    /* 21-B9yi(续34)：视口映射改成 melonDS 口径（视口宽 y1-y0+1、屏幕行号
       以 191-y1 为上边缘）后，屏幕中心是 (128,96)（此前本地按 (vx2-vx1)/2
       得到 (127,95)，整幅 3D 画面还额外被垂直镜像）。 */
    CHECK_EQ("gx (0,0) sx", sx, 128);
    CHECK_EQ("gx (0,0) sy", sy, 96);
}

/* ---- 阶段 19.4 用例：三角形软件光栅化（平色） ---- */
static void test_gx_raster(nds_t *nds)
{
    gx_t *g = &nds->io->gx;
    gx_reset(g);
    gx_raster_tri(g, 10, 10, 100, 10, 10, 100, 0x7C00);
    CHECK_EQ("gx raster in",   g->fb[50 * GX_SCREEN_W + 50],   0x7C00u);
    CHECK_EQ("gx raster out1", g->fb[150 * GX_SCREEN_W + 150], 0u);
    CHECK_EQ("gx raster out2", g->fb[5 * GX_SCREEN_W + 5],     0u);
}

/* ---- 阶段 19.5 用例：GXFIFO 命令流 → 变换 → 光栅化出图 ---- */
static void test_gx_fifo(nds_t *nds)
{
    gx_t *g = &nds->io->gx;
    bus_t *bus = nds->bus;
    gx_reset(g);
    bus->active_is_arm7 = 0; /* 几何区由 ARM9 访问 */

    /* 投影矩阵 = diag(1/4,1/4,1,1)：模型 ±4 → NDC ±1（列主序 16 参数） */
    static const uint32_t proj[16] = {
        0x400, 0, 0, 0,
        0, 0x400, 0, 0,
        0, 0, 0x1000, 0,
        0, 0, 0, 0x1000,
    };

    /* MTX_MODE=投影(0) */
    bus_write32(bus, GX_GXFIFO, 0x10); bus_write32(bus, GX_GXFIFO, 0);
    /* MTX_LOAD_4x4 */
    bus_write32(bus, GX_GXFIFO, 0x16);
    for (int i = 0; i < 16; i++) bus_write32(bus, GX_GXFIFO, proj[i]);
    /* MTX_MODE=位置(1) + MTX_IDENTITY */
    bus_write32(bus, GX_GXFIFO, 0x10); bus_write32(bus, GX_GXFIFO, 1);
    bus_write32(bus, GX_GXFIFO, 0x15);
    /* COLOR=红 + VIEWPORT=(0,0,255,191) */
    bus_write32(bus, GX_GXFIFO, 0x20); bus_write32(bus, GX_GXFIFO, 0x7C00);
    bus_write32(bus, GX_GXFIFO, 0x60); bus_write32(bus, GX_GXFIFO, 0xBFFF0000u);
    /* BEGIN_VTXS 三角形 + 3 个 VTX_16：(0,0) (4,0) (0,-4) */
    bus_write32(bus, GX_GXFIFO, 0x40); bus_write32(bus, GX_GXFIFO, 0);
    bus_write32(bus, GX_GXFIFO, 0x23); bus_write32(bus, GX_GXFIFO, 0x00000000u); bus_write32(bus, GX_GXFIFO, 0);
    bus_write32(bus, GX_GXFIFO, 0x23); bus_write32(bus, GX_GXFIFO, 0x00004000u); bus_write32(bus, GX_GXFIFO, 0);
    bus_write32(bus, GX_GXFIFO, 0x23); bus_write32(bus, GX_GXFIFO, 0xC0000000u); bus_write32(bus, GX_GXFIFO, 0);
    /* END_VTXS → 光栅化三角形 (127,95)-(255,95)-(127,191) */
    bus_write32(bus, GX_GXFIFO, 0x41);

    /* 21-B9yi(续24)：命令按成本排程执行，测试里显式把引擎时钟推够。 */
    gx_advance(g, 100000u);
    CHECK_EQ("gx fifo in",   g->fb[120 * GX_SCREEN_W + 200], 0x7C00u);
    CHECK_EQ("gx fifo out1", g->fb[160 * GX_SCREEN_W + 200], 0u);
    CHECK_EQ("gx fifo out2", g->fb[100 * GX_SCREEN_W + 120], 0u);

    /* GXSTAT 0x04000603 写 bits30-31：mode=2（FIFO 空触发）→ IF bit21 */
    bus_write8(bus, GX_GXSTAT + 3, 0x80u);
    CHECK_EQ("gx irq mode", g->gxstat & GXSTAT_IRQ_MODE, 0x80000000u);
    CHECK_EQ("gx irq if21",
             nds->io->irq[0].ifl & IO_IF_GXFIFO, IO_IF_GXFIFO);
    bus_write8(bus, GX_GXSTAT + 3, 0x00u);
    CHECK_EQ("gx irq cleared",
             nds->io->irq[0].ifl & IO_IF_GXFIFO, 0u);
}

/* ---- 阶段 19.4 用例：3D 图层合成进 2D 顶屏 ---- */
static void test_gx_layer(nds_t *nds)
{
    gx_t *g = &nds->io->gx;
    uint32_t fb_top[RENDER_SCREEN_W * RENDER_SCREEN_H];
    uint32_t fb_bot[RENDER_SCREEN_W * RENDER_SCREEN_H];

    gx_reset(g);
    gx_raster_tri(g, 10, 10, 100, 10, 10, 100, 0x7C00); /* 红三角，(50,50) 在内 */

    /* 2D 顶屏：mode 5 直色位图（未写 VRAM → 全黑背景） */
    bus_write32(nds->bus, IO_DISPCNT, 5u | DISPCNT_BG2 | (1u << DISPCNT_DISPLAY_MODE_SHIFT));
    bus_write16(nds->bus, IO_BGCNT_BASE + 2 * 2, BGCNT_COLORS_256 | BGCNT_DIRECT_COLOR | (1u << 14));

    /* 21-B9xw：3D 图层不需要 DISP3DCNT 使能位（melonDS 里 bit12/13 是写 1 清零位，
       参考核帧 1900 的 DISP3DCNT=0x0011 时 3D 内容照样显示）→ 三角形直接覆盖顶屏 */
    render_frame(nds->bus, fb_top, fb_bot);
    CHECK_EQ("gx layer on", fb_top[50 * RENDER_SCREEN_W + 50], 0xFFFB0000u);

    /* DISP3DCNT bit13 的写入语义：写 1 清零、写 0 保持（melonDS GPU3D::Write16） */
    bus_write32(nds->bus, IO_DISP3DCNT, DISP3D_ENABLE);
    CHECK_EQ("disp3dcnt w1c", bus_read32(nds->bus, IO_DISP3DCNT) & DISP3D_ENABLE, 0u);
}

/* ---- 阶段 20.1 用例：仿射背景寄存器读写（PA-PD 1.7.8 / X-Y 1.19.8） ---- */
static void test_affine_regs(nds_t *nds)
{
    /* 主引擎 BG2：PA=PB=PC=PD + X/Y 参考点 */
    bus_write16(nds->bus, IO_BG_AFFINE_BASE + 0, 0x0100u);  /* PA=1.0 */
    bus_write16(nds->bus, IO_BG_AFFINE_BASE + 6, 0x0100u);  /* PD=1.0 */
    bus_write32(nds->bus, IO_BG_AFFINE_BASE + 8, 0x00000100u);   /* X=1.0 */
    bus_write32(nds->bus, IO_BG_AFFINE_BASE + 12, 0x00000100u);  /* Y=1.0 */
    CHECK_EQ("aff BG2PA", bus_read16(nds->bus, IO_BG_AFFINE_BASE + 0), 0x0100u);
    CHECK_EQ("aff BG2PD", bus_read16(nds->bus, IO_BG_AFFINE_BASE + 6), 0x0100u);
    CHECK_EQ("aff BG2X",  bus_read32(nds->bus, IO_BG_AFFINE_BASE + 8), 0x00000100u);
    CHECK_EQ("aff BG2Y",  bus_read32(nds->bus, IO_BG_AFFINE_BASE + 12), 0x00000100u);

    /* 负数参考点：bit28-31 被硬件忽略，读回应为 28 位值（0x0FFFFF00） */
    bus_write32(nds->bus, IO_BG_AFFINE_BASE + 8, 0xFFFFFF00u);
    CHECK_EQ("aff BG2X sign", bus_read32(nds->bus, IO_BG_AFFINE_BASE + 8), 0x0FFFFF00u);

    /* BG3 参数（块内偏移 +0x10） */
    bus_write16(nds->bus, IO_BG_AFFINE_BASE + 16, 0x0080u);
    bus_write32(nds->bus, IO_BG_AFFINE_BASE + 24, 0x00000200u);
    CHECK_EQ("aff BG3PA", bus_read16(nds->bus, IO_BG_AFFINE_BASE + 16), 0x0080u);
    CHECK_EQ("aff BG3X",  bus_read32(nds->bus, IO_BG_AFFINE_BASE + 24), 0x00000200u);

    /* 副引擎（偏移 +0x1000） */
    bus_write16(nds->bus, IO_BG_AFFINE_SUB_BASE + 0, 0x0100u);
    bus_write32(nds->bus, IO_BG_AFFINE_SUB_BASE + 8, 0x00000100u);
    CHECK_EQ("aff sub BG2PA", bus_read16(nds->bus, IO_BG_AFFINE_SUB_BASE + 0), 0x0100u);
    CHECK_EQ("aff sub BG2X",  bus_read32(nds->bus, IO_BG_AFFINE_SUB_BASE + 8), 0x00000100u);
}

/* ---- 阶段 20.1 用例：MODE2 仿射背景（逆仿射采样 + 1 字节 map + 缩放） ---- */
static void test_affine_render(nds_t *nds)
{
    uint32_t fb_top[RENDER_SCREEN_W * RENDER_SCREEN_H];
    uint32_t fb_bot[RENDER_SCREEN_W * RENDER_SCREEN_H];

    /* 调色板：0=黑(背景) 1=红 2=绿 */
    bus_write16(nds->bus, BUS_PALETTE_BASE + 0, 0x0000u);
    bus_write16(nds->bus, BUS_PALETTE_BASE + 2, 0x7C00u);
    bus_write16(nds->bus, BUS_PALETTE_BASE + 4, 0x03E0u);

    /* 字符数据（8bpp，字符块0）：tile0=红(索引1)、tile1=绿(索引2) */
    for (int i = 0; i < 64; i++) bus_write8(nds->bus, BUS_VRAM_BASE + 0u + i, 1u);
    for (int i = 0; i < 64; i++) bus_write8(nds->bus, BUS_VRAM_BASE + 64u + i, 2u);

    /* 仿射 map（1 字节/项，size1=32×32）：默认 tile2=透明，map(0,0)=tile0(红)、map(1,0)=tile1(绿) */
    for (int ty = 0; ty < 32; ty++)
        for (int tx = 0; tx < 32; tx++)
            bus_write8(nds->bus, BUS_VRAM_BASE + 0x800u + (uint32_t)(ty * 32 + tx), 2u);
    bus_write8(nds->bus, BUS_VRAM_BASE + 0x800u, 0u);
    bus_write8(nds->bus, BUS_VRAM_BASE + 0x801u, 1u);

    /* mode 2 + BG2 + 显示模式 1；BG2CNT：8bpp + 屏幕块1 + size1(256×256) */
    bus_write32(nds->bus, IO_DISPCNT, 2u | DISPCNT_BG2 | (1u << DISPCNT_DISPLAY_MODE_SHIFT));
    bus_write16(nds->bus, IO_BGCNT_BASE + 2 * 2,
                BGCNT_COLORS_256 | (1u << BGCNT_SCREEN_BASE_SHIFT) | (1u << BGCNT_SCREEN_SIZE_SHIFT));

    /* 单位矩阵 + 参考点(0,0)：屏幕坐标=纹理坐标 */
    bus_write16(nds->bus, IO_BG_AFFINE_BASE + 0, 0x0100u);  /* PA=1.0 */
    bus_write16(nds->bus, IO_BG_AFFINE_BASE + 6, 0x0100u);  /* PD=1.0 */
    bus_write32(nds->bus, IO_BG_AFFINE_BASE + 8, 0u);       /* X=0 */
    bus_write32(nds->bus, IO_BG_AFFINE_BASE + 12, 0u);      /* Y=0 */

    render_frame(nds->bus, fb_top, fb_bot);

    CHECK_EQ("aff id red",      fb_top[0],  0xFFFB0000u);  /* (0,0)→tile0 红 */
    CHECK_EQ("aff id green",    fb_top[8],  0xFF00FB00u);  /* (8,0)→tile1 绿 */
    CHECK_EQ("aff id backdrop", fb_top[16], 0xFF000000u);  /* (16,0)→tile2 透明→黑 */

    /* 2× 缩放：PA=PD=2.0 → 纹理=2×屏幕，红 tile 盖屏幕 0..3、绿盖 4..7、再外透明 */
    bus_write16(nds->bus, IO_BG_AFFINE_BASE + 0, 0x0200u);
    bus_write16(nds->bus, IO_BG_AFFINE_BASE + 6, 0x0200u);
    render_frame(nds->bus, fb_top, fb_bot);

    CHECK_EQ("aff 2x red",      fb_top[0], 0xFFFB0000u);
    CHECK_EQ("aff 2x green",    fb_top[4], 0xFF00FB00u);
    CHECK_EQ("aff 2x backdrop", fb_top[8], 0xFF000000u);
}

/* ---- 阶段 20.2/20.3 用例：混合/亮度/窗口/显示捕获寄存器读写 ---- */
static void test_blend_regs(nds_t *nds)
{
    /* 主引擎：混合三件套 + 主亮度 + 窗口 + 捕获 */
    bus_write16(nds->bus, IO_BLENDCNT, 0x1234u);
    bus_write16(nds->bus, IO_BLENDALPHA, 0x0A05u);   /* EVA=5, EVB=10 */
    bus_write16(nds->bus, IO_BLENDY, 0x0008u);       /* EVY=8 */
    CHECK_EQ("blend BLENDCNT",  bus_read16(nds->bus, IO_BLENDCNT),  0x1234u);
    CHECK_EQ("blend BLDALPHA",  bus_read16(nds->bus, IO_BLENDALPHA), 0x0A05u);
    CHECK_EQ("blend BLDY",      bus_read16(nds->bus, IO_BLENDY),     0x0008u);

    bus_write16(nds->bus, IO_MASTER_BRIGHT, 0x4008u); /* 增亮 + 因子8 */
    CHECK_EQ("blend MASTER_BRIGHT", bus_read16(nds->bus, IO_MASTER_BRIGHT), 0x4008u);

    bus_write16(nds->bus, IO_WIN0H, 0x1020u);         /* X1=0x10, X2=0x20 */
    bus_write16(nds->bus, IO_WININ, 0x1Fu);
    bus_write16(nds->bus, IO_WINOUT, 0x3Fu);
    CHECK_EQ("blend WIN0H",  bus_read16(nds->bus, IO_WIN0H),  0x1020u);
    CHECK_EQ("blend WININ",  bus_read16(nds->bus, IO_WININ),  0x1Fu);
    CHECK_EQ("blend WINOUT", bus_read16(nds->bus, IO_WINOUT), 0x3Fu);

    bus_write32(nds->bus, IO_DISPCAPCNT, 0x80020000u); /* 使能 + 写块1 */
    CHECK_EQ("blend DISPCAPCNT", bus_read32(nds->bus, IO_DISPCAPCNT), 0x80020000u);

    /* 副引擎：BLENDCNT_SUB + MASTER_BRIGHT_SUB */
    bus_write16(nds->bus, IO_BLENDCNT_SUB, 0x0101u);
    bus_write16(nds->bus, IO_MASTER_BRIGHT_SUB, 0x8004u);
    CHECK_EQ("blend sub BLENDCNT",       bus_read16(nds->bus, IO_BLENDCNT_SUB),       0x0101u);
    CHECK_EQ("blend sub MASTER_BRIGHT",  bus_read16(nds->bus, IO_MASTER_BRIGHT_SUB),  0x8004u);
}

/* ---- 阶段 20.2 用例：Alpha 混合 + 增亮/减暗 + 整屏主亮度 ---- */
static void test_blend_render(nds_t *nds)
{
    uint32_t fb_top[RENDER_SCREEN_W * RENDER_SCREEN_H];
    uint32_t fb_bot[RENDER_SCREEN_W * RENDER_SCREEN_H];

    /* 调色板：0=蓝(背景) 1=红 */
    bus_write16(nds->bus, BUS_PALETTE_BASE + 0, 0x001Fu);
    bus_write16(nds->bus, BUS_PALETTE_BASE + 2, 0x7C00u);

    /* mode 0 + BG0 + 显示模式1；BG0CNT 8bpp + 屏幕块1（tilemap 与字符数据分离）；
       tile0 像素(0,0)=索引1，其余透明 */
    bus_write32(nds->bus, IO_DISPCNT, DISPCNT_BG0 | (1u << DISPCNT_DISPLAY_MODE_SHIFT));
    bus_write16(nds->bus, IO_BGCNT_BASE, BGCNT_COLORS_256 | (1u << BGCNT_SCREEN_BASE_SHIFT));
    bus_write8(nds->bus, BUS_VRAM_BASE, 1u);

    /* 混合关闭：红像素直出、透明像素露蓝背景 */
    bus_write16(nds->bus, IO_BLENDCNT, 0u);
    render_frame(nds->bus, fb_top, fb_bot);
    CHECK_EQ("blend off red",      fb_top[0], 0xFFFB0000u);
    CHECK_EQ("blend off backdrop", fb_top[1], 0xFF0000FBu);

    /* 模式1 Alpha：第一目标=BG0、第二目标=BD、EVA=EVB=8 → 红+蓝各半 (R=15,B=15) */
    bus_write16(nds->bus, IO_BLENDCNT,
                (1u << BLEND_MODE_SHIFT) | BLEND_1ST_BG0 | BLEND_2ND_BD);
    bus_write16(nds->bus, IO_BLENDALPHA, 0x0808u);
    render_frame(nds->bus, fb_top, fb_bot);
    CHECK_EQ("alpha blend red+blue", fb_top[0], 0xFF780078u);
    CHECK_EQ("alpha blend backdrop", fb_top[1], 0xFF0000FBu);

    /* 模式2 增亮：第一目标=BG0、EVY=8；改测灰 0x4210（R=G=B=16）→ 23 */
    bus_write16(nds->bus, BUS_PALETTE_BASE + 2, 0x4210u);
    bus_write16(nds->bus, IO_BLENDCNT, (2u << BLEND_MODE_SHIFT) | BLEND_1ST_BG0);
    bus_write16(nds->bus, IO_BLENDY, 8u);
    render_frame(nds->bus, fb_top, fb_bot);
    CHECK_EQ("bright up gray", fb_top[0], 0xFFB8B8B8u);

    /* 模式3 减暗：EVY=8；灰 16 → 8 */
    bus_write16(nds->bus, IO_BLENDCNT, (3u << BLEND_MODE_SHIFT) | BLEND_1ST_BG0);
    render_frame(nds->bus, fb_top, fb_bot);
    CHECK_EQ("bright down gray", fb_top[0], 0xFF404040u);

    /* 关混合，测 MASTER_BRIGHT 增亮：因子8 → 灰 0x80 升到 0xBC（6bit 通道 32→47） */
    bus_write16(nds->bus, IO_BLENDCNT, 0u);
    bus_write16(nds->bus, IO_MASTER_BRIGHT, 0x4008u);
    render_frame(nds->bus, fb_top, fb_bot);
    CHECK_EQ("master bright gray", fb_top[0], 0xFFBCBCBCu);
}

/* ---- 阶段 20.2 用例：显示捕获（顶屏出图 → LCDC VRAM RGB555） ---- */
static void test_capture_render(nds_t *nds)
{
    uint32_t fb_top[RENDER_SCREEN_W * RENDER_SCREEN_H];
    uint32_t fb_bot[RENDER_SCREEN_W * RENDER_SCREEN_H];

    /* 顶屏：mode 5 + BG2 直色位图，位图前两像素 红/蓝 */
    bus_write32(nds->bus, IO_DISPCNT, 5u | DISPCNT_BG2 | (1u << DISPCNT_DISPLAY_MODE_SHIFT));
    bus_write16(nds->bus, IO_BGCNT_BASE + 2 * 2,
                BGCNT_COLORS_256 | BGCNT_DIRECT_COLOR | (1u << 14));
    bus_write16(nds->bus, BUS_VRAM_BASE, 0x7C00u);
    bus_write16(nds->bus, BUS_VRAM_BASE + 2, 0x001Fu);

    /* DISPCAPCNT：使能 + 写块0 + 偏移0 → 目标 0x06800000 */
    bus_write32(nds->bus, IO_DISPCAPCNT, DISPCAP_ENABLE);
    render_frame(nds->bus, fb_top, fb_bot);

    /* 捕获把顶屏 RGB555 写进 LCDC VRAM 窗口（映射到物理 vram[0]） */
    CHECK_EQ("cap px0 red",  bus_read16(nds->bus, BUS_LCDC_VRAM_BASE),     0x7C00u);
    CHECK_EQ("cap px1 blue", bus_read16(nds->bus, BUS_LCDC_VRAM_BASE + 2), 0x001Fu);
    /* 捕获完成后使能位应被清除 */
    CHECK_EQ("cap done clear", bus_read32(nds->bus, IO_DISPCAPCNT) & DISPCAP_ENABLE, 0u);
}

/* ---- 21-B9wj：DISPCNT VRAM 显示模式（bit16-17=2，FFXII 标题顶屏） ---- */
static void test_vram_display_mode(nds_t *nds)
{
    uint32_t fb_top[RENDER_SCREEN_W * RENDER_SCREEN_H];
    uint32_t fb_bot[RENDER_SCREEN_W * RENDER_SCREEN_H];

    /* bank0：写 LCDC 窗口（0x06800000 映射到物理 bank A），前两像素红/蓝 */
    bus_write16(nds->bus, BUS_LCDC_VRAM_BASE + 0x00000u, 0x7C00u);
    bus_write16(nds->bus, BUS_LCDC_VRAM_BASE + 0x00002u, 0x001Fu);
    /* bank1：0x06820000 落到物理 bank B，写绿色 */
    bus_write16(nds->bus, BUS_LCDC_VRAM_BASE + 0x20000u, 0x03E0u);

    /* 主引擎 VRAM 显示模式：2<<16，选 bank0（bit18-19=0） */
    bus_write32(nds->bus, IO_DISPCNT, 2u << DISPCNT_DISPLAY_MODE_SHIFT);
    render_frame(nds->bus, fb_top, fb_bot);
    CHECK_EQ("vramdisp bank0 red",  fb_top[0], 0xFFFB0000u);
    CHECK_EQ("vramdisp bank0 blue", fb_top[1], 0xFF0000FBu);

    /* 选 bank1（bit18-19=1）→ 全屏绿首像素 */
    bus_write32(nds->bus, IO_DISPCNT,
                (2u << DISPCNT_DISPLAY_MODE_SHIFT) | (1u << 18));
    render_frame(nds->bus, fb_top, fb_bot);
    CHECK_EQ("vramdisp bank1 green", fb_top[0], 0xFF00FB00u);
}

/* ---- 阶段 20.3 用例：窗口（WIN0 限定 BG 显示区域） ---- */
static void test_window_render(nds_t *nds)
{
    uint32_t fb_top[RENDER_SCREEN_W * RENDER_SCREEN_H];
    uint32_t fb_bot[RENDER_SCREEN_W * RENDER_SCREEN_H];

    /* 调色板：0=黑(背景) 1=红 */
    bus_write16(nds->bus, BUS_PALETTE_BASE + 0, 0x0000u);
    bus_write16(nds->bus, BUS_PALETTE_BASE + 2, 0x7C00u);

    /* BG0 8bpp：tile0 全红(索引1)，tilemap 全 tile0 → 满屏红 */
    for (int i = 0; i < 64; i++) bus_write8(nds->bus, BUS_VRAM_BASE + i, 1u);
    for (int ty = 0; ty < 32; ty++)
        for (int tx = 0; tx < 32; tx++)
            bus_write16(nds->bus, BUS_VRAM_BASE + 0x800u + (uint32_t)(ty * 32 + tx) * 2u, 0u);
    bus_write16(nds->bus, IO_BGCNT_BASE, BGCNT_COLORS_256 | (1u << BGCNT_SCREEN_BASE_SHIFT));

    /* 窗口：WIN0 = 左上 8×8；窗内只开 BG0，窗外全关 */
    bus_write16(nds->bus, IO_WIN0H, 0x0008u);   /* X1=0, X2=8 */
    bus_write16(nds->bus, IO_WIN0V, 0x0008u);   /* Y1=0, Y2=8 */
    bus_write16(nds->bus, IO_WININ, 0x01u);     /* 窗内: 仅 BG0 */
    bus_write16(nds->bus, IO_WINOUT, 0x00u);    /* 窗外: 全关 */

    /* DISPCNT：mode0 + BG0 + WIN0 使能(bit13) + 显示模式1 */
    bus_write32(nds->bus, IO_DISPCNT,
                DISPCNT_BG0 | (1u << 13) | (1u << DISPCNT_DISPLAY_MODE_SHIFT));
    render_frame(nds->bus, fb_top, fb_bot);

    CHECK_EQ("win in red",    fb_top[0],        0xFFFB0000u); /* (0,0) 窗内 → 红 */
    CHECK_EQ("win out black", fb_top[8],        0xFF000000u); /* (8,0) 窗外 → 黑 */
    CHECK_EQ("win out row",   fb_top[8 * 256],  0xFF000000u); /* (0,8) 窗外 → 黑 */
}

int main(void)
{
#ifdef _WIN32
    /* Windows 控制台：程序日志按 UTF-8 输出，先切输出代码页，避免 936(GBK) 下中文乱码 */
    SetConsoleOutputCP(CP_UTF8);
#endif
    printf("=== test_nds: 统一测试入口 ===\n\n");

    printf("[migrated 2] bus 内存总线读写换算\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) { fprintf(stderr, "nds_create failed\n"); return 1; }
        test_bus_rw(nds);
        nds_destroy(nds);
    }
    printf("\n[migrated 3b] ARM 指令集\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_arm_instructions(nds);
        nds_destroy(nds);
    }
    printf("\n[migrated 4.5] CPU 写 VRAM 出图\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_cpu_draw_vram(nds);
        nds_destroy(nds);
    }
    printf("\n[case 5.2] 清屏\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_clear_screen(nds);
        nds_destroy(nds);
    }
    printf("\n[case 5.3] 画矩形\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_draw_rect(nds);
        nds_destroy(nds);
    }
    printf("\n[case 5.4] 死循环保活\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_dead_loop(nds);
        nds_destroy(nds);
    }
    printf("\n[case 6.2] 中断寄存器 IME/IE/IF\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_irq_regs(nds);
        nds_destroy(nds);
    }
    printf("\n[case 6.3] VBlank 标志\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_vblank_flag(nds);
        nds_destroy(nds);
    }
    printf("\n[case 6.4] 最小 IRQ 检测\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_irq_pending(nds);
        nds_destroy(nds);
    }
    printf("\n[case 6.5] 定时器 0-3\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_timers(nds);
        nds_destroy(nds);
    }
    printf("\n[case 6.6] KEYINPUT\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_keyinput(nds);
        nds_destroy(nds);
    }
    printf("\n[case 6.7] 等 VBlank / 读键程序\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_wait_vblank(nds);
        nds_destroy(nds);
    }
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_key_program(nds);
        nds_destroy(nds);
    }
    printf("\n[case 7.2] DMA0 寄存器\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_dma_regs(nds);
        nds_destroy(nds);
    }
    printf("\n[case 7.3] DMA 立即模式拷贝/填色\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_dma_copy(nds);
        nds_destroy(nds);
    }
    printf("\n[case 7.4] CPU 程序触发 DMA 填 VRAM\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_dma_program(nds);
        nds_destroy(nds);
    }
    printf("\n[case 8.2] 双核都能 step\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_dual_core_step(nds);
        nds_destroy(nds);
    }
    printf("\n[case 8.3] ARM7 WRAM + 入口取指\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_arm7_wram(nds);
        test_arm7_fetch(nds);
        nds_destroy(nds);
    }
    printf("\n[case 21-B1] Shared WRAM 映射（0x03000000 + 0x037F8000 镜像）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_shared_wram(nds);
        nds_destroy(nds);
    }
    printf("\n[case 21-B2] IPCSYNC 同步寄存器（双核数据 + bit13 中断请求）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_ipcsync_regs(nds);
        test_ipcsync_irq(nds);
        nds_destroy(nds);
    }
    printf("\n[case 21-B3] Main RAM 无缓存镜像（0x02400000 起 4MB 别名）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_main_ram_mirror(nds);
        nds_destroy(nds);
    }
    printf("\n[case 21-B8] ARM9 DTCM / ITCM 与主存镜像隔离\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_arm9_tcm(nds);
        nds_destroy(nds);
    }
    printf("\n[case 21-B9c] ARM9 IRQ 槽跳板（DTCM+0x3FFC → handler）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_irq_slot_jump(nds);
        nds_destroy(nds);
    }
    printf("\n[case 21-B9d] CLZ 前导零计数（FFXII IRQ 分发循环依赖）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_clz(nds);
        nds_destroy(nds);
    }
    printf("\n[case 21-B9wk] ARMv5 DSP 乘法（标题位流解码依赖）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_dsp_mul(nds);
        nds_destroy(nds);
    }
    printf("\n[case 21-B9e] ARM BLX Rm 寄存器间接调用（Thumb/ARM 双路径）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_arm_blx_reg(nds);
        nds_destroy(nds);
    }
    printf("\n[case 21-B9s] ARM BLX 立即数（安全区 Thumb SWI 桩入口）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_arm_blx_imm(nds);
        nds_destroy(nds);
    }
    printf("\n[case 21-B9g] 模式私有 r13/r14（System/SVC/IRQ 栈互不覆盖）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_banked_r13_r14(nds);
        nds_destroy(nds);
    }
    printf("\n[case 21-B9h] LDM ^ 的 User 槽语义（任务上下文恢复）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_ldm_user_bank(nds);
        nds_destroy(nds);
    }
    printf("\n[case 21-B9i] FIFO CNT 合并写 + ARM7 IRQ 槽跳板\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_fifo_cnt_combine(nds);
        test_arm7_irq_slot(nds);
        nds_destroy(nds);
    }
    printf("\n[case 21-B9j] TM3 CNT_L 重载值 + 溢出置 IF\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_timer_reload_overflow(nds);
        nds_destroy(nds);
    }
    printf("\n[case 21-B9j] SPI device1 固件最小回读\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_spi_fw_hle(nds);
        nds_destroy(nds);
    }
    printf("\n[case 21-B9k] ARM9 硬件除法/开方寄存器（DIVCNT/SQRT）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_math_div_sqrt(nds);
        nds_destroy(nds);
    }
    printf("\n[case 21-B9a] BIOS SWI 0x0E GetCRC16 (CRC-16/IBM)\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_bios_crc16(nds);
        nds_destroy(nds);
    }
    printf("\n[case 21-B9l] ARM7 BIOS SWI 0x08 SoundBias\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_bios_soundbias(nds);
        nds_destroy(nds);
    }
    printf("\n[case 21-B9wi] ARM7 BIOS 音频查表 SWI 0x1A-0x1D\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_bios_audio_tables(nds);
        nds_destroy(nds);
    }
    printf("\n[case 21-B9n] 电源/启动寄存器（POWCNT1/2 + POSTFLG）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_power_regs(nds);
        nds_destroy(nds);
    }
    printf("\n[case 21-B9u] VCOUNT 只读扫描线（任务调度时钟）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_vcount(nds);
        nds_destroy(nds);
    }
    printf("\n[case 21-B4] ARM BX 奇地址切 Thumb\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_arm_bx_thumb(nds);
        nds_destroy(nds);
    }
    printf("\n[case 21-B7] EXMEMCNT/WRAMCNT + Shared WRAM 双核切分\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_wramcnt_regs(nds);
        test_wramcnt_split(nds);
        nds_destroy(nds);
    }
    printf("\n[case 8.4] 交错调度 2:1\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_interleave(nds);
        nds_destroy(nds);
    }
    printf("\n[case 8.5] 中断按 CPU 分流\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_irq_split(nds);
        nds_destroy(nds);
    }
    printf("\n[case 8.6] IPC FIFO 收发/状态位/中断\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_fifo_basic(nds);
        test_fifo_irq(nds);
        nds_destroy(nds);
    }
    printf("\n[case 8.7] 双核 FIFO 传值 + 底屏体现\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_fifo_program(nds);
        nds_destroy(nds);
    }
    printf("\n[case 9.2] 显示控制寄存器 + 调色板 RAM\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_disp_regs(nds);
        printf("\n[case 21-B9wd] VRAMCNT 动态映射\n");
        test_vramcnt_mapping(nds);
        nds_destroy(nds);
    }
    printf("\n[case 9.3] 直色位图模式渲染\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_bitmap_render(nds);
        nds_destroy(nds);
    }
    printf("\n[case 9.4] Mode 0 tile 图层渲染\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_tile_render(nds);
        nds_destroy(nds);
    }
    printf("\n[case 9.5] 副引擎（Engine B）对称渲染\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_engine_b(nds);
        nds_destroy(nds);
    }
    printf("\n[case 9.6] OBJ 最小（一个 sprite）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_obj_render(nds);
        nds_destroy(nds);
    }
    printf("\n[case 9.7] 自造数据 2D 场景验收\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_2d_scene(nds);
        nds_destroy(nds);
    }
    printf("\n[case 10.2] 移位操作数 LSL/LSR/ASR/ROR\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_shifts(nds);
        nds_destroy(nds);
    }
    printf("\n[case 10.3] 更多数据处理 MVN/BIC/ADC/SBC/RSB/RSC/TST/TEQ/CMN\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_more_dataop(nds);
        nds_destroy(nds);
    }
    printf("\n[case 10.4] MRS/MSR\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_mrs_msr(nds);
        nds_destroy(nds);
    }
    printf("\n[case 10.5] LDM/STM（PUSH/POP）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_block_transfer(nds);
        test_arm7_stm_base_in_list(nds);
        test_arm9_stm_base_in_list(nds);
        test_step_cycles(nds);
        test_timing_events(nds);
        nds_destroy(nds);
    }
    printf("\n[case 10.6] 乘法 MUL/MLA + 长乘\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_mul(nds);
        nds_destroy(nds);
    }
    printf("\n[case 10.7] 字节/半字访存 + 变址\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_byte_halfword(nds);
        nds_destroy(nds);
    }
    printf("\n[case 10.8] SWI 指令\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_swi(nds);
        nds_destroy(nds);
    }
    printf("\n[case 10.9] SWP 交换\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_swp(nds);
        nds_destroy(nds);
    }
    printf("\n[case 10.10] MRC/MCR（CP15 桩）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_coprocessor(nds);
        nds_destroy(nds);
    }
    printf("\n[case 10.11] 综合（栈+SWI+半字搬 VRAM）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_stage10_integration(nds);
        nds_destroy(nds);
    }
    printf("\n[case 11.3] Div 除法 / Sqrt 开方\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_bios_div_sqrt(nds);
        nds_destroy(nds);
    }
    printf("\n[case 11.4] CpuSet / CpuFastSet 搬移与填充\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_bios_cpuset(nds);
        nds_destroy(nds);
    }
    printf("\n[case 11.5] BitUnPack / LZ77 / RL / Huffman 解压\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_bios_decompress(nds);
        nds_destroy(nds);
    }
    printf("\n[case 11.6] Halt / IntrWait / VBlankIntrWait\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_bios_wait(nds);
        nds_destroy(nds);
    }
    printf("\n[case 21-B9wt] ARM7 SWI/Halt 低地址路径（分发器 + swi_complete）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_bios7_low_wait(nds);
        nds_destroy(nds);
    }
    printf("\n[case 21-B9wt] ARM7 Halt 暂停 + IRQ 打断顺序（0x1FB0/0x1FC0）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_bios7_low_halt_irq(nds);
        nds_destroy(nds);
    }
    printf("\n[case 21-B9wu] DMA 按属主核分流 IO（ARM9 显示列表 → GXFIFO）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_dma_owner_routing(nds);
        nds_destroy(nds);
    }
    printf("\n[case 21-B9ww] ARM9 DMA 模式 7（GX FIFO 显示列表）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_dma_gx_fifo_mode(nds);
        nds_destroy(nds);
    }
    printf("\n[case 21-B9wx] NDS RTC 串行协议（0x04000134/0x04000138）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_rtc(nds);
        nds_destroy(nds);
    }
    printf("\n[case 21-B9xi] ARM7 BIOS 保护值 + SOUNDBIAS 语义\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_arm7_biosprot_soundbias(nds);
        nds_destroy(nds);
    }
    printf("\n[case 21-B9xj] DISPSTAT 每核一套（ARM7 独立寄存器 + VCount 匹配）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_dispstat_per_core(nds);
        nds_destroy(nds);
    }
    printf("\n[case 21-B9xq] GBA 扩展槽空槽读值（0xFF open bus）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_gba_slot_open_bus(nds);
        nds_destroy(nds);
    }
    printf("\n[case 21-B9wy] GXSTAT FIFO 状态位（bit25/26）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_gxstat_fifo_bits(nds);
        nds_destroy(nds);
    }
    printf("\n[case 21-B9xc] VBlank 时序（第 192 行起、帧边界结束）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_vblank_timing(nds);
        nds_destroy(nds);
    }
    printf("\n[case 21-B9wu] 无头模式 SPU 时间推进（单发通道清 start 位）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_snd_advance_headless(nds);
        nds_destroy(nds);
    }
    printf("\n[case 11.7] 综合（LZ77 解压 + Div + Sqrt 串行）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_stage11_integration(nds);
        nds_destroy(nds);
    }
    printf("\n[case 12.2] 异常向量：未定义 / 未知 SWI\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_exception_vector_undef(nds);
        test_exception_vector_swi(nds);
        nds_destroy(nds);
    }
    printf("\n[case 12.3] CPSR 模式切换 + SPSR 分槽/恢复\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_spsr_restore(nds);
        nds_destroy(nds);
    }
    printf("\n[case 12.4] CP15 c1 控制向量基址\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_cp15_control(nds);
        nds_destroy(nds);
    }
    printf("\n[case 12.5] IRQ 真实响应：进 handler + 返回\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_irq_response(nds);
        nds_destroy(nds);
    }
    printf("\n[case 13.3] Thumb 数据处理（移位/立即数/ALU/高寄存器）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_thumb_dataproc(nds);
        nds_destroy(nds);
    }
    printf("\n[case 13.4] Thumb 访存（字/字节/半字/SP 相对/寄存器偏移/字面量池）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_thumb_memory(nds);
        nds_destroy(nds);
    }
    printf("\n[case 13.5] Thumb 分支/切换/SWI\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_thumb_branch(nds);
        nds_destroy(nds);
    }
    printf("\n[case 13.5] Thumb PUSH/POP + STMIA/LDMIA\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_thumb_stack(nds);
        nds_destroy(nds);
    }
    printf("\n[case 13.7] Thumb 真码写 VRAM + 调 SWI\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_thumb_vram(nds);
        nds_destroy(nds);
    }
    printf("\n[case 14.2] KEY1 (Blowfish) 块加解密 + 密钥表自检\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_key1_roundtrip(nds);
        nds_destroy(nds);
    }
    printf("\n[case 14.2] 安全区加密→解密往返 + encryObj 魔数\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_key1_secure_area(nds);
        nds_destroy(nds);
    }
    printf("\n[case 14.5] 自制含加密安全区的 ROM 完整装载\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_secure_area_load(nds);
        nds_destroy(nds);
    }
    printf("\n[case 15.2] 卡带命令读（B7 命令 + ROMCTRL 激活 + CARD_DATA）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_cartbus_read(nds);
        nds_destroy(nds);
    }
    printf("\n[case 15.3] DMA 4 通道 + 递减 + VBlank 触发\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_dma_channels(nds);
        nds_destroy(nds);
    }
    printf("\n[case 15.4] 卡带 DMA：DMA 从 ROM 搬数据到 RAM\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_card_dma(nds);
        nds_destroy(nds);
    }
    printf("\n[case 15.5] CPU 程序设 DMA + 激活卡带命令，DMA 读 ROM\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_card_program(nds);
        nds_destroy(nds);
    }
    printf("\n[case 21-B9wr] NDS7 卡带 DMA 触发模式（0x12）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_card_dma7(nds);
        nds_destroy(nds);
    }
    printf("\n[case 16.2] 存档芯片 SPI 状态机（EEPROM）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_save_eeprom(nds);
        nds_destroy(nds);
    }
    printf("\n[case 16.2] Flash 存档（RDID + 擦除 + AND 写语义）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_save_flash(nds);
        nds_destroy(nds);
    }
    printf("\n[case 16.2] 经 AUXSPICNT/AUXSPIDATA 总线读写存档\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_save_spi_regs(nds);
        nds_destroy(nds);
    }
    printf("\n[case 16.3] 存档持久化（.sav 写读回）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_save_persist(nds);
        nds_destroy(nds);
    }
    printf("\n[case 16.4] CPU 程序经 AUXSPICNT/AUXSPIDATA 读写存档\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_save_program(nds);
        nds_destroy(nds);
    }
    printf("\n[case 17.2] TSC 触摸屏 SPI 状态机（12/8 位 + 未按下 + Hold 复位）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_touch_unit(nds);
        nds_destroy(nds);
    }
    printf("\n[case 17.2] 经 SPICNT/SPIDATA 总线读触摸坐标\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_touch_spi_regs(nds);
        nds_destroy(nds);
    }
    printf("\n[case 17.3] CPU 程序经 SPICNT/SPIDATA 读出 X/Y 坐标\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_touch_program(nds);
        nds_destroy(nds);
    }
    printf("\n[case 18.2] 音频寄存器 SOUNDCNT/SOUNDBIAS + 16 通道读写\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_snd_regs(nds);
        nds_destroy(nds);
    }
    printf("\n[case 18.3] 混音器 PCM8/PCM16/ADPCM/PSG + 静音 + 主音量 + 单发停止\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_snd_mix(nds);
        nds_destroy(nds);
    }
    printf("\n[case 18.4] CPU 程序配置通道 0 并读回音频寄存器\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_snd_program(nds);
        nds_destroy(nds);
    }

    printf("\n[case 19.3] 顶点变换（pos×proj → 透视除 → 视口映射）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_gx_transform(nds);
        nds_destroy(nds);
    }
    printf("\n[case 19.4] 三角形软件光栅化\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_gx_raster(nds);
        nds_destroy(nds);
    }
    printf("\n[case 19.5] GXFIFO 命令流 → 变换 → 光栅化出图\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_gx_fifo(nds);
        nds_destroy(nds);
    }
    printf("\n[case 19.4] 3D 图层合成进 2D 顶屏\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_gx_layer(nds);
        nds_destroy(nds);
    }
    printf("\n[case 20.1] 仿射背景寄存器读写\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_affine_regs(nds);
        nds_destroy(nds);
    }
    printf("\n[case 20.1] MODE2 仿射背景渲染（缩放）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_affine_render(nds);
        nds_destroy(nds);
    }
    printf("\n[case 20.2] 混合/亮度/窗口/捕获寄存器读写\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_blend_regs(nds);
        nds_destroy(nds);
    }
    printf("\n[case 20.2] Alpha 混合 + 增亮/减暗 + 主亮度\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_blend_render(nds);
        nds_destroy(nds);
    }
    printf("\n[case 20.2] 显示捕获（顶屏 → LCDC VRAM）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_capture_render(nds);
        nds_destroy(nds);
    }
    printf("\n[case 21-B9wj] DISPCNT VRAM 显示模式（主引擎直读 bank）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_vram_display_mode(nds);
        nds_destroy(nds);
    }
    printf("\n[case 20.3] 窗口（WIN0 限定 BG 显示区域）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_window_render(nds);
        nds_destroy(nds);
    }

    printf("\n=== 共 %d 项检查，%d 项失败 ===\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
