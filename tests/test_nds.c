/* 阶段 5：统一测试入口。
   把所有此前散落在 main.c 的临时自测（bus 读写、ARM 指令集、CPU 写 VRAM）
   迁到这里，并用同样的「装程序 → 跑 CPU → 断言」方式新增验收用例：
   清屏、画矩形、死循环保活。
   本文件只链 ndscore 核心库（不依赖 SDL/窗口），可直接运行或经 ctest 收集。 */

#include <stdio.h>
#include <stdint.h>

#include "nds/nds.h"
#include "bus/bus.h"
#include "cpu/cpu.h"
#include "cpu/exec.h"
#include "io/io.h"
#include "io/disp.h"
#include "ppu/render.h"

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

/* 顶屏像素(x,y) 的颜色字（RGB555）。地址 = VRAM 基址 + (y*宽 + x)*2 字节。 */
static uint16_t top_px(nds_t *nds, int x, int y)
{
    return bus_read16(nds->bus, BUS_VRAM_BASE + TEST_VRAM_TOP_OFFSET
                      + (uint32_t)(y * TEST_SCREEN_W + x) * 2u);
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

    /* IO 桩：写忽略、读返回 0 */
    bus_write8(bus, BUS_IO_BASE + 0x04, 0x11);
    CHECK_EQ("io stub read8", bus_read8(bus, BUS_IO_BASE + 0x04), 0x00);
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

    /* IE：使能 VBlank（bit3）与其他位 */
    bus_write32(nds->bus, IO_IE_ADDR, 0x00000008u);
    CHECK_EQ("IE write/read", bus_read32(nds->bus, IO_IE_ADDR), 0x00000008u);

    /* IF：硬件（io_set_vblank）置位后能读到 */
    io_set_vblank(nds->io);
    CHECK_EQ("IF vblank set", bus_read32(nds->bus, IO_IF_ADDR), 0x00000008u);

    /* IF 写 1 清除：只清被写 1 的位，写 0 的位不受影响 */
    io_set_vblank(nds->io);              /* 重新挂起 */
    bus_write32(nds->bus, IO_IF_ADDR, 0xFFFFFFFFu); /* 全部清掉 */
    CHECK_EQ("IF clear-all", bus_read32(nds->bus, IO_IF_ADDR), 0x00000000u);

    io_set_vblank(nds->io);
    bus_write32(nds->bus, IO_IF_ADDR, 0x00000008u); /* 只清 VBlank 位 */
    CHECK_EQ("IF clear vblank only", bus_read32(nds->bus, IO_IF_ADDR), 0x00000000u);
}

/* ---- 6.3 用例：指令计数产生 VBlank（IF bit3 位置位） ---- */
static void test_vblank_flag(nds_t *nds)
{
    /* 每帧跑完固定步数后 io_set_vblank，IF 的 bit3 应为 1 */
    io_set_vblank(nds->io);
    CHECK_EQ("IF bit3 (VBlank) set", bus_read32(nds->bus, IO_IF_ADDR), IO_IF_VBLANK);
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
    /* 未按键：全部位为 1（含高 4 位恒 1） */
    io_set_keyinput(nds->io, 0x0000);
    CHECK_EQ("no key pressed", bus_read16(nds->bus, IO_KEYINPUT_ADDR), 0xF000u | 0x0FFFu);

    /* 按下 A（bit0）：读值该位变 0 */
    io_set_keyinput(nds->io, KEY_A);
    CHECK_EQ("A pressed -> bit0=0", bus_read16(nds->bus, IO_KEYINPUT_ADDR), 0xF000u | 0x0FFEu);

    /* 按下 UP + B */
    io_set_keyinput(nds->io, KEY_UP | KEY_B);
    CHECK_EQ("UP+B pressed", bus_read16(nds->bus, IO_KEYINPUT_ADDR), 0xF000u | 0x0FBDu);
}

