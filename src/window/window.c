#include <stdio.h>
#include <SDL_ttf.h>
#include "window.h"

static SDL_Window *g_window = NULL;
static SDL_Renderer *g_renderer = NULL;
static int g_scale = 1;

int window_init(char *err, size_t errsz)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        snprintf(err, errsz, "SDL_Init failed: %s", SDL_GetError());
        return -1;
    }
    if (TTF_Init() != 0) {
        snprintf(err, errsz, "TTF_Init failed: %s", TTF_GetError());
        SDL_Quit();
        return -1;
    }

    g_window = SDL_CreateWindow(
        "nds-emu", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WIN_W * g_scale, WIN_H * g_scale,
        SDL_WINDOW_SHOWN);
    if (g_window == NULL) {
        snprintf(err, errsz, "SDL_CreateWindow failed: %s", SDL_GetError());
        TTF_Quit();
        SDL_Quit();
        return -1;
    }

    g_renderer = SDL_CreateRenderer(g_window, -1, 0);
    if (g_renderer == NULL) {
        snprintf(err, errsz, "SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(g_window);
        g_window = NULL;
        TTF_Quit();
        SDL_Quit();
        return -1;
    }

    /* 像素游戏画面（未来 framebuffer）用最近邻，UI 文字按物理分辨率独立绘制 */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    return 0;
}

void window_shutdown(void)
{
    if (g_renderer != NULL)
        SDL_DestroyRenderer(g_renderer);
    if (g_window != NULL)
        SDL_DestroyWindow(g_window);
    TTF_Quit();
    SDL_Quit();
}

SDL_Renderer *window_get_renderer(void)
{
    return g_renderer;
}

int window_get_scale(void)
{
    return g_scale;
}

void window_set_scale(int scale)
{
    g_scale = scale;
    SDL_SetWindowSize(g_window, WIN_W * scale, WIN_H * scale);
}

int window_handle_event(const SDL_Event *e)
{
    if (e->type == SDL_QUIT)
        return 1;
    if (e->type == SDL_KEYDOWN && e->key.keysym.sym == SDLK_ESCAPE)
        return 1;
    return 0;
}
