#include <stdio.h>
#include <SDL_ttf.h>
#include "menu.h"
#include "window/window.h"

enum { LANG_EN = 0, LANG_ZH };
enum { MENU_NONE = -1, MENU_SCALE = 0, MENU_LANG = 1 };

static const int SCALE_VALUES[] = { 1, 2, 3, 4 };
#define NUM_SCALES 4

static const char *const FONT_CANDIDATES[] = {
    "C:/Windows/Fonts/msyh.ttc",
    "C:/Windows/Fonts/msyh.ttf",
    "C:/Windows/Fonts/simhei.ttf",
    "C:/Windows/Fonts/simsun.ttc",
    "C:/Windows/Fonts/arial.ttf",
};
#define NUM_FONT_CANDIDATES (sizeof(FONT_CANDIDATES) / sizeof(FONT_CANDIDATES[0]))

/* 布局矩形均以逻辑坐标（1x）定义，绘制时乘以 scale */
#define MENU_ITEM_H MENU_H
#define MENU_ITEM_Y 0
#define MENU_ITEM_GAP 4
#define MENU_ITEM_W 72
#define MENU_ITEM_W_LANG 88

#define DD_ITEM_H 24
#define DD_ITEM_GAP 2
#define DD_X 4

#define FONT_PT 16 /* 1x 时的字号；N 倍放大时字号 = FONT_PT*scale */

static TTF_Font *font = NULL;
static int zh_ok = 0;
static int lang = LANG_EN;     /* 初始英文；无中文字形时永远英文 */
static int open_menu = MENU_NONE;

/* 菜单栏两个菜单项的矩形（逻辑坐标） */
static SDL_Rect menu_item_rect(int idx)
{
    SDL_Rect r = {
        MENU_ITEM_GAP + idx * (MENU_ITEM_W + MENU_ITEM_GAP),
        MENU_ITEM_Y,
        MENU_ITEM_W,
        MENU_ITEM_H,
    };
    if (idx == MENU_LANG)
        r.w = MENU_ITEM_W_LANG;
    return r;
}

/* 下拉菜单里某一项的矩形（逻辑坐标，menu: MENU_SCALE / MENU_LANG） */
static SDL_Rect dropdown_item_rect(int menu, int idx)
{
    SDL_Rect r = {
        DD_X,
        MENU_H + idx * (DD_ITEM_H + DD_ITEM_GAP),
        (menu == MENU_SCALE) ? MENU_ITEM_W : MENU_ITEM_W_LANG,
        DD_ITEM_H,
    };
    return r;
}

/* 在物理矩形 rect 内居中文本。
   1x 时以 2 倍字号渲染再线性缩小（超采样），小字号更清晰；
   2x+ 直接 1:1 渲染（字号够大，像素本就充足）。 */
static void draw_text_centered(SDL_Renderer *r, TTF_Font *font, const char *text,
                               SDL_Rect rect, SDL_Color color, int scale)
{
    int target_pt = FONT_PT * scale;
    int ss = (scale == 1) ? 2 : 1; /* 超采样系数 */

    TTF_SetFontSize(font, target_pt * ss);
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, color);
    if (surf == NULL)
        return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surf);
    SDL_FreeSurface(surf);
    if (tex == NULL)
        return;

    /* 缩小（1x 超采样）用线性平滑；只影响本文字纹理，不改全局 nearest hint */
    if (ss > 1)
        SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);

    /* 目标尺寸按 target_pt 测，用于居中与缩放绘制 */
    int tw, th;
    TTF_SetFontSize(font, target_pt);
    if (TTF_SizeUTF8(font, text, &tw, &th) != 0) {
        SDL_DestroyTexture(tex);
        return;
    }
    SDL_Rect dst = {
        rect.x + (rect.w - tw) / 2,
        rect.y + (rect.h - th) / 2,
        tw,
        th,
    };
    SDL_RenderCopy(r, tex, NULL, &dst);
    SDL_DestroyTexture(tex);
}

static TTF_Font *open_font(void)
{
    TTF_Font *f = NULL;
    for (size_t i = 0; i < NUM_FONT_CANDIDATES; ++i) {
        f = TTF_OpenFontIndex(FONT_CANDIDATES[i], FONT_PT, 0);
        if (f != NULL)
            break;
    }
    return f;
}

int menu_init(SDL_Renderer *r)
{
    (void)r;

    font = open_font();
    if (font == NULL)
        return -1;

    /* 检查是否支持中文字形（“缩放”两字），不支持则强制英文 */
    zh_ok = TTF_GlyphIsProvided(font, 0x7F29) &&   /* 缩 */
            TTF_GlyphIsProvided(font, 0x8BED);     /* 语 */
    return 0;
}

void menu_shutdown(void)
{
    if (font != NULL)
        TTF_CloseFont(font);
}

