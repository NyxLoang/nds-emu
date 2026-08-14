#ifndef NDS_EMU_PPU_H
#define NDS_EMU_PPU_H

#include <stdint.h>
#include <SDL.h>
#include "nds/nds.h"
#include "render.h" /* RENDER_SCREEN_W/H + render_frame（纯渲染，SDL 无关） */

/* 创建 / 销毁 ppu（持有 SDL 纹理与 RGB888 主机缓冲）。渲染器由 window 提供。 */
typedef struct ppu ppu_t;

ppu_t *ppu_create(nds_t *nds, SDL_Renderer *renderer);
void ppu_destroy(ppu_t *ppu);

/* 每帧调用：render_frame 按 DISPCNT/BGxCNT 出图 → 上传 SDL 纹理。
   顶屏纹理画在菜单栏下方，底屏画在其正下方（按 scale 缩放）。 */
void ppu_render(ppu_t *ppu, int scale);

#endif /* NDS_EMU_PPU_H */
