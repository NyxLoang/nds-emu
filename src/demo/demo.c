#include <stdio.h>
#include "nds/nds.h"   /* nds_t / bus_t / bus_write* */
#include "io/io.h"     /* IO_DISPCNT / BGCNT_* / DISPCNT_* 常量 */
#include "snd/snd.h"   /* SND_SOUNDCNT / SND_BASE 等常量 */
#include "demo.h"

/* 阶段 18.4：demo 音源——PCM8 方波（64 采样/周期）循环播放，约 256Hz 提示音。
   写入 Main RAM 空闲区并配置通道 0 循环播放，验证「寄存器→合成→声卡」全链路。 */
static void setup_audio_demo(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE + 0x2000;
    const int cycle = 64;
    for (int i = 0; i < cycle; i++)
        bus_write8(nds->bus, base + (uint32_t)i, (i < cycle / 2) ? 0x7F : 0x81);

    bus_write16(nds->bus, SND_SOUNDCNT, 0x807F);   /* 主使能 + 主音量 127 */
    bus_write16(nds->bus, SND_SOUNDBIAS, 0x0200);  /* 中心偏置 0x200 */

    uint32_t ch = SND_BASE;
    bus_write32(nds->bus, ch + 0x0, 0x8840007Fu);  /* PCM8, loop, pan=64, vol=127, start */
    bus_write32(nds->bus, ch + 0x4, base);
    bus_write16(nds->bus, ch + 0x8, 0x0800);       /* tmr=2048 → 约 256Hz */
    bus_write32(nds->bus, ch + 0xC, cycle / 4);    /* len = 64 字节 = 16 字 */

    printf("18 audio: demo tone configured (PCM8 square wave, ~256 Hz)\n");
    fflush(stdout);
}

/* 写 n 个 RGB555 颜色到调色板基址（每项 2 字节） */
static void write_palette(nds_t *nds, uint32_t base, int n, const uint16_t *colors)
{
    for (int i = 0; i < n; i++)
        bus_write16(nds->bus, base + (uint32_t)i * 2u, colors[i]);
}

/* 把一个 8×8 的 8bpp tile 填成单一调色板索引（64 字节全 = idx） */
static void fill_tile_8bpp(nds_t *nds, uint32_t tile_base, int idx)
{
    for (int i = 0; i < 64; i++)
        bus_write8(nds->bus, tile_base + (uint32_t)i, (uint8_t)idx);
}

/* 阶段 9.7：真 2D 演示——不再把 VRAM 当线性 framebuffer，而是用
   DISPCNT / BGxCNT / tile / tilemap / 调色板 寄存器驱动 2D 引擎出图。 */
