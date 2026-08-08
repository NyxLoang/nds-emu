#ifndef NDS_EMU_MENU_H
#define NDS_EMU_MENU_H

#include <SDL.h>

/* 加载系统字体并检测中文字形。失败返回非 0。 */
int menu_init(SDL_Renderer *r);

/* 关闭字体资源（须在 window_shutdown 之前调用） */
void menu_shutdown(void);

/* 处理逻辑坐标下的左键点击。
   命中缩放下拉项时返回新缩放值（>=1），其余情况返回 0。 */
int menu_handle_click(int lx, int ly);

/* 绘制菜单栏（两项），rect 与字号按 scale 放大 */
void menu_render_bar(SDL_Renderer *r, int scale);

/* 绘制下拉菜单（若当前打开），rect 与字号按 scale 放大 */
void menu_render_dropdown(SDL_Renderer *r, int scale);

#endif /* NDS_EMU_MENU_H */
