#include "render.h"
#include <stdio.h>
#include <stdlib.h>
#include "bus/bus.h"
#include "io/disp.h"
#include "io/io.h"   /* 3D 图层：读 bus->io->gx 的帧缓冲（阶段 19.4） */
#include "gx/gx.h"

/* BG 图形内存基址：主引擎 0x06000000，副引擎 0x06200000（阶段 9 固定窗口） */
#define BG_GFX_MAIN 0x06000000u
#define BG_GFX_SUB  0x06200000u

/* 调色板基址：主 0x05000000，副 0x05000400 */
#define BG_PAL_MAIN 0x05000000u
#define BG_PAL_SUB  0x05000400u

/* OBJ 相关基址（阶段 9.6）：属性内存 OAM、图形内存、OBJ 调色板（主/副各一套）。 */
#define OAM_MAIN   0x07000000u
#define OAM_SUB    0x07000400u
#define OBJ_GFX_MAIN 0x06400000u
#define OBJ_GFX_SUB  0x06600000u
#define OBJ_PAL_MAIN 0x05000200u
#define OBJ_PAL_SUB  0x05000600u

/* 显示捕获目标：LCDC 分配 VRAM 窗口（阶段 20.2 最小实现，映射到 VRAM bank A） */
#define LCDC_VRAM   0x06800000u

#define PX_COUNT (RENDER_SCREEN_W * RENDER_SCREEN_H)

/* RGB555 → RGB888：5 位通道左移 3 位扩成 8 位（0x1F→0xF8），A 通道固定不透明 */
static uint32_t rgb555_to_888(uint16_t c)
{
    uint32_t r = (c >> 10) & 0x1Fu;
    uint32_t g = (c >> 5)  & 0x1Fu;
    uint32_t b = c & 0x1Fu;
    /* 21-B9yc：5bit→8bit 换算对齐 melonDS 的 6bit 中间路径
       （c6 = c5<<1，c8 = (c6<<2)|(c6>>4)），等价于 `(c5<<3)|(c5>>3)`：
       白 31 → 0xFB（旧实现 `c5<<3` 得 0xF8，与参考核每通道差几个单位，
       实测把帧 100 的"逐像素相同率"从 0% 拉到 81.9%）。 */
    uint32_t r8 = (r << 3) | (r >> 3);
    uint32_t g8 = (g << 3) | (g >> 3);
    uint32_t b8 = (b << 3) | (b >> 3);
    return 0xFF000000u | (r8 << 16) | (g8 << 8) | b8;
}

/* 背景色：某引擎 BG 调色板第 0 项（tile 透明 / 无图层时露出） */
static uint32_t backdrop(const bus_t *bus, int is_sub)
{
    uint16_t c = bus_read16(bus, is_sub ? BG_PAL_SUB : BG_PAL_MAIN);
    return rgb555_to_888(c);
}

static void fill_fb(uint32_t *fb, uint32_t color)
{
    for (int i = 0; i < PX_COUNT; i++)
        fb[i] = color;
}

/* ---- 合成器（阶段 20.2/20.3）：每像素记录最上层/次上层颜色 + 混合目标标记 ---- */
typedef struct {
    uint32_t top;                    /* 最上层像素颜色 */
    uint32_t under;                  /* 紧邻其下的像素颜色 */
    uint8_t  top1st;                 /* 最上层是第一目标 */
    uint8_t  top2nd;                 /* 最上层是第二目标 */
    uint8_t  under2nd;               /* 次上层是第二目标 */
} px_t;

typedef struct {
    px_t *px;                        /* 指向 g_px 静态缓冲 */
    uint32_t blendcnt;
    uint32_t eva, evb, evy;          /* 0..16 */
    uint32_t bright;                 /* MASTER_BRIGHT */
    uint32_t win0h, win1h, win0v, win1v;
    uint32_t winin, winout;
    uint32_t dispcnt;                /* 窗口使能位 */
} comp_t;

static px_t g_px[PX_COUNT];

/* 像素 (x,y) 是否在矩形 (h,v) 内：h/v = X1|X2 / Y1|Y2，窗口 [X1,X2)×[Y1,Y2) */
static int in_win(int x, int y, uint32_t h, uint32_t v)
{
    int x1 = (h >> 8) & 0xFF;
    int x2 = h & 0xFF;
    int y1 = (v >> 8) & 0xFF;
    int y2 = v & 0xFF;
    if (x2 > RENDER_SCREEN_W) x2 = RENDER_SCREEN_W;
    if (x1 > x2) x2 = RENDER_SCREEN_W;      /* 非法（X1>X2）视为全宽 */
    if (y2 > RENDER_SCREEN_H) y2 = RENDER_SCREEN_H;
    if (y1 > y2) y2 = RENDER_SCREEN_H;
    return x >= x1 && x < x2 && y >= y1 && y < y2;
}