/* ---- 6.7 用例：等 VBlank（CPU 轮询 IF，置位后写 VRAM） ----
   程序：读 IF → 检查 bit3 → 没置位就继续转圈 → 置位后把黄色写进 VRAM。
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
        /* 0x10 */ 0xE2112008, /* ANDS r2, r1, #8           VBlank 位（bit3）→ Z 标志 */
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
    bus_write32(nds->bus, IO_IE_ADDR,  0x00000008u);

    nds->bus->active_is_arm7 = 1;
    CHECK_EQ("arm7 IME independent", bus_read32(nds->bus, IO_IME_ADDR), 0x00000000u);
    CHECK_EQ("arm7 IE independent",  bus_read32(nds->bus, IO_IE_ADDR),  0x00000000u);

    /* ARM9 视角读回应得自己写过的值 */
    nds->bus->active_is_arm7 = 0;
    CHECK_EQ("arm9 IME", bus_read32(nds->bus, IO_IME_ADDR), 0x00000001u);
    CHECK_EQ("arm9 IE",  bus_read32(nds->bus, IO_IE_ADDR),  0x00000008u);
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
    /* VRAM 固定窗口：副 BG 0x06200000 应映射到物理 bank C（vram[0x40000]） */
    bus_write16(nds->bus, BUS_VRAM_SUB_BG_BASE, 0x001Fu);
    CHECK_EQ("vram sub bg win", bus_read16(nds->bus, BUS_VRAM_SUB_BG_BASE), 0x001Fu);
    CHECK_EQ("vram sub bg phys", bus_read16(nds->bus, BUS_VRAM_BASE + BUS_VRAM_SUB_BG_PHYS), 0x001Fu);
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

    CHECK_EQ("bitmap px0 red",  fb_top[0], 0xFFF80000u);
    CHECK_EQ("bitmap px1 blue", fb_top[1], 0xFF0000F8u);
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
    CHECK_EQ("tile4 px00 red",      fb_top[0],   0xFFF80000u);
    CHECK_EQ("tile4 px10 backdrop", fb_top[1],   0xFF00F800u);
    CHECK_EQ("tile4 row1 backdrop", fb_top[256], 0xFF00F800u);

    /* 切 256 色（bit7=1），tile0(8bpp) 只让像素(0,0)=索引5 */
    bus_write16(nds->bus, IO_BGCNT_BASE, 0x0180u);
    bus_write8(nds->bus, BUS_VRAM_BASE, 5u);

    render_frame(nds->bus, fb_top, fb_bot);
    CHECK_EQ("tile8 px00 red",      fb_top[0],   0xFFF80000u);
    CHECK_EQ("tile8 px10 backdrop", fb_top[1],   0xFF00F800u);
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
    CHECK_EQ("engB bmp red", fb_bot[0], 0xFFF80000u);

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
    CHECK_EQ("engB tile green",    fb_bot[0], 0xFF00F800u); /* 索引1 → 绿 */
    CHECK_EQ("engB tile backdrop", fb_bot[1], 0xFFF80000u); /* 透明 → 红背景 */
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

    CHECK_EQ("obj px at (20,10) red", fb_top[10 * RENDER_SCREEN_W + 20], 0xFFF80000u);
    CHECK_EQ("obj px transparent",     fb_top[10 * RENDER_SCREEN_W + 21], 0xFF000000u);
    CHECK_EQ("obj px outside",         fb_top[0], 0xFF000000u);

    /* 切 256 色 OBJ：a0 bit13=1，OBJ 图形 8bpp 像素(0,0)=索引5，OBJ 调色板[5]=绿 */
    bus_write16(nds->bus, BUS_PALETTE_BASE + 0x200 + 10, 0x03E0u);
    bus_write16(nds->bus, BUS_OAM_BASE + 0, 0x200Au); /* bit13=256色 */
    bus_write8(nds->bus, BUS_VRAM_MAIN_OBJ_BASE + 0, 5u);
    bus_write8(nds->bus, BUS_VRAM_MAIN_OBJ_BASE + 1, 0x00u);

    render_frame(nds->bus, fb_top, fb_bot);
    CHECK_EQ("obj 256c px green", fb_top[10 * RENDER_SCREEN_W + 20], 0xFF00F800u);
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

    CHECK_EQ("scene top tile0 red",    fb_top[0],              0xFFF80000u);
    CHECK_EQ("scene top tile1 green",  fb_top[8],              0xFF00F800u);
    CHECK_EQ("scene top transparent",  fb_top[16],             0xFF000000u); /* map[2]=透明→黑背景 */
    CHECK_EQ("scene obj blue",         fb_top[16 * 256 + 16],  0xFF0000F8u);
    CHECK_EQ("scene bot cyan",         fb_bot[0],              0xFF00F8F8u);
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