int menu_handle_click(int lx, int ly)
{
    int handled = 0;
    int new_scale = 0;

    if (open_menu == MENU_SCALE) {
        for (int i = 0; i < NUM_SCALES; ++i) {
            SDL_Rect r = dropdown_item_rect(MENU_SCALE, i);
            if (lx >= r.x && lx < r.x + r.w &&
                ly >= r.y && ly < r.y + r.h) {
                new_scale = SCALE_VALUES[i];
                open_menu = MENU_NONE;
                handled = 1;
                break;
            }
        }
    } else if (open_menu == MENU_LANG) {
        for (int i = LANG_EN; i <= LANG_ZH; ++i) {
            SDL_Rect r = dropdown_item_rect(MENU_LANG, i);
            if (lx >= r.x && lx < r.x + r.w &&
                ly >= r.y && ly < r.y + r.h) {
                if (zh_ok || i == LANG_EN)
                    lang = i;
                open_menu = MENU_NONE;
                handled = 1;
                break;
            }
        }
    }

    if (!handled) {
        SDL_Rect r = menu_item_rect(MENU_SCALE);
        if (lx >= r.x && lx < r.x + r.w &&
            ly >= r.y && ly < r.y + r.h) {
            open_menu = (open_menu == MENU_SCALE) ? MENU_NONE : MENU_SCALE;
            handled = 1;
        }
    }
    if (!handled) {
        SDL_Rect r = menu_item_rect(MENU_LANG);
        if (lx >= r.x && lx < r.x + r.w &&
            ly >= r.y && ly < r.y + r.h) {
            open_menu = (open_menu == MENU_LANG) ? MENU_NONE : MENU_LANG;
            handled = 1;
        }
    }
    if (!handled) {
        open_menu = MENU_NONE; /* 点其它区域收起下拉 */
    }

    return new_scale;
}

void menu_render_bar(SDL_Renderer *r, int scale)
{
    SDL_Color c_text = { 235, 235, 235, 255 };
    SDL_Color c_menu = { 45, 45, 45, 255 };
    SDL_Color c_menu_hl = { 70, 120, 200, 255 };

    for (int i = MENU_SCALE; i <= MENU_LANG; ++i) {
        SDL_Rect rct = menu_item_rect(i);
        rct.x *= scale;
        rct.y *= scale;
        rct.w *= scale;
        rct.h *= scale;
        if (open_menu == i)
            SDL_SetRenderDrawColor(r, c_menu_hl.r, c_menu_hl.g, c_menu_hl.b, 255);
        else
            SDL_SetRenderDrawColor(r, c_menu.r, c_menu.g, c_menu.b, 255);
        SDL_RenderFillRect(r, &rct);

        const char *label;
        if (i == MENU_SCALE)
            label = (lang == LANG_ZH) ? "\xe7\xbc\xa9\xe6\x94\xbe" : "Scale";      /* 缩放 */
        else
            label = (lang == LANG_ZH) ? "\xe8\xaf\xad\xe8\xa8\x80" : "Language";    /* 语言 */
        draw_text_centered(r, font, label, rct, c_text, scale);
    }
}

void menu_render_dropdown(SDL_Renderer *r, int scale)
{
    SDL_Color c_text = { 235, 235, 235, 255 };
    SDL_Color c_dd = { 60, 60, 60, 255 };
    SDL_Color c_dd_hl = { 70, 120, 200, 255 };

    if (open_menu == MENU_SCALE) {
        for (int i = 0; i < NUM_SCALES; ++i) {
            SDL_Rect rct = dropdown_item_rect(MENU_SCALE, i);
            rct.x *= scale;
            rct.y *= scale;
            rct.w *= scale;
            rct.h *= scale;
            if (SCALE_VALUES[i] == scale)
                SDL_SetRenderDrawColor(r, c_dd_hl.r, c_dd_hl.g, c_dd_hl.b, 255);
            else
                SDL_SetRenderDrawColor(r, c_dd.r, c_dd.g, c_dd.b, 255);
            SDL_RenderFillRect(r, &rct);
            char label[8];
            snprintf(label, sizeof label, "%dx", SCALE_VALUES[i]);
            draw_text_centered(r, font, label, rct, c_text, scale);
        }
    } else if (open_menu == MENU_LANG) {
        for (int i = LANG_EN; i <= LANG_ZH; ++i) {
            SDL_Rect rct = dropdown_item_rect(MENU_LANG, i);
            rct.x *= scale;
            rct.y *= scale;
            rct.w *= scale;
            rct.h *= scale;
            if (lang == i)
                SDL_SetRenderDrawColor(r, c_dd_hl.r, c_dd_hl.g, c_dd_hl.b, 255);
            else
                SDL_SetRenderDrawColor(r, c_dd.r, c_dd.g, c_dd.b, 255);
            SDL_RenderFillRect(r, &rct);
            const char *label = (i == LANG_EN) ? "English"
                                               : "\xe7\xae\x80\xe4\xbd\x93\xe4\xb8\xad\xe6\x96\x87"; /* 简体中文 */
            draw_text_centered(r, font, label, rct, c_text, scale);
        }
    }
}
