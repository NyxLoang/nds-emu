#include "render.h"
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

/* RGB555 → RGB888：5 位通道左移 3 位扩成 8 位（0x1F→0xF8），A 通道固定不透明 */
static uint32_t rgb555_to_888(uint16_t c)
{
    uint32_t r = (c >> 10) & 0x1Fu;
    uint32_t g = (c >> 5)  & 0x1Fu;
    uint32_t b = c & 0x1Fu;
    return 0xFF000000u | (r << 19) | (g << 11) | (b << 3);
}

/* 背景色：某引擎 BG 调色板第 0 项（tile 透明 / 无图层时露出） */
static uint32_t backdrop(const bus_t *bus, int is_sub)
{
    uint16_t c = bus_read16(bus, is_sub ? BG_PAL_SUB : BG_PAL_MAIN);
    return rgb555_to_888(c);
}

static void fill_fb(uint32_t *fb, uint32_t color)
{
    for (int i = 0; i < RENDER_SCREEN_W * RENDER_SCREEN_H; i++)
        fb[i] = color;
}

/* 直色位图：BG mode 3-5 下，BG2/BG3 若为「扩展位图 + 直色」（BGxCNT bit7=1 且
   bit2=1），则把 VRAM 里 256×256 的 16bpp 位图左上 256×192 画到屏幕。
   位图基址复用 BGxCNT 的「屏幕基址」字段 bit8-12，单位 16KB（真机 bitmap 语义，
   见 libnds BG_BMP_RAM / BG_BMP_BASE）。返回 1 表示命中位图路径，否则走 tile。 */
static int render_bitmap(const bus_t *bus, uint32_t *fb, int is_sub, uint32_t dispcnt)
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
                fb[y * RENDER_SCREEN_W + x] = rgb555_to_888(bus_read16(bus, paddr));
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
   索引 0 = 透明（跳过，露出已画的更低层/背景色）。支持 hflip/vflip。
   最小实现：screen size=0（32×32 tile），暂不支持滚动（map 坐标 = 屏幕坐标）。 */
static void draw_bg(const bus_t *bus, uint32_t *fb, uint32_t char_base,
                    uint32_t map_base, uint32_t pal_base, int is_256)
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
            if (is_256)
                color = bus_read16(bus, pal_base + (uint32_t)idx * 2u);
            else
                color = bus_read16(bus, pal_base + (uint32_t)(((entry >> 12) & 0xFu) * 16 + idx) * 2u);
            fb[y * RENDER_SCREEN_W + x] = rgb555_to_888(color);
        }
    }
}

/* tile 图层渲染：Mode 0/1/2 的文本 BG。按优先级合成（数字小的在最上层，
   同优先级 BG0 最高），透明像素露出已画的更低层/背景色。 */
static void render_tiled(const bus_t *bus, uint32_t *fb, int is_sub, uint32_t dispcnt)
{
    uint32_t gfx_base = is_sub ? BG_GFX_SUB : BG_GFX_MAIN;
    uint32_t pal_base = is_sub ? BG_PAL_SUB : BG_PAL_MAIN;
    uint32_t bgcnt_base = is_sub ? IO_BGCNT_SUB_BASE : IO_BGCNT_BASE;
    /* DISPCNT 字符/屏幕基址（64K 步进）仅主引擎有效，副引擎为 0 */
    uint32_t disp_char = is_sub ? 0 : ((dispcnt >> DISPCNT_CHAR_BASE_SHIFT) & 7u);
    uint32_t disp_screen = is_sub ? 0 : ((dispcnt >> DISPCNT_SCREEN_BASE_SHIFT) & 7u);

    fill_fb(fb, backdrop(bus, is_sub));

    /* 先画低优先级（数字大），再画高优先级（数字小）以正确覆盖 */
    for (int prio = 3; prio >= 0; prio--) {
        for (int bg = 3; bg >= 0; bg--) {
            if (!(dispcnt & (DISPCNT_BG0 << bg)))
                continue;
            uint16_t bgcnt = bus_read16(bus, bgcnt_base + 2 * bg);
            if (((bgcnt >> BGCNT_PRIORITY_SHIFT) & 3u) != prio)
                continue;
            int is_256 = (bgcnt & BGCNT_COLORS_256) != 0;
            uint32_t char_base = gfx_base
                + ((bgcnt >> BGCNT_CHAR_BASE_SHIFT) & 0xFu) * 0x4000u
                + disp_char * 0x10000u;
            uint32_t map_base = gfx_base
                + ((bgcnt >> BGCNT_SCREEN_BASE_SHIFT) & 0x1Fu) * 0x800u
                + disp_screen * 0x10000u;
            draw_bg(bus, fb, char_base, map_base, pal_base, is_256);
        }
    }
}

/* OBJ（sprite）渲染：读 OAM 每个条目（6 字节 = 3 个半字，步进 8 字节），
   按 1D tile 映射取字符数据 → 查 OBJ 调色板 → 覆盖到 framebuffer。
   最小实现：无旋转/缩放（bit8 跳过）、无 flip、无半透明，透明索引 0 不画；
   OBJ 一律画在 BG 之上（相当于最高优先级，验收仅需「屏上有活动块」）。 */
static void render_obj(const bus_t *bus, uint32_t *fb, int is_sub)
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
                fb[sy * RENDER_SCREEN_W + sx] = rgb555_to_888(color);
            }
        }
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

    if (!render_bitmap(bus, fb, is_sub, dispcnt))
        render_tiled(bus, fb, is_sub, dispcnt);
    if (dispcnt & DISPCNT_OBJ)
        render_obj(bus, fb, is_sub);
}

/* 3D 图层合成（阶段 19.4）：DISP3DCNT 使能位(bit13)置位时，把 3D 帧缓冲
   覆盖到主引擎（顶屏）。3D 帧缓冲的 0 像素视为「无几何」背景，保留 2D 输出；
   非 0 像素用其 RGB555 颜色覆盖。最小实现：不分优先级、不做 alpha 混合。 */
static void render_3d(const bus_t *bus, uint32_t *fb)
{
    if (bus->io == NULL)
        return;
    const gx_t *g = &bus->io->gx;
    if (!(g->disp3dcnt & DISP3D_ENABLE))
        return;
    const uint16_t *src = gx_framebuffer(g);
    for (int i = 0; i < RENDER_SCREEN_W * RENDER_SCREEN_H; i++) {
        if (src[i] != 0)
            fb[i] = rgb555_to_888(src[i]);
    }
}

void render_frame(const bus_t *bus, uint32_t *fb_top, uint32_t *fb_bot)
{
    render_engine(bus, fb_top, 0);
    render_engine(bus, fb_bot, 1);
    render_3d(bus, fb_top);
}