/* ---- 10.4 用例：MRS/MSR 读/写 CPSR/SPSR（含字段掩码与模式位保护） ---- */
static void test_mrs_msr(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE;
    arm_cpu_t *cpu = nds->cpu;

    static const uint32_t prog[] = {
        /* 0x00 */ 0xE10F0000, /* MRS r0, CPSR         r0 = cpsr */
        /* 0x04 */ 0xE3A010DF, /* MOV r1, #0xDF        r1 = 0xDF */
        /* 0x08 */ 0xE121F001, /* MSR CPSR_c, r1       cpsr 控制字节(bit7-5)写入 */
        /* 0x0C */ 0xE10F2000, /* MRS r2, CPSR         r2 = 新 cpsr */
        /* 0x10 */ 0xE14F3000, /* MRS r3, SPSR         r3 = spsr */
        /* 0x14 */ 0xE3A04055, /* MOV r4, #0x55        r4 = 0x55 */
        /* 0x18 */ 0xE16FF004, /* MSR SPSR, r4         spsr = 0x55 */
        /* 0x1C */ 0xEAFFFFFE, /* B self */
    };

    /* 预置：N=0 Z=0 C=1 V=0 + 模式 0x13(SVC) */
    cpu->cpsr = 0x20000013u;
    cpu->spsr = 0x12345678u;

    run_program(nds, base, prog, sizeof prog / sizeof prog[0],
                base, base + 0x1C, 32);

    CHECK_EQ("mrs r0 = cpsr", cpu->r[0], 0x20000013u);
    /* MSR CPSR_c：bit7-5 写入 0xDF→0xC0，模式位(bit4-0)保留 0x13 → 控制字节 0xD3 */
    CHECK_EQ("mrs r2 control byte", cpu->r[2] & 0xFFu, 0xD3u);
    CHECK_EQ("mrs r2 flags preserved", cpu->r[2] & 0xF0000000u, 0x20000000u);
    CHECK_EQ("mrs r2 mode preserved", cpu->r[2] & 0x1Fu, 0x13u);
    CHECK_EQ("mrs r3 = spsr", cpu->r[3], 0x12345678u);
    CHECK_EQ("msr spsr written", cpu->spsr, 0x55u);
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

/* ---- 10.8 用例：SWI 记录软件中断号、继续执行 ---- */
static void test_swi(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE;
    static const uint32_t prog[] = {
        /* 0x00 */ 0xEF000123, /* SWI 0x123 */
        /* 0x04 */ 0xE3A00005, /* MOV r0, #5（应继续执行） */
        /* 0x08 */ 0xEAFFFFFE, /* B self */
    };
    run_program(nds, base, prog, sizeof prog / sizeof prog[0],
                base, base + 0x08, 16);

    CHECK_EQ("swi num recorded", nds->cpu->swi_num, 0x123u);
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
        /* 0x28 */ 0xEF00001A, /* SWI 0x1A */
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
    CHECK_EQ("itg swi_num", nds->cpu->swi_num, 0x1Au);
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
    /* 置 IE & IF 后 Halt 返回 */
    nds->io->irq[0].ie = IO_IF_VBLANK;
    nds->io->irq[0].ifl = IO_IF_VBLANK;
    cpu_step(nds->cpu);
    cpu_step(nds->cpu);
    exec_set_trace(1);
    CHECK_EQ("halt r0", nds->cpu->r[0], 7u);
    CHECK_EQ("halt PC", nds->cpu->r[15], base + 8);
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

int main(void)
{
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
    printf("\n[case 11.7] 综合（LZ77 解压 + Div + Sqrt 串行）\n");
    {
        nds_t *nds = nds_create();
        if (nds == NULL) return 1;
        test_stage11_integration(nds);
        nds_destroy(nds);
    }

    printf("\n=== 共 %d 项检查，%d 项失败 ===\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
