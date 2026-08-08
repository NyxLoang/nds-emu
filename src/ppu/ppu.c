#include <stdlib.h>
#include <SDL.h>
#include "ppu.h"
#include "bus/bus.h"
#include "window/window.h"

struct ppu {
    nds_t *nds;
    SDL_Renderer *renderer;
    SDL_Texture *tex_top;     /* 顶屏 RGB888 纹理，256×192 */
    SDL_Texture *tex_bot;     /* 底屏 RGB888 纹理，256×192 */
    uint32_t fb_top[PPU_SCREEN_W * PPU_SCREEN_H]; /* 顶屏 RGB888 主机缓冲 */
    uint32_t fb_bot[PPU_SCREEN_W * PPU_SCREEN_H]; /* 底屏 RGB888 主机缓冲 */
};

/* RGB555 → RGB888：把 5 位通道左移 3 位扩成 8 位（0x1F→0xF8）。
   颜色字布局 bit15(0) bit14-10 R bit9-5 G bit4-0 B。 */
static uint32_t rgb555_to_888(uint16_t c)
{
    uint32_t r = (c >> 10) & 0x1Fu;  /* 红色 5 位 */
    uint32_t g = (c >> 5)  & 0x1Fu;  /* 绿色 5 位 */
    uint32_t b = c & 0x1Fu;          /* 蓝色 5 位 */
    return 0xFF000000u | (r << 19) | (g << 11) | (b << 3);
    /* 0xFF 不透明 A；R/G/B 左移 3 位放到 8 位通道的对应位置 */
}

/* 把 VRAM 里的 RGB555 线性 framebuffer 转成 RGB888 主机缓冲。
   vram_base 是 VRAM 里的偏移；每像素 16 位，按小端读。 */
static void blit_vram_to_fb(const bus_t *bus, uint32_t vram_base,
                            uint32_t *fb, int w, int h)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            /* 地址 = VRAM 基址 + 偏移 + 像素下标*2（RGB555 每像素 2 字节） */
            uint32_t addr = BUS_VRAM_BASE + vram_base + (uint32_t)(y * w + x) * 2u;
            uint16_t c = (uint16_t)bus_read16(bus, addr);
            fb[y * w + x] = rgb555_to_888(c);
        }
    }
}

/* 把 RGB888 主机缓冲上传为 SDL 纹理并绘制在屏幕上的指定矩形。 */
static void draw_fb(SDL_Texture *tex, const uint32_t *fb,
                    SDL_Renderer *r, int x, int y, int scale)
{
    SDL_UpdateTexture(tex, NULL, fb, PPU_SCREEN_W * sizeof(uint32_t));
    SDL_Rect dst = { x, y, PPU_SCREEN_W * scale, PPU_SCREEN_H * scale };
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
                                   PPU_SCREEN_W, PPU_SCREEN_H);
    p->tex_bot = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                   SDL_TEXTUREACCESS_STREAMING,
                                   PPU_SCREEN_W, PPU_SCREEN_H);
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

/* 每帧：读 VRAM 两个 framebuffer → 转 RGB888 → 上传纹理 → 绘制。
   顶屏在菜单栏 (MENU_H*scale) 之下，底屏紧随其后。 */
void ppu_render(ppu_t *p, int scale)
{
    blit_vram_to_fb(p->nds->bus, PPU_VRAM_TOP_OFFSET, p->fb_top,
                    PPU_SCREEN_W, PPU_SCREEN_H);
    blit_vram_to_fb(p->nds->bus, PPU_VRAM_BOTTOM_OFFSET, p->fb_bot,
                    PPU_SCREEN_W, PPU_SCREEN_H);
    draw_fb(p->tex_top, p->fb_top, p->renderer, 0, MENU_H * scale, scale);
    draw_fb(p->tex_bot, p->fb_bot, p->renderer, 0,
            (MENU_H + PPU_SCREEN_H) * scale, scale);
}
