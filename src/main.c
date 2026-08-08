#include <stdio.h>
#include <SDL.h>
#include <SDL_ttf.h>
#include "window/window.h"
#include "menu/menu.h"
#include "nds/nds.h"
#include "cart/cart.h"

int main(int argc, char *argv[])
{
    /* 阶段 1：argv[1] 是 .nds 文件路径；暂未提供时为 NULL，跳过装载 */
    const char *rom_path = (argc > 1) ? argv[1] : NULL;

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
        cart = cart_load(rom_path, err, sizeof err);
        if (cart == NULL) {
            fprintf(stderr, "%s\n", err);
            menu_shutdown();
            window_shutdown();
            return 1;
        }
        printf("cart: loaded %s (%zu bytes)\n", rom_path, cart->size);
        fflush(stdout);

        cart_header_t hdr;
        if (cart_parse_header(cart, &hdr) != 0) {
            fprintf(stderr, "cart: header too short (%zu bytes)\n", cart->size);
            fflush(stdout);
        } else {
            printf("arm9 offset=%08X entry=%08X ram=%08X size=%08X\n",
                   hdr.arm9_offset, hdr.arm9_entry, hdr.arm9_ram, hdr.arm9_size);
            fflush(stdout);
        }
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
