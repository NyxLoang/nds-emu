# menulog

> 覆盖：菜单 UI（字体、文本渲染、菜单栏/下拉、语言切换），即 `src/menu/menu.h` / `src/menu/menu.c`。
> 按时间从旧到新记录。

## 2026-08-08 · 从 main.c 拆出 menu 模块

- **做了什么**：
  - 新建 `src/menu/menu.h` / `menu.c`，把 main.c 里的字体候选与加载、中文字形检测、布局矩形、超采样 `draw_text_centered`、菜单栏/下拉绘制、点击命中、语言状态搬入。
  - 对外接口：`menu_init` / `menu_shutdown` / `menu_handle_click` / `menu_render_bar` / `menu_render_dropdown`。
  - 缩放变更经 `menu_handle_click` 返回值通知 main，由 main 调 `window_set_scale` 中转，menu 不直接依赖 window。
- **怎么验证**：`cmake --build build` 编译链接通过；运行后 1x/2x/3x/4x 缩放、中英文切换、点击准确，行为与拆分前一致。
- **结果**：✅ 通过。