/* 取像素所在窗口区域的层使能掩码（bit0-4 层使能，bit5 特效使能）。无窗口 → 全部使能。 */
static uint32_t comp_winmask(const comp_t *c, int x, int y)
{
    uint32_t wen = (c->dispcnt >> 13) & 7u;   /* WIN0/WIN1/OBJ 窗口使能 */
    if (wen == 0)
        return 0x3Fu;
    uint32_t m = c->winout & 0x3Fu;           /* 窗口外 */
    if ((wen & 1u) && in_win(x, y, c->win0h, c->win0v))
        m = c->winin & 0x3Fu;                 /* 窗口 0 */
    else if ((wen & 2u) && in_win(x, y, c->win1h, c->win1v))
        m = (c->winin >> 8) & 0x3Fu;          /* 窗口 1 */
    return m;
}

static void comp_reset(comp_t *c, const bus_t *bus, int is_sub)
{
    uint32_t win_base = is_sub ? IO_WIN0H_SUB : IO_WIN0H;
    c->win0h = bus_read16(bus, win_base + 0);
    c->win1h = bus_read16(bus, win_base + 2);
    c->win0v = bus_read16(bus, win_base + 4);
    c->win1v = bus_read16(bus, win_base + 6);
    c->winin  = bus_read16(bus, win_base + 8);
    c->winout = bus_read16(bus, win_base + 10);
    c->blendcnt = bus_read16(bus, is_sub ? IO_BLENDCNT_SUB : IO_BLENDCNT);
    uint16_t alpha = bus_read16(bus, is_sub ? IO_BLENDCNT_SUB + 2 : IO_BLENDALPHA);
    uint16_t yreg  = bus_read16(bus, is_sub ? IO_BLENDCNT_SUB + 4 : IO_BLENDY);
    c->eva = alpha & 0x1Fu; if (c->eva > 16) c->eva = 16;
    c->evb = (alpha >> 8) & 0x1Fu; if (c->evb > 16) c->evb = 16;
    c->evy = yreg & 0x1Fu; if (c->evy > 16) c->evy = 16;
    c->bright = bus_read16(bus, is_sub ? IO_MASTER_BRIGHT_SUB : IO_MASTER_BRIGHT);
    c->dispcnt = bus_read32(bus, is_sub ? IO_DISPCNT_SUB : IO_DISPCNT);
    c->px = g_px;

    uint32_t bd = backdrop(bus, is_sub);
    uint8_t bd1st = (uint8_t)((c->blendcnt >> 5) & 1u);
    uint8_t bd2nd = (uint8_t)((c->blendcnt >> 13) & 1u);
    for (int i = 0; i < PX_COUNT; i++) {
        c->px[i].top = c->px[i].under = bd;
        c->px[i].top1st = bd1st;
        c->px[i].top2nd = bd2nd;
        c->px[i].under2nd = bd2nd;
    }
}

/* 写一个像素：窗口掩码禁止该层则不画；否则把当前最上层压成次上层，再写入新顶层。
   layer_bit：BG0-3=0-3、OBJ/3D=4；其混合目标位取自 BLENDCNT。 */
static void comp_put(comp_t *c, int i, int x, int y, uint32_t color, int layer_bit)
{
    if (!((comp_winmask(c, x, y) >> layer_bit) & 1u))
        return;
    px_t *p = &c->px[i];
    p->under = p->top;
    p->under2nd = p->top2nd;
    p->top = color;
    p->top1st = (uint8_t)((c->blendcnt >> layer_bit) & 1u);
    p->top2nd = (uint8_t)((c->blendcnt >> (layer_bit + 8)) & 1u);
}

/* 5 位通道（0-31）Alpha 混合：I = (I1st*EVA + I2nd*EVB)/16，截到 31 */
static uint32_t blend_alpha(uint32_t c1, uint32_t c2, uint32_t eva, uint32_t evb)
{
    int r1 = (c1 >> 19) & 0x1F, g1 = (c1 >> 11) & 0x1F, b1 = (c1 >> 3) & 0x1F;
    int r2 = (c2 >> 19) & 0x1F, g2 = (c2 >> 11) & 0x1F, b2 = (c2 >> 3) & 0x1F;
    int r = ((r1 * (int)eva + r2 * (int)evb) >> 4); if (r > 31) r = 31;
    int g = ((g1 * (int)eva + g2 * (int)evb) >> 4); if (g > 31) g = 31;
    int b = ((b1 * (int)eva + b2 * (int)evb) >> 4); if (b > 31) b = 31;
    return 0xFF000000u | ((uint32_t)r << 19) | ((uint32_t)g << 11) | ((uint32_t)b << 3);
}

