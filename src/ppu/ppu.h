#ifndef NDS_EMU_PPU_H
#define NDS_EMU_PPU_H

#include <stdint.h>
#include <SDL.h>
#include "nds/nds.h"

/* framebuffer 约定（见 docs/05-framebuffer.md）：
   顶屏 = VRAM 起始 256×192 RGB555；底屏 = VRAM + 0x18000。 */
#define PPU_SCREEN_W 256
#define PPU_SCREEN_H 192
#define PPU_VRAM_TOP_OFFSET    0x00000u   /* 顶屏 framebuffer 在 VRAM 里的偏移 */
#define PPU_VRAM_BOTTOM_OFFSET 0x18000u   /* 底屏 framebuffer 在 VRAM 里的偏移 */

/* 创建 / 销毁 ppu（持有 SDL 纹理与 RGB888 主机缓冲）。渲染器由 window 提供。 */
typedef struct ppu ppu_t;

ppu_t *ppu_create(nds_t *nds, SDL_Renderer *renderer);
void ppu_destroy(ppu_t *ppu);

/* 每帧调用：读 VRAM 的两个 framebuffer（RGB555）→ 转 RGB888 → 上传 SDL 纹理。
   顶屏纹理画在菜单栏下方，底屏画在其正下方（按 scale 缩放）。 */
void ppu_render(ppu_t *ppu, int scale);

#endif /* NDS_EMU_PPU_H */
