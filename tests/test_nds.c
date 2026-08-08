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
    static const uint32_t drawprog[] = {
        /* 0x00 */ 0xE3A00406, /* MOV r0, #0x06000000       r0 = 顶屏基址 */
        /* 0x04 */ 0xE2800CC0, /* ADD r0, r0, #0xC000       r0 = 0x0600C000（y=96 行起点） */
        /* 0x08 */ 0xE3A014FF, /* MOV r1, #0xFF000000       黄色高半字（0xFF ROR 8） */
        /* 0x0C */ 0xE3811CFF, /* ORR r1, r1, #0xFF00       → r1=0xFF00FF00（2 像素同色） */
        /* 0x10 */ 0xE2803C02, /* ADD r3, r0, #0x200        r3 = 行终点 0x0600C200 */
        /* 0x14 */ 0xE5801000, /* STR r1, [r0]              写 2 像素黄 */
        /* 0x18 */ 0xE2800004, /* ADD r0, r0, #4            前进 2 像素 */
        /* 0x1C */ 0xE1500003, /* CMP r0, r3                行画完了吗 */
        /* 0x20 */ 0x1AFFFFFB, /* BNE 0x14                  未到终点继续 */
        /* 0x24 */ 0xE3A00406, /* MOV r0, #0x06000000       r0 = 顶屏基址 */
        /* 0x28 */ 0xE2800C01, /* ADD r0, r0, #0x100        r0 = 0x06000100（x=128 列起点） */
        /* 0x2C */ 0xE2804CC0, /* ADD r4, r0, #0xC000       r4 = 列终点 0x0600C100 */
        /* 0x30 */ 0xE5801000, /* STR r1, [r0]              写 2 像素黄 */
        /* 0x34 */ 0xE2800C02, /* ADD r0, r0, #0x200        跳到下一行同一列 */
        /* 0x38 */ 0xE1500004, /* CMP r0, r4                列画完了吗 */
        /* 0x3C */ 0x1AFFFFFB, /* BNE 0x30                  未到终点继续 */
        /* 0x40 */ 0xE3A00406, /* MOV r0, #0x06000000       r0 = 顶屏基址 */
        /* 0x44 */ 0xE2801A18, /* ADD r1, r0, #0x18000      r1 = 底屏基址 0x06018000 */
        /* 0x48 */ 0xE2815A18, /* ADD r5, r1, #0x18000      r5 = 底屏终点 0x06030000 */
        /* 0x4C */ 0xE3A0263E, /* MOV r2, #0x03E00000       绿色高半字（0x3E ROR 12） */
        /* 0x50 */ 0xE3822E3E, /* ORR r2, r2, #0x03E0       → r2=0x03E003E0（2 像素同色） */
        /* 0x54 */ 0xE5812000, /* STR r2, [r1]              写 2 像素绿 */
        /* 0x58 */ 0xE2811004, /* ADD r1, r1, #4            前进 2 像素 */
        /* 0x5C */ 0xE1510005, /* CMP r1, r5                底屏画完了吗 */
        /* 0x60 */ 0x1AFFFFFB, /* BNE 0x54                  未到终点继续 */
        /* 0x64 */ 0xEAFFFFFE, /* B self（停机） */
    };

    int steps = run_program(nds, base, drawprog,
                            sizeof drawprog / sizeof drawprog[0],
                            base, base + 0x64, 1 << 20);

    /* 回读 CPU 实际写过的像素：顶屏 (x=128,y=0) 竖线起点 → 黄 0xFF00；
       底屏 (0,0) → 绿 0x03E0。 */
    uint32_t top_px_val = bus_read16(nds->bus, BUS_VRAM_BASE
                                     + TEST_VRAM_TOP_OFFSET + 0x100u);
    uint32_t bot_px_val = bus_read16(nds->bus, BUS_VRAM_BASE
                                     + TEST_VRAM_BOTTOM_OFFSET);
    CHECK_EQ("top cross (128,0)", top_px_val, 0xFF00u);
    CHECK_EQ("bottom green (0,0)", bot_px_val, 0x03E0u);
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

    printf("\n=== 共 %d 项检查，%d 项失败 ===\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