/* 5 位通道亮度增减：up=1 增亮 I+(31-I)*EVY/16，up=0 减暗 I-I*EVY/16 */
static uint32_t blend_bright(uint32_t c, uint32_t evy, int up)
{
    int r = (c >> 19) & 0x1F, g = (c >> 11) & 0x1F, b = (c >> 3) & 0x1F;
    if (up) {
        r = r + (((31 - r) * (int)evy) >> 4);
        g = g + (((31 - g) * (int)evy) >> 4);
        b = b + (((31 - b) * (int)evy) >> 4);
    } else {
        r = r - ((r * (int)evy) >> 4);
        g = g - ((g * (int)evy) >> 4);
        b = b - ((b * (int)evy) >> 4);
    }
    return 0xFF000000u | ((uint32_t)r << 19) | ((uint32_t)g << 11) | ((uint32_t)b << 3);
}

/* 合成收尾：按 BLENDCNT 模式应用混合，再做整屏主亮度（MASTER_BRIGHT，6bit 通道） */
static void comp_finalize(const comp_t *c, uint32_t *fb)
{
    uint32_t mode = (c->blendcnt >> BLEND_MODE_SHIFT) & 3u;
    for (int i = 0; i < PX_COUNT; i++) {
        int x = i % RENDER_SCREEN_W, y = i / RENDER_SCREEN_W;
        const px_t *p = &c->px[i];
        uint32_t col = p->top;
        int effect = (comp_winmask(c, x, y) >> 5) & 1;
        if (effect && mode == 1 && p->top1st && p->under2nd)
            col = blend_alpha(p->top, p->under, c->eva, c->evb);
        else if (effect && mode == 2 && p->top1st)
            col = blend_bright(p->top, c->evy, 1);
        else if (effect && mode == 3 && p->top1st)
            col = blend_bright(p->top, c->evy, 0);
        fb[i] = col;
    }

    uint32_t bmode = (c->bright >> MB_MODE_SHIFT) & 3u;
    /* 21-B9y7：`NDS_NOMB=1` 时跳过 MASTER_BRIGHT（仅用于与参考核 dump 对齐：
       melonDS 的 `GPU.GetFramebuffers` 返回的是**加主亮度之前**的合成结果，
       而本地把主亮度做进了输出，导致"参考核有画面、本地全黑"的假分歧）。 */
    if (bmode == 0 || getenv("NDS_NOMB") != NULL)
        return;
    uint32_t f = c->bright & MB_FACTOR_MASK; if (f > 16) f = 16;
    for (int i = 0; i < PX_COUNT; i++) {
        uint32_t col = fb[i];
        int r6 = ((col >> 19) & 0x1F) << 1;
        int g6 = ((col >> 11) & 0x1F) << 1;
        int b6 = ((col >> 3)  & 0x1F) << 1;
        if (bmode == 1) { /* 增亮 */
            r6 += ((63 - r6) * (int)f) >> 4;
            g6 += ((63 - g6) * (int)f) >> 4;
            b6 += ((63 - b6) * (int)f) >> 4;
        } else if (bmode == 2) { /* 减暗 */
            r6 -= (r6 * (int)f) >> 4;
            g6 -= (g6 * (int)f) >> 4;
            b6 -= (b6 * (int)f) >> 4;
        }
        fb[i] = 0xFF000000u | ((uint32_t)r6 << 18) | ((uint32_t)g6 << 10) | ((uint32_t)b6 << 2);
    }
}

/* 直色位图：BG mode 3-5 下，BG2/BG3 若为「扩展位图 + 直色」（BGxCNT bit7=1 且
   bit2=1），则把 VRAM 里 256×256 的 16bpp 位图左上 256×192 画到屏幕。
   位图基址复用 BGxCNT 的「屏幕基址」字段 bit8-12，单位 16KB（真机 bitmap 语义，
   见 libnds BG_BMP_RAM / BG_BMP_BASE）。返回 1 表示命中位图路径，否则走 tile。 */
