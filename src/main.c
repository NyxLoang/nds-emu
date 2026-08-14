#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include <SDL.h>
#include <SDL_ttf.h>
#include "window/window.h"
#include "menu/menu.h"
#include "nds/nds.h"
#include "bus/bus.h"
#include "cpu/cpu.h"
#include "cpu/exec.h"
#include "io/io.h"
#include "ppu/ppu.h"
#include "cart/cart.h"

/* 阶段 4.4：主机直接往 VRAM 的 framebuffer 写测试图，验证「VRAM→纹理→窗口」管线。
   顶屏画 6 色横带 + 白边框（可辨识的测试图），底屏填纯蓝。 */
static void fill_test_pattern(nds_t *nds)
{
    const uint16_t colors[6] = {
        0x7C00, /* 红 */
        0x7FE0, /* 黄 */
        0x03E0, /* 绿 */
        0x03FF, /* 青 */
        0x001F, /* 蓝 */
        0x7C1F, /* 品红 */
    };
    /* 顶屏：每 32 行一条色带（192 行 / 6 色） */
    for (int y = 0; y < PPU_SCREEN_H; y++) {
        for (int x = 0; x < PPU_SCREEN_W; x++) {
            uint16_t c = colors[y / 32];
            /* 最外一圈画白框，便于肉眼确认屏的边界 */
            if (x == 0 || y == 0 || x == PPU_SCREEN_W - 1 || y == PPU_SCREEN_H - 1)
                c = 0x7FFF;
            /* 地址 = VRAM 基址 + 顶屏偏移 + (y*宽 + x)*2 字节（RGB555 每像素 2 字节） */
            uint32_t addr = BUS_VRAM_BASE + PPU_VRAM_TOP_OFFSET
                          + (uint32_t)(y * PPU_SCREEN_W + x) * 2u;
            bus_write16(nds->bus, addr, c);
        }
    }
    /* 底屏：纯蓝 */
    for (int y = 0; y < PPU_SCREEN_H; y++) {
        for (int x = 0; x < PPU_SCREEN_W; x++) {
            uint32_t addr = BUS_VRAM_BASE + PPU_VRAM_BOTTOM_OFFSET
                          + (uint32_t)(y * PPU_SCREEN_W + x) * 2u;
            bus_write16(nds->bus, addr, 0x001F);
        }
    }
    printf("4 display: host-filled test pattern into VRAM\n");
    fflush(stdout);
}

/* 阶段 4.5：不再靠主机 memset，而是把一段 ARM 程序装入 Main RAM，
   让模拟 CPU 自己往 VRAM 写图，再交给 ppu 刷新上屏。
   程序画：顶屏黄色十字（水平线 y=96 + 竖线 x=128），底屏整屏绿色。
   地址构造用的是 3b 阶段练过的 imm8 旋转立即数与 STR 偏移。 */
