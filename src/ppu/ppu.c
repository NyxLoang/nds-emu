#include <stdlib.h>
#include <SDL.h>
#include "ppu.h"
#include "render.h"
#include "bus/bus.h"
#include "window/window.h"

struct ppu {
    nds_t *nds;
    SDL_Renderer *renderer;
    SDL_Texture *tex_top;     /* 顶屏 RGB888 纹理，256×192 */
    SDL_Texture *tex_bot;     /* 底屏 RGB888 纹理，256×192 */
    uint32_t fb_top[RENDER_SCREEN_W * RENDER_SCREEN_H]; /* 顶屏 RGB888 主机缓冲 */
    uint32_t fb_bot[RENDER_SCREEN_W * RENDER_SCREEN_H]; /* 底屏 RGB888 主机缓冲 */
};

/* 把 RGB888 主机缓冲上传为 SDL 纹理并绘制在屏幕上的指定矩形。 */
static void draw_fb(SDL_Texture *tex, const uint32_t *fb,
                    SDL_Renderer *r, int x, int y, int scale)
{
    SDL_UpdateTexture(tex, NULL, fb, RENDER_SCREEN_W * sizeof(uint32_t));
    SDL_Rect dst = { x, y, RENDER_SCREEN_W * scale, RENDER_SCREEN_H * scale };
    SDL_RenderCopy(r, tex, NULL, &dst);
}

ppu_t *ppu_create(nds_t *nds, SDL_Renderer *renderer)
{
    ppu_t *p = calloc(1, sizeof *p);
    if (p == NULL)
        return NULL;
    p->nds = nds;
    p->renderer = renderer;
    p->tex_top = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                   SDL_TEXTUREACCESS_STREAMING,
                                   RENDER_SCREEN_W, RENDER_SCREEN_H);
    p->tex_bot = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                   SDL_TEXTUREACCESS_STREAMING,
                                   RENDER_SCREEN_W, RENDER_SCREEN_H);
    if (p->tex_top == NULL || p->tex_bot == NULL) {
        ppu_destroy(p);
        return NULL;
    }
    return p;
}

void ppu_destroy(ppu_t *p)
{
    if (p == NULL)
        return;
    if (p->tex_top) SDL_DestroyTexture(p->tex_top);
    if (p->tex_bot) SDL_DestroyTexture(p->tex_bot);
    free(p);
}

/* 每帧：让纯渲染器按 DISPCNT/BGxCNT 出图 → 上传纹理 → 绘制。
   顶屏在菜单栏 (MENU_H*scale) 之下，底屏紧随其后。 */
void ppu_render(ppu_t *p, int scale)
{
    render_frame(p->nds->bus, p->fb_top, p->fb_bot);
    draw_fb(p->tex_top, p->fb_top, p->renderer, 0, MENU_H * scale, scale);
    draw_fb(p->tex_bot, p->fb_bot, p->renderer, 0,
            (MENU_H + RENDER_SCREEN_H) * scale, scale);
}