static int render_bitmap(const bus_t *bus, comp_t *c, int is_sub, uint32_t dispcnt)
{
    unsigned bmode = dispcnt & DISPCNT_MODE_MASK;

    if (bmode < 3 || bmode > 5)
        return 0;
    uint32_t gfx_base = is_sub ? BG_GFX_SUB : BG_GFX_MAIN;
    uint32_t bgcnt_base = is_sub ? IO_BGCNT_SUB_BASE : IO_BGCNT_BASE;

    for (int bg = 2; bg <= 3; bg++) {
        if (!(dispcnt & (DISPCNT_BG0 << bg)))
            continue; /* 该 BG 未启用 */
        uint16_t bgcnt = bus_read16(bus, bgcnt_base + 2 * bg);
        if (!(bgcnt & BGCNT_COLORS_256) || !(bgcnt & BGCNT_DIRECT_COLOR))
            continue; /* 不是「扩展位图 + 直色」 */
        uint32_t bmp_block = (bgcnt >> BGCNT_SCREEN_BASE_SHIFT) & 0x1Fu;
        uint32_t bmp = gfx_base + bmp_block * 0x4000u;
        for (int y = 0; y < RENDER_SCREEN_H; y++) {
            for (int x = 0; x < RENDER_SCREEN_W; x++) {
                uint32_t paddr = bmp + (uint32_t)(y * 256 + x) * 2u;
                comp_put(c, y * RENDER_SCREEN_W + x, x, y,
                         rgb555_to_888(bus_read16(bus, paddr)), bg);
            }
        }
        return 1;
    }
    return 0;
}

/* 取 tile 里某像素的调色板索引。tile_addr 是该 tile 在字符数据里的字节基址。
   4bpp（16 色）：32 字节/tile，8 行×4 字节，4 个位面，bit 位序 0=最左像素；
   8bpp（256 色）：64 字节/tile，直接 1 字节 1 索引。 */
static uint8_t tile_pixel(const bus_t *bus, uint32_t tile_addr, int px, int py, int is_256)
{
    if (is_256)
        return bus_read8(bus, tile_addr + (uint32_t)(py * 8 + px));
    uint32_t row = tile_addr + (uint32_t)py * 4u;
    int bit = 7 - px;
    uint8_t idx = 0;
    for (int b = 0; b < 4; b++)
        idx |= (uint8_t)(((bus_read8(bus, row + b) >> bit) & 1u) << b);
    return idx;
}

/* 绘制一个 tile 图层：对每个屏幕像素，经 tilemap 找 tile → 取索引 → 查调色板。
   索引 0 = 透明（跳过）。支持 hflip/vflip。
   最小实现：screen size=0（32×32 tile），暂不支持滚动（map 坐标 = 屏幕坐标）。 */
static void draw_bg(const bus_t *bus, comp_t *c, uint32_t char_base,
                    uint32_t map_base, uint32_t pal_base, int is_256,
                    int ext, int ext_slot, int bg, int is_sub)
{
    for (int y = 0; y < RENDER_SCREEN_H; y++) {
        for (int x = 0; x < RENDER_SCREEN_W; x++) {
            int tile_x = x >> 3;
            int tile_y = y >> 3;
            int px = x & 7;
            int py = y & 7;
            uint16_t entry = bus_read16(bus, map_base + (uint32_t)(tile_y * 32 + tile_x) * 2u);
            uint16_t tile_idx = entry & 0x3FFu;
            if ((entry >> 10) & 1u) px = 7 - px; /* 水平翻转 */
            if ((entry >> 11) & 1u) py = 7 - py; /* 垂直翻转 */
            uint32_t tile_addr = char_base + (uint32_t)tile_idx * (is_256 ? 64u : 32u);
            uint8_t idx = tile_pixel(bus, tile_addr, px, py, is_256);
            if (idx == 0)
                continue; /* 透明像素 */
            uint16_t color;
            if (is_256 && ext)
                /* 21-B9yi(续37)：扩展调色板槽号按 melonDS 口径——
                   `extpalslot = ((bgnum<2) && (bgcnt & 0x2000)) ? 2+bgnum : bgnum`
                   （BG0/BG1 可用 bit13 切到 2/3 号槽；本地此前一律用 bgnum）。 */
                color = bus_vram_extpal16(bus, is_sub, ext_slot,
                                          (entry >> 12) & 0xFu, idx);
            else if (is_256)
                color = bus_read16(bus, pal_base + (uint32_t)idx * 2u);
            else
                color = bus_read16(bus, pal_base + (uint32_t)(((entry >> 12) & 0xFu) * 16 + idx) * 2u);
            comp_put(c, y * RENDER_SCREEN_W + x, x, y, rgb555_to_888(color), bg);
        }
    }
}

/* 1.19.8 参考点：取 28 位（bit0-27）并从 bit27 符号扩展。 */
static int32_t affine_ref(int32_t v)
{
    return (int32_t)((uint32_t)v << 4) >> 4;
}

