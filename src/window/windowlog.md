# windowlog

> 覆盖：窗口与渲染器生命周期、缩放管理、退出事件，即 `src/window/window.h` / `src/window/window.c`。
> 按时间从旧到新记录。

## 2026-08-08 · 从 main.c 拆出 window 模块

- **做了什么**：
  - 新建 `src/window/window.h` / `window.c`，把 main.c 里的 SDL/TTF 初始化、窗口/渲染器创建、`SDL_HINT_RENDER_SCALE_QUALITY=nearest`、scale 状态与 `SDL_SetWindowSize` 调整、QUIT/ESC 退出处理搬入。
  - 对外接口：`window_init` / `window_shutdown` / `window_get_renderer` / `window_get_scale` / `window_set_scale` / `window_handle_event`。
  - 尺寸常量 `GAME_W/GAME_H/MENU_H/WIN_W/WIN_H` 移到 `window.h` 供 menu 模块复用 `MENU_H`。
- **怎么验证**：`cmake --build build` 编译链接通过；运行后 1x/2x/3x/4x 缩放、中英文切换、点击准确，行为与拆分前一致。
- **结果**：✅ 通过。
