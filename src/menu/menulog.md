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

## 2026-09-13 · 21-B9yi（续70）：菜单文字纹理缓存（SDL 侧 2730 → 540 ms/2000 帧）

- **问题**：`draw_text_centered()` 每次调用都做
  `TTF_RenderUTF8_Blended`（栅格化）→ `SDL_CreateTextureFromSurface`（上传）
  → 画完 `SDL_DestroyTexture` **销毁**；而菜单栏（缩放/语言两个标签）与下拉菜单
  **每帧**都要重画 —— 窗口模式的 per-frame 计时里「SDL 侧」恒定 1.37 ms/帧全在这里。
- **修复**：加一个 24 项的**文字纹理缓存**，用「文本指针 + 颜色 + 缩放」做键
  （标签都是字符串字面量 ⇒ 指针稳定；缩放不同会命中不同键 ⇒ 不需要失效逻辑），
  命中时直接 `SDL_RenderCopy` 缓存纹理与缓存的尺寸。
- **怎么验证**（窗口模式 + 键盘脚本，每 2000 帧）：
  ```
  SDL(clear/menu/present) 段：2730 ms → 540 ms（约 1/5）
  窗口 fps：0–2000 段 66.1 → 69.2、2000–4000 段 68.8 → 72.6、4000–6000 段 70.2 → 74.2
  ```
  菜单仍可正常点击切换缩放/语言（缓存键含缩放，切档后自动用新纹理）。
- **结果**：✅ 保留。