/* 仿射背景（MODE1 的 BG2、MODE2 的 BG2/3，阶段 20.1）：屏幕坐标经逆仿射变换映射回纹理。
   纹理恒为 8bpp、1 字节/项（无 flip/调色板号）；PA-PD 为 1.7.8，X/Y 参考点为 1.19.8。
   溢出位(BGxCNT bit13)=0 时越界透明，=1 时环绕。 */
static void draw_affine_bg(const bus_t *bus, comp_t *c, uint16_t bgcnt,
                           uint32_t char_base, uint32_t map_base, uint32_t pal_base,
                           int bg, int is_sub)
{
    static const int aff_size[4] = { 128, 256, 512, 1024 };
    uint32_t aff_base = (is_sub ? IO_BG_AFFINE_SUB_BASE : IO_BG_AFFINE_BASE)
                      + (uint32_t)(bg == 3 ? 16 : 0);
    int16_t pa = (int16_t)bus_read16(bus, aff_base + 0);
    int16_t pb = (int16_t)bus_read16(bus, aff_base + 2);
    int16_t pc = (int16_t)bus_read16(bus, aff_base + 4);
    int16_t pd = (int16_t)bus_read16(bus, aff_base + 6);
    int32_t refx = affine_ref((int32_t)bus_read32(bus, aff_base + 8));
    int32_t refy = affine_ref((int32_t)bus_read32(bus, aff_base + 12));

    int size = aff_size[(bgcnt >> BGCNT_SCREEN_SIZE_SHIFT) & 3u];
    int wrap = (bgcnt & BGCNT_OVERFLOW) != 0;
    int tiles = size >> 3;

    for (int y = 0; y < RENDER_SCREEN_H; y++) {
        for (int x = 0; x < RENDER_SCREEN_W; x++) {
            int64_t tx = (int64_t)refx + (int64_t)pa * x + (int64_t)pb * y;
            int64_t ty = (int64_t)refy + (int64_t)pc * x + (int64_t)pd * y;
            int tex_x = (int)(tx >> 8);
            int tex_y = (int)(ty >> 8);
            if (!wrap) {
                if (tex_x < 0 || tex_x >= size || tex_y < 0 || tex_y >= size)
                    continue; /* 越界透明 */
            } else {
                tex_x &= size - 1;
                tex_y &= size - 1;
            }
            int tile_x = tex_x >> 3;
            int tile_y = tex_y >> 3;
            int px = tex_x & 7;
            int py = tex_y & 7;
            uint8_t tile_idx = bus_read8(bus, map_base + (uint32_t)(tile_y * tiles + tile_x));
            uint32_t tile_addr = char_base + (uint32_t)tile_idx * 64u;
            uint8_t idx = bus_read8(bus, tile_addr + (uint32_t)(py * 8 + px));
            if (idx == 0)
                continue; /* 透明像素 */
            uint16_t color = bus_read16(bus, pal_base + (uint32_t)idx * 2u);
            comp_put(c, y * RENDER_SCREEN_W + x, x, y, rgb555_to_888(color), bg);
        }
    }
}

/* 某 BG 在当前模式下是否为仿射背景（MODE1：BG2；MODE2：BG2/BG3） */
static int bg_is_affine(unsigned bmode, int bg)
{
    if (bmode == 1)
        return bg == 2;
    if (bmode == 2)
        return bg == 2 || bg == 3;
    return 0;
}

/* tile/仿射 图层渲染：Mode 0/1/2 的文本 BG + 仿射 BG。按优先级合成（数字小的在最上层，
   同优先级 BG0 最高），透明像素露出已画的更低层/背景色。 */
