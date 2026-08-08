#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include <SDL.h>
#include <SDL_ttf.h>
#include "window/window.h"
#include "menu/menu.h"
#include "nds/nds.h"
#include "cart/cart.h"

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
            }
        }
        fflush(stdout);
#ifdef _WIN32
        LocalFree(wargv);
#endif
    }

    /* === 阶段 2 自测：bus 读写换算 ===
       写一个字节到 Main RAM 内地址，读回应一致；未映射区间读应返回 0。
       16/32 位验证小端拼拆：写 0xABCD 后应读回 0xABCD，且内存字节序为 CD AB。
       （阶段 5 起换成正式单元测试，此块为各微步的临时验证入口） */
    bus_write8(nds->bus, BUS_MAIN_RAM_BASE + 0x100, 0xAB);
    printf("bus test: write8 0xAB -> read8 0x%02X\n",
           bus_read8(nds->bus, BUS_MAIN_RAM_BASE + 0x100));
    printf("bus test: unmapped read8 0x%02X\n",
           bus_read8(nds->bus, 0x0F000000u));

    bus_write16(nds->bus, BUS_MAIN_RAM_BASE + 0x200, 0xABCD);
    printf("bus test: write16 0xABCD -> read16 0x%04X, bytes %02X %02X\n",
           bus_read16(nds->bus, BUS_MAIN_RAM_BASE + 0x200),
           bus_read8(nds->bus, BUS_MAIN_RAM_BASE + 0x200),
           bus_read8(nds->bus, BUS_MAIN_RAM_BASE + 0x201));

    bus_write32(nds->bus, BUS_MAIN_RAM_BASE + 0x300, 0x12345678);
    printf("bus test: write32 0x12345678 -> read32 0x%08X, bytes %02X %02X %02X %02X\n",
           bus_read32(nds->bus, BUS_MAIN_RAM_BASE + 0x300),
           bus_read8(nds->bus, BUS_MAIN_RAM_BASE + 0x300),
           bus_read8(nds->bus, BUS_MAIN_RAM_BASE + 0x301),
           bus_read8(nds->bus, BUS_MAIN_RAM_BASE + 0x302),
           bus_read8(nds->bus, BUS_MAIN_RAM_BASE + 0x303));

    bus_write16(nds->bus, BUS_VRAM_BASE + 0x10, 0xBEEF);
    printf("bus test: VRAM write16 0xBEEF -> read16 0x%04X\n",
           bus_read16(nds->bus, BUS_VRAM_BASE + 0x10));

    bus_write8(nds->bus, BUS_IO_BASE + 0x04, 0x11); /* IO 桩写应忽略、不崩 */
    printf("bus test: IO stub read8 0x%02X\n",
           bus_read8(nds->bus, BUS_IO_BASE + 0x04));
    fflush(stdout);

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
