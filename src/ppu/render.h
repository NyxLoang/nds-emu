#ifndef NDS_EMU_PPU_RENDER_H
#define NDS_EMU_PPU_RENDER_H

#include <stdint.h>

struct bus; /* 前向声明：渲染只经 bus 读显存/寄存器，不依赖 SDL */

/* NDS 单屏分辨率（顶屏 / 底屏一致） */
#define RENDER_SCREEN_W 256
#define RENDER_SCREEN_H 192

/* 把 NDS 双屏渲染成两块 256×192 的 RGB888 主机缓冲（纯逻辑，无 SDL，可测试）。
   fb_top / fb_bot 各需 RENDER_SCREEN_W * RENDER_SCREEN_H 个 uint32_t。
   渲染流程：读 DISPCNT/BGxCNT → 按模式画位图或 tile 图层 → 合成。 */
void render_frame(struct bus *bus, uint32_t *fb_top, uint32_t *fb_bot);

#endif /* NDS_EMU_PPU_RENDER_H */