static void render_tiled(const bus_t *bus, comp_t *c, int is_sub, uint32_t dispcnt)
{
    uint32_t gfx_base = is_sub ? BG_GFX_SUB : BG_GFX_MAIN;
    uint32_t pal_base = is_sub ? BG_PAL_SUB : BG_PAL_MAIN;
    uint32_t bgcnt_base = is_sub ? IO_BGCNT_SUB_BASE : IO_BGCNT_BASE;
    /* DISPCNT 字符/屏幕基址（64K 步进）仅主引擎有效，副引擎为 0 */
    uint32_t disp_char = is_sub ? 0 : ((dispcnt >> DISPCNT_CHAR_BASE_SHIFT) & 7u);
    uint32_t disp_screen = is_sub ? 0 : ((dispcnt >> DISPCNT_SCREEN_BASE_SHIFT) & 7u);
    unsigned bmode = dispcnt & DISPCNT_MODE_MASK;

    /* 21-B9y3 诊断：无条件打印本引擎的 DISPCNT 与 4 个 BGxCNT（不受使能/优先级过滤） */
    {
        static int dbg_eng[2] = {0, 0};
        int eng = is_sub ? 1 : 0;
        if (!dbg_eng[eng] && getenv("NDS_BGDBG") != NULL) {
            dbg_eng[eng] = 1;
            printf("bgdbg: engine=%d dispcnt=%08X mode=%u bgcnt=%04X/%04X/%04X/%04X\n",
                   is_sub, dispcnt, bmode,
                   bus_read16(bus, bgcnt_base + 0), bus_read16(bus, bgcnt_base + 2),
                   bus_read16(bus, bgcnt_base + 4), bus_read16(bus, bgcnt_base + 6));
        }
    }

    /* 先画低优先级（数字大），再画高优先级（数字小）以正确覆盖 */
    for (int prio = 3; prio >= 0; prio--) {
        for (int bg = 3; bg >= 0; bg--) {
            if (!(dispcnt & (DISPCNT_BG0 << bg)))
                continue;
            uint16_t bgcnt = bus_read16(bus, bgcnt_base + 2 * bg);
            if (((bgcnt >> BGCNT_PRIORITY_SHIFT) & 3u) != prio)
                continue;
            uint32_t char_base = gfx_base
                + ((bgcnt >> BGCNT_CHAR_BASE_SHIFT) & 0xFu) * 0x4000u
                + disp_char * 0x10000u;
            uint32_t map_base = gfx_base
                + ((bgcnt >> BGCNT_SCREEN_BASE_SHIFT) & 0x1Fu) * 0x800u
                + disp_screen * 0x10000u;
            /* 21-B9y3 诊断（NDS_BGDBG=1 时生效）：打印本层取址与首个地图项，
               用于对照 melonDS 的同口径公式、定位"全黑"是取址还是取色问题。 */
            {
                static int bgdbg_done = 0;
                if (!bgdbg_done && getenv("NDS_BGDBG") != NULL) {
                    uint16_t e0 = bus_read16(bus, map_base);
                    uint16_t e1 = bus_read16(bus, map_base + 2);
                    printf("bgdbg: bg=%d prio=%d bgcnt=%04X char=%08X map=%08X"
                           " e0=%04X e1=%04X 256c=%d ext=%d\n",
                           bg, prio, bgcnt, char_base, map_base, e0, e1,
                           (bgcnt & BGCNT_COLORS_256) != 0,
                           (dispcnt & (1u << 30)) != 0);
                    if (bg == 3 && prio == 3)
                        bgdbg_done = 1;
                    /* 21-B9y5：顺手打印 tile0 的前 8 字节与调色板前 4 项，
                       用于判断"全黑"是取址错（读到空区）还是取色错（索引 0 透明）。 */
                    {
                        uint16_t t0 = (uint16_t)(e0 & 0x3FFu);
                        uint32_t ta = char_base + (uint32_t)t0 * 64u;
                        printf("bgdbg:   tile0_addr=%08X bytes=", ta);
                        for (int k = 0; k < 8; k++)
                            printf("%02X", bus_read8(bus, ta + (uint32_t)k));
                        printf(" pal0..3=");
                        for (int k = 0; k < 4; k++)
                            printf("%04X ", bus_read16(bus, pal_base + (uint32_t)k * 2));
                        printf("\n");
                    }
                }
            }
            int ext = (dispcnt & (1u << 30)) != 0;
            /* 21-B9yi(续37)：扩展调色板槽号（melonDS：BG0/BG1 的 bit13 会把槽切到 2/3） */
            int ext_slot = (bg < 2 && (bgcnt & 0x2000u)) ? (2 + bg) : bg;
            if (bg_is_affine(bmode, bg))
                draw_affine_bg(bus, c, bgcnt, char_base, map_base, pal_base, bg, is_sub);
            else
                draw_bg(bus, c, char_base, map_base, pal_base,
                        (bgcnt & BGCNT_COLORS_256) != 0, ext, ext_slot, bg, is_sub);
        }
    }
}

/* OBJ（sprite）渲染：读 OAM 每个条目（6 字节 = 3 个半字，步进 8 字节），
   按 1D tile 映射取字符数据 → 查 OBJ 调色板 → 覆盖到 framebuffer。
   最小实现：无旋转/缩放（bit8 跳过）、无 flip、无半透明，透明索引 0 不画；
   OBJ 一律画在 BG 之上（相当于最高优先级，验收仅需「屏上有活动块」）。 */
