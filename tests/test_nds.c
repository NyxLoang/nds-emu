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

    printf("\n=== 共 %d 项检查，%d 项失败 ===\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
