#ifndef NDS_EMU_WINDOW_H
#define NDS_EMU_WINDOW_H

#include <stddef.h>
#include <SDL.h>

#define GAME_W 256
#define GAME_H 384 /* 顶屏 256x192 + 底屏 256x192 */
#define MENU_H 28
#define WIN_W GAME_W
#define WIN_H (MENU_H + GAME_H)

/* 初始化 SDL/TTF、创建窗口与渲染器并设置 nearest hint。
   失败返回非 0，错误信息写入 err。 */
int window_init(char *err, size_t errsz);

/* 逆序清理并 SDL_Quit（menu_shutdown 应先于本函数调用） */
void window_shutdown(void);

SDL_Renderer *window_get_renderer(void);

int window_get_scale(void);

/* 更新缩放倍数并按 WIN_W*scale × WIN_H*scale 调整窗口大小 */
void window_set_scale(int scale);

/* QUIT / ESC 返回 1（应退出），其余返回 0 */
int window_handle_event(const SDL_Event *e);

#endif /* NDS_EMU_WINDOW_H */