static void render_obj(const bus_t *bus, comp_t *c, int is_sub)
{
    uint32_t oam_base = is_sub ? OAM_SUB : OAM_MAIN;
    uint32_t gfx_base = is_sub ? OBJ_GFX_SUB : OBJ_GFX_MAIN;
    uint32_t pal_base = is_sub ? OBJ_PAL_SUB : OBJ_PAL_MAIN;

    /* OBJ 尺寸表：size × shape（0=方,1=横,2=竖）→ 宽/高。shape=3 非法。 */
    static const int w_tbl[4][3] = { {8,16,8}, {16,32,8}, {32,32,16}, {64,64,32} };
    static const int h_tbl[4][3] = { {8,8,16}, {16,8,32}, {32,16,32}, {64,32,64} };

    for (int n = 0; n < 128; n++) {
        uint32_t oam = oam_base + (uint32_t)n * 8u;
        uint16_t a0 = bus_read16(bus, oam);       /* 属性0：Y/模式/色深/形状 */
        uint16_t a1 = bus_read16(bus, oam + 2u);  /* 属性1：X/flip/尺寸 */
        uint16_t a2 = bus_read16(bus, oam + 4u);  /* 属性2：tile 号/优先级/调色板 */
        if (a0 & 0x0200u) continue;               /* bit9 = 禁用（非旋转模式） */
        if (a0 & 0x0100u) continue;               /* bit8 = 旋转/缩放：不支持 */
        int is_256 = (a0 >> 13) & 1;
        int shape = (a0 >> 14) & 3;
        int size = (a1 >> 14) & 3;
        if (shape == 3) continue;
        int w = w_tbl[size][shape];
        int h = h_tbl[size][shape];
        int x = a1 & 0x1FF;                       /* 9 位 X（>=256 在屏幕右侧外） */
        int y = a0 & 0xFF;
        uint16_t base_tile = a2 & 0x3FF;
        int pal_slot = (a2 >> 12) & 0xF;
        int tiles_x = w / 8;

        for (int ty = 0; ty < h; ty++) {
            int sy = y + ty;
            if (sy < 0 || sy >= RENDER_SCREEN_H) continue;
            for (int tx = 0; tx < w; tx++) {
                int sx = x + tx;
                if (sx < 0 || sx >= RENDER_SCREEN_W) continue;
                /* 1D 映射：tile 号从左到右、从上到下线性排布 */
                uint16_t tile = base_tile + (uint16_t)((ty / 8) * tiles_x + (tx / 8));
                uint32_t tile_addr = gfx_base + (uint32_t)tile * (is_256 ? 64u : 32u);
                uint8_t idx = tile_pixel(bus, tile_addr, tx & 7, ty & 7, is_256);
                if (idx == 0) continue;            /* 透明 */
                uint16_t color = is_256
                    ? bus_read16(bus, pal_base + (uint32_t)idx * 2u)
                    : bus_read16(bus, pal_base + (uint32_t)(pal_slot * 16 + idx) * 2u);
                comp_put(c, sy * RENDER_SCREEN_W + sx, sx, sy, rgb555_to_888(color), 4);
            }
        }
    }
}

/* 3D 图层（阶段 19.4 / 21-B9xw 更正）：3D 输出**不**由 DISP3DCNT 使能位控制
    （melonDS 里 DISP3DCNT bit12/13 是写 1 清零位，帧 1900 参考核 bit13=0 时
    3D 内容照样出现在顶屏）。因此这里直接合成 3D 帧缓冲。

    21-B9yi(续34)：透明度按 melonDS `SoftRenderer2D::DrawBG_3D()` 的口径——
    **看 alpha（本地新增的 alpha 平面），alpha==0 才跳过**；此前按「颜色==0 → 无
    几何」判，所有涂成黑色（0x0000）的不透明多边形都会被误当成没有几何。
    混合目标位复用 OBJ（bit4/12）。 */
static void render_3d(const bus_t *bus, comp_t *c)
{
    if (bus->io == NULL)
        return;
    const gx_t *g = &bus->io->gx;
    const uint16_t *src = gx_framebuffer(g);
    const uint8_t *asrc = gx_framebuffer_alpha(g);
    for (int i = 0; i < PX_COUNT; i++) {
        if (asrc[i] == 0)
            continue;
        comp_put(c, i, i % RENDER_SCREEN_W, i / RENDER_SCREEN_W, rgb555_to_888(src[i]), 4);
    }
}