static void setup_2d_demo(nds_t *nds)
{
    static const uint16_t pal[8] = {
        0x0000, /* 0 黑（背景/透明露出色） */
        0x7C00, /* 1 红 */
        0x03E0, /* 2 绿 */
        0x001F, /* 3 蓝 */
        0x7FE0, /* 4 黄 */
        0x03FF, /* 5 青 */
        0x7C1F, /* 6 品红 */
        0x7FFF, /* 7 白 */
    };

    /* === 顶屏 Engine A：8bpp tile 棋盘格 + OBJ === */
    /* 主 BG 调色板（0x05000000） */
    write_palette(nds, BUS_PALETTE_BASE, 8, pal);

    /* 4 个 8bpp tile：tile0=红 tile1=绿 tile2=蓝 tile3=黄，字符块 0（0x06000000） */
    fill_tile_8bpp(nds, BUS_VRAM_BASE + 0u * 64u, 1);
    fill_tile_8bpp(nds, BUS_VRAM_BASE + 1u * 64u, 2);
    fill_tile_8bpp(nds, BUS_VRAM_BASE + 2u * 64u, 3);
    fill_tile_8bpp(nds, BUS_VRAM_BASE + 3u * 64u, 4);

    /* tilemap（屏幕块 1 → 0x06000800）：32×32 棋盘格，用 (tx+ty)&3 选 tile */
    for (int ty = 0; ty < 32; ty++)
        for (int tx = 0; tx < 32; tx++)
            bus_write16(nds->bus, BUS_VRAM_BASE + 0x800u + (uint32_t)(ty * 32 + tx) * 2u,
                        (uint16_t)((tx + ty) & 3));

    /* BG0CNT：256 色(bit7) + 屏幕块 1(bit8-12 单位 2KB) */
    bus_write16(nds->bus, IO_BGCNT_BASE,
                BGCNT_COLORS_256 | (1u << BGCNT_SCREEN_BASE_SHIFT));

    /* OBJ：一个白色 8×8 方块（16 色），放在 (120,80) */
    bus_write16(nds->bus, BUS_PALETTE_BASE + 0x200u + 2u, 0x7FFFu); /* OBJ 调色板[1]=白 */
    for (int r = 0; r < 8; r++) {   /* OBJ tile0（4bpp）：全部像素 = 索引1 */
        bus_write8(nds->bus, BUS_VRAM_MAIN_OBJ_BASE + 4u * r + 0u, 0xFFu);
        bus_write8(nds->bus, BUS_VRAM_MAIN_OBJ_BASE + 4u * r + 1u, 0x00u);
        bus_write8(nds->bus, BUS_VRAM_MAIN_OBJ_BASE + 4u * r + 2u, 0x00u);
        bus_write8(nds->bus, BUS_VRAM_MAIN_OBJ_BASE + 4u * r + 3u, 0x00u);
    }
    for (int n = 0; n < 128; n++)   /* 全 0 的 OAM = 原点可见 sprite，先统一禁用 */
        bus_write16(nds->bus, BUS_OAM_BASE + 8u * n, 0x0200u);
    bus_write16(nds->bus, BUS_OAM_BASE + 0u, 80u);   /* 属性0：Y=80 方形 16 色 */
    bus_write16(nds->bus, BUS_OAM_BASE + 2u, 120u);  /* 属性1：X=120 尺寸 8×8 */
    bus_write16(nds->bus, BUS_OAM_BASE + 4u, 0u);    /* 属性2：tile0 调色板0 */

    /* DISPCNT（主）：mode 0 + BG0 + OBJ + 显示模式 1（正常） */
    bus_write32(nds->bus, IO_DISPCNT,
                DISPCNT_BG0 | DISPCNT_OBJ | (1u << DISPCNT_DISPLAY_MODE_SHIFT));

    /* === 底屏 Engine B：8bpp tile 竖条纹（青/品红），副引擎独立资源 === */
    static const uint16_t pal_sub[8] = {
        0x0000, 0x03FF, 0x7C1F, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    };
    write_palette(nds, BUS_PALETTE_BASE + 0x400u, 8, pal_sub); /* 副 BG 调色板 */

    /* 副 BG 图形窗口 0x06200000（物理 vram[0x40000]）：tile0=青 tile1=品红 */
    fill_tile_8bpp(nds, BUS_VRAM_SUB_BG_BASE + 0u * 64u, 1);
    fill_tile_8bpp(nds, BUS_VRAM_SUB_BG_BASE + 1u * 64u, 2);

    /* 副 tilemap（屏幕块 1 → 0x06200800）：按列奇偶选 tile，形成竖条纹 */
    for (int ty = 0; ty < 32; ty++)
        for (int tx = 0; tx < 32; tx++)
            bus_write16(nds->bus, BUS_VRAM_SUB_BG_BASE + 0x800u + (uint32_t)(ty * 32 + tx) * 2u,
                        (uint16_t)(tx & 1));

    /* 副 BG0CNT：256 色 + 屏幕块 1 */
    bus_write16(nds->bus, IO_BGCNT_SUB_BASE,
                BGCNT_COLORS_256 | (1u << BGCNT_SCREEN_BASE_SHIFT));

    /* DISPCNT_SUB：mode 0 + BG0 + 显示模式 1 */
    bus_write32(nds->bus, IO_DISPCNT_SUB,
                DISPCNT_BG0 | (1u << DISPCNT_DISPLAY_MODE_SHIFT));

    printf("9 display: true 2D demo configured (top=tiled checkerboard+OBJ, bottom=stripes)\n");
    fflush(stdout);
}

void demo_setup(nds_t *nds)
{
    setup_2d_demo(nds);
    setup_audio_demo(nds);
}
