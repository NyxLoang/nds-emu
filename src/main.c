#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include <SDL.h>
#include <SDL_ttf.h>
#include "window/window.h"
#include "menu/menu.h"
#include "nds/nds.h"
#include "cpu/cpu.h"
#include "cart/cart.h"

/* 阶段 3b 自测：把一段手工汇编的 ARM 指令序列写进 Main RAM，逐条执行后断言
   寄存器值。覆盖 MOV/ADD/SUB/CMP/LDR/STR/B/BL/BX 每类指令（验收见计划表）。 */

static int selftest_failures = 0;

/* 断言工具：got 与 want 不一致则计一次失败并打印。 */
static void check(const char *name, uint32_t got, uint32_t want)
{
    if (got == want) {
        printf("3b test: %-14s PASS (0x%08X)\n", name, got);
    } else {
        printf("3b test: %-14s FAIL got=0x%08X want=0x%08X\n", name, got, want);
        selftest_failures++;
    }
}

static void selftest_3b(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE;

    /* 手工汇编的测试程序（指令字来自 ARM 编码手册，见 cpulog.md 附表）。
       布局：程序 0x00-0x54，数据 0x68，BL 子程序 0x70。 */
    const uint32_t prog[] = {
        /* 0x00 */ 0xE3A00005, /* MOV r0, #5            → r0=5        */
        /* 0x04 */ 0xE3A01007, /* MOV r1, #7            → r1=7        */
        /* 0x08 */ 0xE0802001, /* ADD r2, r0, r1        → r2=12       */
        /* 0x0C */ 0xE0423000, /* SUB r3, r2, r0        → r3=7        */
        /* 0x10 */ 0xE2804008, /* ADD r4, r0, #8        → r4=13       */
        /* 0x14 */ 0xE3500005, /* CMP r0, #5            → Z=1         */
        /* 0x18 */ 0xE1500001, /* CMP r0, r1            → N=1 C=0     */
        /* 0x1C */ 0xE3A05402, /* MOV r5, #0x02000000（0x02 ROR 8）→ r5=Main RAM 基址 */
        /* 0x20 */ 0xE3A0607F, /* MOV r6, #0x7F         → r6=0x7F     */
        /* 0x24 */ 0xE5856060, /* STR r6, [r5, #0x60]   → mem[0x02000060]=0x7F */
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

    cpu_reset(nds->cpu, base);
    printf("3b test: --- begin instruction self-test ---\n");
    for (int i = 0; i < 64; i++) {
        cpu_step(nds->cpu);
        if (nds->cpu->r[15] == base + 0x54)
            break; /* 到达停机死循环，测试程序执行完毕 */
    }

    /* 逐项断言 */
    check("r0 (MOV imm)",  nds->cpu->r[0],  0x00000005u);
    check("r1 (MOV imm)",  nds->cpu->r[1],  0x00000007u);
    check("r2 (ADD reg)",  nds->cpu->r[2],  0x0000000Cu);
    check("r3 (SUB reg)",  nds->cpu->r[3],  0x00000007u);
    check("r4 (ADD imm)",  nds->cpu->r[4],  0x0000000Du);
    check("r5 (MOV addr)", nds->cpu->r[5],  0x02000000u);
    check("r7 (LDR mem)",  nds->cpu->r[7],  0x0000007Fu);
    check("r8 (LDR off)",  nds->cpu->r[8],  0x12345678u);
    check("r9 (BL ret)",   nds->cpu->r[9],  0x00000022u);
    check("r10 (BL sub)",  nds->cpu->r[10], 0x00000033u);
    check("r11 (B tgt)",   nds->cpu->r[11], 0x00000011u);
    check("lr (BL saved)", nds->cpu->r[14], base + 0x50u);

    /* 3b.9 位运算程序（0x80 起）：
       AND r7, r5, r6 / ORR r8, r5, r6 / EOR r9, r5, r6 */
    const uint32_t bitop[] = {
        /* 0x80 */ 0xE3A0500F, /* MOV r5, #0x0F        → r5=0x0F */
        /* 0x84 */ 0xE3A06033, /* MOV r6, #0x33        → r6=0x33 */
        /* 0x88 */ 0xE0057006, /* AND r7, r5, r6       → r7=0x0F & 0x33 = 0x03 */
        /* 0x8C */ 0xE1858006, /* ORR r8, r5, r6       → r8=0x0F | 0x33 = 0x3F */
        /* 0x90 */ 0xE0259006, /* EOR r9, r5, r6       → r9=0x0F ^ 0x33 = 0x3C */
        /* 0x94 */ 0xEAFFFFFE, /* B self（停机） */
    };
    for (size_t i = 0; i < sizeof bitop / sizeof bitop[0]; i++)
        bus_write32(nds->bus, base + 0x80 + 4 * i, bitop[i]);
    cpu_reset(nds->cpu, base + 0x80);
    for (int i = 0; i < 16; i++) {
        cpu_step(nds->cpu);
        if (nds->cpu->r[15] == base + 0x94)
            break;
    }
    check("r7 (AND)",  nds->cpu->r[7],  0x00000003u);
    check("r8 (ORR)",  nds->cpu->r[8],  0x0000003Fu);
    check("r9 (EOR)",  nds->cpu->r[9],  0x0000003Cu);

    /* 3b.10 综合：用纯机器码把一个 RGB555 颜色字写进 VRAM，再读回。
       相当于模拟 CPU 直接驱动显存的第一步（阶段 4 用这套数据出图）。 */
    const uint32_t vramprog[] = {
        /* 0x98 */ 0xE3A00406, /* MOV r0, #0x06000000（0x06 ROR 8）→ VRAM 基址 */
        /* 0x9C */ 0xE3A01C7C, /* MOV r1, #0x7C00（0x7C ROR 24）→ 红色（RGB555） */
        /* 0xA0 */ 0xE5801000, /* STR r1, [r0]           → VRAM[0] = 0x7C00 */
        /* 0xA4 */ 0xE5902000, /* LDR r2, [r0]           → r2 = 读回 */
        /* 0xA8 */ 0xEAFFFFFE, /* B self（停机） */
    };
    for (size_t i = 0; i < sizeof vramprog / sizeof vramprog[0]; i++)
        bus_write32(nds->bus, base + 0x98 + 4 * i, vramprog[i]);
    cpu_reset(nds->cpu, base + 0x98);
    for (int i = 0; i < 16; i++) {
        cpu_step(nds->cpu);
        if (nds->cpu->r[15] == base + 0xA8)
            break;
    }
    check("r2 (VRAM readback)", nds->cpu->r[2], 0x00007C00u);
    check("VRAM[0] color word", bus_read32(nds->bus, BUS_VRAM_BASE), 0x00007C00u);

    /* CMP 更新了 flags：CMP r0,#5 → Z=1；CMP r0,r1 → N=1 C=0（V 未知，只看 N/Z/C） */
    uint32_t cpsr = nds->cpu->cpsr;
    printf("3b test: cpsr=0x%08X (N=%d Z=%d C=%d V=%d)\n",
           cpsr, (cpsr >> 31) & 1, (cpsr >> 30) & 1, (cpsr >> 29) & 1, (cpsr >> 28) & 1);
    if ((cpsr & (1u << 31)) && !(cpsr & (1u << 30)) && !(cpsr & (1u << 29))) {
        printf("3b test: flags (N=1 Z=0 C=0)   PASS\n");
    } else {
        printf("3b test: flags (N=1 Z=0 C=0)   FAIL cpsr=%08X\n", cpsr);
        selftest_failures++;
    }

    if (selftest_failures == 0)
        printf("3b test: --- ALL PASS ---\n");
    else
        printf("3b test: --- %d FAILURES ---\n", selftest_failures);
    fflush(stdout);
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    /* Windows：用宽字符命令行拿路径，避免窄 argv 在非 UTF-8 代码页下中文乱码 */
#ifdef _WIN32
    int wargc = 0;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    const wchar_t *rom_path = (wargc > 1) ? wargv[1] : NULL;
#else
    const char *rom_path = (argc > 1) ? argv[1] : NULL;
#endif

    char err[256];
    if (window_init(err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    SDL_Renderer *renderer = window_get_renderer();

    if (menu_init(renderer) != 0) {
        fprintf(stderr, "menu_init failed, TTF error: %s\n", TTF_GetError());
        window_shutdown();
        return 1;
    }

    /* 一台空机器：整机状态容器，bus 已挂入。
       必须先建 nds，后续装载镜像时才有可写的 Main RAM。 */
    nds_t *nds = nds_create();
    if (nds == NULL) {
        fprintf(stderr, "nds_create failed\n");
        menu_shutdown();
        window_shutdown();
        return 1;
    }

    /* 阶段 2：装载 .nds（若有）→ 解析头 → 把 ARM9 镜像写进 bus 的 Main RAM，
       再从 bus 读回验证「装载-读回」闭环成立。 */
    cart_t *cart = NULL;
    if (rom_path != NULL) {
#ifdef _WIN32
        cart = cart_load_w(rom_path, err, sizeof err);
#else
        cart = cart_load(rom_path, err, sizeof err);
#endif
        if (cart == NULL) {
            fprintf(stderr, "%s\n", err);
            menu_shutdown();
            window_shutdown();
            nds_destroy(nds);
            return 1;
        }
        printf("=== NDS cartridge ===\n");
        printf("file  : %ls (%zu bytes)\n", rom_path, cart->size);
        fflush(stdout);

        cart_header_t hdr;
        if (cart_parse_header(cart, &hdr) != 0) {
            printf("header: too short (%zu bytes), cannot parse\n", cart->size);
        } else {
            printf("arm9  : offset=%08X entry=%08X ram=%08X size=%08X\n",
                   hdr.arm9.offset, hdr.arm9.entry, hdr.arm9.ram, hdr.arm9.size);
            printf("arm7  : offset=%08X entry=%08X ram=%08X size=%08X\n",
                   hdr.arm7.offset, hdr.arm7.entry, hdr.arm7.ram, hdr.arm7.size);

            /* 把 ARM9 镜像逐字节写进 bus 的 Main RAM（地址 = 头里的 ram 字段）。
               逐字节走 bus_write8，让每个字节都经过地址换算，验证 bus 语义。 */
            if (hdr.arm9.offset + hdr.arm9.size > cart->size) {
                printf("image : arm9 out of file range\n");
            } else if (hdr.arm9.size > BUS_MAIN_RAM_SIZE) {
                printf("image : arm9 too large for Main RAM (%u bytes)\n",
                       hdr.arm9.size);
            } else {
                for (uint32_t i = 0; i < hdr.arm9.size; i++)
                    bus_write8(nds->bus, hdr.arm9.ram + i,
                               cart->data[hdr.arm9.offset + i]);

                /* 读回验证：镜像头 4 字节应从 bus 读出且与文件一致。
                   直接读前 32 位，验证小端拼拆与地址换算双正确。 */
                uint32_t readback = bus_read32(nds->bus, hdr.arm9.ram);
                printf("image : loaded %u bytes into Main RAM @ %08X, first word readback %08X\n",
                       hdr.arm9.size, hdr.arm9.ram, readback);

                /* 阶段 3a：镜像就位后，让 CPU 从 ARM9 入口开始执行 */
                cpu_reset(nds->cpu, hdr.arm9.entry);
                printf("cpu   : reset PC=%08X\n", hdr.arm9.entry);
            }
        }
        fflush(stdout);
#ifdef _WIN32
        LocalFree(wargv);
#endif
    }

    /* === 阶段 3b 自测：指令序列（MOV/ADD/SUB/CMP/LDR/STR/B/BL/BX）===
       把手工汇编的测试程序装入 Main RAM 执行，断言各寄存器值。
       （阶段 5 起换成正式单元测试，此块为各微步的临时验证入口） */
    if (nds->cpu != NULL) {
        selftest_3b(nds);
        /* 自测完毕后恢复 mini.nds 入口，让主循环继续死循环空转 */
        cpu_reset(nds->cpu, 0x02000800);
        fflush(stdout);
    }

    /* 每帧执行的 CPU 步数（阶段 3a：固定 N 步，画面仍黑） */
    const int steps_per_frame = 8;

    int quit = 0;
    while (!quit) {
        int scale = window_get_scale();

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (window_handle_event(&e)) {
                quit = 1;
            } else if (e.type == SDL_MOUSEBUTTONDOWN &&
                       e.button.button == SDL_BUTTON_LEFT) {
                /* 无 logical size，事件坐标即物理坐标；窗口尺寸恒为
                   WIN_W*scale × WIN_H*scale，除以 scale 得逻辑坐标 */
                int new_scale = menu_handle_click(e.button.x / scale,
                                                  e.button.y / scale);
                if (new_scale >= 1) {
                    window_set_scale(new_scale);
                    scale = new_scale;
                }
            }
        }

        /* 阶段 3a：每帧推进固定 N 步 CPU（mini.nds 是死循环，PC 原地打转）。
           阶段 4 起用 VRAM 内容出图，这里先只推进、画面仍黑。
           每 60 帧打一次状态，避免死循环时刷屏。 */
        if (nds->cpu != NULL) {
            for (int i = 0; i < steps_per_frame; i++)
                cpu_step(nds->cpu);
            if (nds->cpu->cycles % 60u == 0) {
                printf("cpu: frame done, PC=%08X cycles=%llu\n",
                       nds->cpu->r[15], (unsigned long long)nds->cpu->cycles);
                fflush(stdout);
            }
        }

        /* 清屏（物理坐标） */
        SDL_SetRenderDrawColor(renderer, 45, 45, 45, 255);
        SDL_RenderClear(renderer);

        menu_render_bar(renderer, scale);

        /* 游戏画面区（占位：顶/底屏两种纯色，物理坐标；将来画双屏 framebuffer） */
        SDL_Rect top_screen = { 0, MENU_H * scale, SCREEN_W * scale, SCREEN_H * scale };
        SDL_SetRenderDrawColor(renderer, 24, 90, 200, 255); /* 顶屏：蓝 */
        SDL_RenderFillRect(renderer, &top_screen);

        SDL_Rect bot_screen = { 0, (MENU_H + SCREEN_H) * scale, SCREEN_W * scale, SCREEN_H * scale };
        SDL_SetRenderDrawColor(renderer, 24, 160, 60, 255); /* 底屏：绿 */
        SDL_RenderFillRect(renderer, &bot_screen);

        /* 下拉菜单（画在游戏区之上） */
        menu_render_dropdown(renderer, scale);

        SDL_RenderPresent(renderer);
    }

    menu_shutdown();
    window_shutdown();
    nds_destroy(nds);
    cart_free(cart);
    return 0;
}