/* 渲染单个引擎（主/副）到一块 framebuffer */
static void render_engine(const bus_t *bus, uint32_t *fb, int is_sub)
{
    uint32_t dispcnt = bus_read32(bus, is_sub ? IO_DISPCNT_SUB : IO_DISPCNT);

    /* 强制消隐 → 白屏 */
    if (dispcnt & DISPCNT_FORCED_BLANK) {
        fill_fb(fb, 0xFFFFFFFFu);
        return;
    }
    /* 显示模式 0 = 关闭（白屏） */
    unsigned dmode = (dispcnt & DISPCNT_DISPLAY_MODE_MASK) >> DISPCNT_DISPLAY_MODE_SHIFT;
    if (dmode == 0) {
        fill_fb(fb, 0xFFFFFFFFu);
        return;
    }
    /* DISPCNT bit16-17 = 2：VRAM 显示模式（仅主引擎）。帧直接来自所选
       VRAM bank（bit18-19），不做 2D 图层合成；FFXII 标题顶屏用此模式。

       21-B9xx 实验记录：曾按"melonDS 只用 mode3 特例"把这条分支去掉，
       结果**帧 1900 输出没有任何变化**（本地 bank A 空 → 普通渲染也是黑），
       但 `vramdisp bank*` 三项单测会挂 → 说明这条分支对应的是项目里
       另一处已验收行为，暂时保留；条纹差异另寻来源。 */
    if (!is_sub && dmode == 2) {
        unsigned vbank = (dispcnt >> 18) & 3u;
        for (int py = 0; py < RENDER_SCREEN_H; py++) {
            for (int px = 0; px < RENDER_SCREEN_W; px++) {
                uint16_t c = bus_vram_phys16(bus, (int)vbank,
                                             (uint32_t)(py * RENDER_SCREEN_W
                                                        + px) * 2u);
                fb[py * RENDER_SCREEN_W + px] = rgb555_to_888(c);
            }
        }
        return;
    }

    comp_t c;
    comp_reset(&c, bus, is_sub);

    if (!render_bitmap(bus, &c, is_sub, dispcnt))
        render_tiled(bus, &c, is_sub, dispcnt);
    if (getenv("NDS_BGDBG"))
        printf("bgdbg: stage=tiled eng=%d px=%08X\n", is_sub, c.px[100 * 256 + 50]);
    if (dispcnt & DISPCNT_OBJ)
        render_obj(bus, &c, is_sub);
    if (getenv("NDS_BGDBG"))
        printf("bgdbg: stage=obj eng=%d px=%08X\n", is_sub, c.px[100 * 256 + 50]);
    /* 21-B9yi(续34) 诊断：`NDS_NO3D=1` 时不合成 3D 图层（判断某段画面是不是 3D 撑的） */
    if (!is_sub && getenv("NDS_NO3D") == NULL)
        render_3d(bus, &c);
    if (getenv("NDS_BGDBG"))
        printf("bgdbg: stage=3d eng=%d px=%08X\n", is_sub, c.px[100 * 256 + 50]);

    comp_finalize(&c, fb);
    if (getenv("NDS_BGDBG"))
        printf("bgdbg: stage=final eng=%d fb=%08X\n", is_sub, fb[100 * 256 + 50]);
}

/* 显示捕获（阶段 20.2 最小实现）：DISPCAPCNT bit31 置位时，把主引擎（顶屏）出图
   以 RGB555 写进 LCDC VRAM（0x06800000），随后清使能位。仅源 A = 图形屏。 */
static void render_capture(bus_t *bus, const uint32_t *fb_top)
{
    if (bus->io == NULL)
        return;
    uint32_t cap = bus->io->disp.dispcapcnt;
    if (!(cap & DISPCAP_ENABLE))
        return;
    uint32_t block = (cap >> 16) & 3u;   /* VRAM 写块 A-D */
    uint32_t off   = (cap >> 18) & 3u;   /* 写偏移 */
    uint32_t dst = LCDC_VRAM + block * 0x20000u + off * 0x8000u;
    for (int i = 0; i < PX_COUNT; i++) {
        uint32_t c = fb_top[i];
        uint16_t rgb = (uint16_t)(((c >> 19) & 0x1Fu) << 10)
                     | (uint16_t)(((c >> 11) & 0x1Fu) << 5)
                     | (uint16_t)((c >> 3) & 0x1Fu);
        bus_write16(bus, dst + (uint32_t)i * 2u, rgb);
    }
    bus->io->disp.dispcapcnt &= ~DISPCAP_ENABLE; /* 捕获完成，清使能 */
}

void render_frame(bus_t *bus, uint32_t *fb_top, uint32_t *fb_bot)
{
    render_engine(bus, fb_top, 0);
    render_engine(bus, fb_bot, 1);
    render_capture(bus, fb_top);
}