static void selftest_4_cpu_draw(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE;

    /* 手工汇编（指令字编码注释见 docs/04-cpu-loop.md 与 cpulog.md）：
       程序布局：0x00-0x74，B self 停机在 0x74。
       十字要对称，两臂都必须 2px：
       - 横线画 y=95、y=96 两行（STR 一次写 2 像素 → 行内成对推进）；
       - 竖线画 x=128、129 两列，从 y=0 一直画到 y=191（终点 = 起点 + 0x18000，
         越过顶屏最后一行，保证下半段完整）。
       STR 是 32 位写 = 2 个 RGB555 像素，颜色值放进两个字半
       （0xFF00FF00 / 0x03E003E0）保证 2 像素同色、线条实心。 */
    const uint32_t drawprog[] = {
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
    for (size_t i = 0; i < sizeof drawprog / sizeof drawprog[0]; i++)
        bus_write32(nds->bus, base + 4 * i, drawprog[i]);

    /* 批量跑 LDR/STR 循环会有数万条指令，先关掉逐条日志避免刷屏 */
    exec_set_trace(0);
    cpu_reset(nds->cpu, base);
    int steps = 0;
    int max_steps = 1 << 20; /* 顶屏行/列 + 底屏整屏，安全上限 100 万步 */
    while (steps++ < max_steps && nds->cpu->r[15] != base + 0x74)
        cpu_step(nds->cpu);
    exec_set_trace(1);

    /* 回读 CPU 实际写过的像素验证真写进去了：
       顶屏 (x=128,y=0) 竖线起点 = 0x06000100 → 黄 0xFF00；
       顶屏 (x=128,y=191) 竖线末行 = 0x06017F00 → 黄（验证下半段完整）；
       底屏 (0,0) = 0x06018000 → 绿 0x03E0。 */
    uint32_t top_px = bus_read16(nds->bus, BUS_VRAM_BASE + PPU_VRAM_TOP_OFFSET + 0x100u);
    uint32_t top_end_px = bus_read16(nds->bus, BUS_VRAM_BASE + PPU_VRAM_TOP_OFFSET + 0x17F00u);
    uint32_t bot_px = bus_read16(nds->bus, BUS_VRAM_BASE + PPU_VRAM_BOTTOM_OFFSET);
    printf("4 display: CPU drew to VRAM, top(x=128,y=0)=%04X top(x=128,y=191)=%04X bot(0,0)=%04X (steps=%d)\n",
           top_px, top_end_px, bot_px, steps);
    if (top_px != 0xFF00u || top_end_px != 0xFF00u || bot_px != 0x03E0u)
        printf("4 display: WARNING unexpected VRAM readback\n");
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

    /* 阶段 4.3：ppu 把 VRAM framebuffer 转成 SDL 纹理（渲染器来自 window） */
    ppu_t *ppu = ppu_create(nds, renderer);
    if (ppu == NULL) {
        fprintf(stderr, "ppu_create failed\n");
        menu_shutdown();
        window_shutdown();
        nds_destroy(nds);
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

            /* 阶段 8.3：把 ARM7 镜像逐字节写进 ARM7 WRAM（0x03800000）。
               与 ARM9 装载对称：逐字节走 bus_write8，再读回验证。 */
            if (hdr.arm7.offset + hdr.arm7.size > cart->size) {
                printf("image : arm7 out of file range\n");
            } else if (hdr.arm7.size > BUS_ARM7_WRAM_SIZE) {
                printf("image : arm7 too large for WRAM (%u bytes)\n",
                       hdr.arm7.size);
            } else {
                for (uint32_t i = 0; i < hdr.arm7.size; i++)
                    bus_write8(nds->bus, hdr.arm7.ram + i,
                               cart->data[hdr.arm7.offset + i]);

                uint32_t readback7 = bus_read32(nds->bus, hdr.arm7.ram);
                printf("image : loaded %u bytes into ARM7 WRAM @ %08X, first word readback %08X\n",
                       hdr.arm7.size, hdr.arm7.ram, readback7);

                /* 阶段 8.3：ARM7 镜像就位，让第二颗 CPU 从 ARM7 入口开始执行 */
                cpu_reset(nds->cpu7, hdr.arm7.entry);
                printf("cpu7  : reset PC=%08X\n", hdr.arm7.entry);
            }
        }
        fflush(stdout);
#ifdef _WIN32
        LocalFree(wargv);
#endif
    }

    /* 显示 demo（阶段 5 起指令级断言已迁入 tests/test_nds.c，此处只保留出图效果）：
       阶段 4.4：主机直接往 VRAM 写测试图（顶屏 6 色带 + 白框，底屏纯蓝）；
       阶段 4.5：模拟 CPU 自己跑一段写 VRAM 的测试码（顶屏黄十字 + 底屏纯绿）。 */
    if (nds->cpu != NULL) {
        fill_test_pattern(nds);
        selftest_4_cpu_draw(nds);

        /* demo 完毕后恢复 mini.nds 入口，让主循环继续死循环空转 */
        cpu_reset(nds->cpu, 0x02000800);
        fflush(stdout);
    }

    /* 阶段 4.6：默认 2× 缩放启动（菜单下拉可切回 1x/2x） */
    window_set_scale(2);

    /* 每帧执行的 CPU 步数（阶段 3a 起固定 N 步；阶段 4 已能出图） */
    const int steps_per_frame = 8;

    /* 阶段 6：SDL 按键 → NDS 按键状态（pressed 位=1 表示按下，按下=0 是 NDS 读值）。
       映射见下方 switch；KEY_* 常量来自 io/key.h。 */
    uint16_t keys_pressed = 0;

    /* 阶段 6.4：IRQ pending 只在首次出现时打印一次，避免每帧刷屏 */
    int irq_logged = 0;

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
            } else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
                /* 阶段 6.6：把 SDL 键码映射成 NDS 键位并写入 io 模块 */
                int down = (e.type == SDL_KEYDOWN);
                uint16_t bit = 0;
                switch (e.key.keysym.sym) {
                case SDLK_z:       bit = KEY_A;     break;
                case SDLK_x:       bit = KEY_B;     break;
                case SDLK_s:       bit = KEY_X;     break;
                case SDLK_d:       bit = KEY_Y;     break;
                case SDLK_a:       bit = KEY_L;     break;
                case SDLK_f:       bit = KEY_R;     break;
                case SDLK_RETURN:  bit = KEY_START; break;
                case SDLK_BACKSPACE: bit = KEY_SELECT; break;
                case SDLK_UP:      bit = KEY_UP;    break;
                case SDLK_DOWN:    bit = KEY_DOWN;  break;
                case SDLK_LEFT:    bit = KEY_LEFT;  break;
                case SDLK_RIGHT:   bit = KEY_RIGHT; break;
                default:           bit = 0;         break;
                }
                if (bit != 0) {
                    if (down) keys_pressed |= bit;
                    else      keys_pressed &= (uint16_t)~bit;
                    io_set_keyinput(nds->io, keys_pressed);
                }
            }
        }

        /* 阶段 3a 起：每帧推进固定 N 步 CPU（mini.nds 是死循环，PC 原地打转）。
           阶段 4 的画面由上面 CPU 写 VRAM 的测试码产生，这里继续空转。
           阶段 8.4：双核按 ARM9:ARM7 = 2:1 交错调度（i%3==2 时跑 ARM7）。
           每 60 周期打一次状态，避免死循环时刷屏。 */
        if (nds->cpu != NULL && nds->cpu7 != NULL) {
            for (int i = 0; i < steps_per_frame; i++) {
                if (i % 3 == 2)
                    cpu_step(nds->cpu7);
                else
                    cpu_step(nds->cpu);
            }
            if (nds->cpu->cycles % 60u == 0) {
                printf("cpu: frame done, ARM9 PC=%08X cycles=%llu | ARM7 PC=%08X cycles=%llu\n",
                       nds->cpu->r[15], (unsigned long long)nds->cpu->cycles,
                       nds->cpu7->r[15], (unsigned long long)nds->cpu7->cycles);
                fflush(stdout);
            }
        }

        /* 阶段 6.3：指令计数近似产生 VBlank——每帧步数跑完即视为一帧结束。
           真机是显示硬件自动置位，这里模拟同一件事。 */
        io_set_vblank(nds->io);

        /* 阶段 6.4：最小 IRQ 检测（不进异常向量，仅观察挂起）。
           真机上这时 CPU 会被叫走跑 handler；本阶段只打印一次。 */
        if (io_irq_pending(nds->io) && !irq_logged) {
            printf("irq: IRQ pending (VBlank IF=%08X IE=%08X IME=%u)\n",
                   bus_read32(nds->bus, IO_IF_ADDR),
                   bus_read32(nds->bus, IO_IE_ADDR),
                   bus_read32(nds->bus, IO_IME_ADDR) & 1u);
            fflush(stdout);
            irq_logged = 1;
        }

        /* 清屏（物理坐标） */
        SDL_SetRenderDrawColor(renderer, 45, 45, 45, 255);
        SDL_RenderClear(renderer);

        menu_render_bar(renderer, scale);

        /* 阶段 4：每帧从 VRAM framebuffer 读图 → 转 RGB888 → 上传纹理 → 画双屏。
           顶屏在菜单栏下，底屏紧随其后，按当前 scale 缩放。 */
        ppu_render(ppu, scale);

        /* 下拉菜单（画在游戏区之上） */
        menu_render_dropdown(renderer, scale);

        SDL_RenderPresent(renderer);
    }

    menu_shutdown();
    window_shutdown();
    ppu_destroy(ppu);
    nds_destroy(nds);
    cart_free(cart);
    return 0;
}
