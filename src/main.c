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

/* 阶段 1 过渡：ARM9 镜像的 RAM 缓冲区（4MB = NDS Main RAM 大小）。
   阶段 2 换成 bus 管理，这里先满足「拷入后能读回」的验收。 */
static unsigned char arm9_ram[4 * 1024 * 1024];

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

    /* 阶段 1：装载 .nds（若有）；仅打印文件大小，尚未解析头 */
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

            /* 把 ARM9 镜像从文件拷入 RAM 缓冲区（暂用裸数组，阶段 2 换 bus） */
            if (hdr.arm9.offset + hdr.arm9.size > cart->size) {
                printf("image : arm9 out of file range\n");
            } else if (hdr.arm9.size > sizeof arm9_ram) {
                printf("image : arm9 too large for RAM buffer (%u bytes)\n",
                       hdr.arm9.size);
            } else {
                memcpy(arm9_ram, cart->data + hdr.arm9.offset, hdr.arm9.size);
                printf("image : copied %u bytes to RAM, first bytes: ",
                       hdr.arm9.size);
                for (size_t i = 0; i < 4 && i < hdr.arm9.size; i++)
                    printf("%02X ", arm9_ram[i]);
                printf("\n");
            }
        }
        fflush(stdout);
#ifdef _WIN32
        LocalFree(wargv);
#endif
    }

    /* 一台空机器：整机状态容器，后续微步往里装 bus / cpu / ppu */
    nds_t *nds = nds_create();
    if (nds == NULL) {
        fprintf(stderr, "nds_create failed\n");
        menu_shutdown();
        window_shutdown();
        return 1;
    }

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
