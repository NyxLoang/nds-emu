# 开发日志（总索引）

> 本文件只做索引，不写细节。每个功能模块一个目录 `src/<模块名>/`，源码与日志同目录：内含 `<模块名>.h`、`<模块名>.c`、`<模块名>log.md`（如 `src/cpu/` 内含 `cpu.h`、`cpu.c`、`cpulog.md`）。
> 新建模块时，先复制 `devlog/_template.md`，再在本页两个表中各加一行。

## 模块日志清单

| 模块 | 日志文件 | 职责 |
|------|----------|------|
| 窗口 / 主循环 | [`src/mainlog.md`](src/mainlog.md) | 工程骨架、SDL2、窗口与主循环（`src/main.c`） |
| 窗口 | [`src/window/windowlog.md`](src/window/windowlog.md) | 窗口/渲染器生命周期、缩放、退出事件 |
| 菜单 | [`src/menu/menulog.md`](src/menu/menulog.md) | 菜单 UI、字体、文本渲染、语言切换 |
| 整机状态 | [`src/nds/ndslog.md`](src/nds/ndslog.md) | 整机状态容器 `nds_t`（`src/nds/nds.h` / `src/nds/nds.c`） |

## 按时间索引

| 日期 | 微步 | 模块 | 一句话说明 | 详情 |
|------|------|------|------------|------|
| 2026-08-08 | 0.1 | 窗口/主循环 | 安装 WinLibs 工具链（gcc 16.1 / cmake 4.4） | [main](src/mainlog.md) |
| 2026-08-08 | 0.2 | 窗口/主循环 | CMakeLists + main.c 打印 `nds-emu` | [main](src/mainlog.md) |
| 2026-08-08 | 0.3 | 窗口/主循环 | FetchContent 本地 URL 引入 SDL2 2.26.3 | [main](src/mainlog.md) |
| 2026-08-08 | — | 工程管理 | git 分支工作流（main 保护 / develop 默认）+ 开发日志体系建立 | [main](src/mainlog.md) |
| 2026-08-08 | — | 窗口/菜单 | 拆分 window 与 menu 为独立模块 | [main](src/mainlog.md) · [window](src/window/windowlog.md) · [menu](src/menu/menulog.md) |
| 2026-08-08 | P3 | 预习 | 新增 `docs/00b-hex-endianness.md`：十六进制与小端、`bus_read/write` 拼接影响 | [00b](docs/00b-hex-endianness.md) |
| 2026-08-08 | 0.5 | 整机状态 | 定义空 `nds_t`（`nds.h`/`nds.c` + create/destroy），`main` 创建一份 | [nds](src/nds/ndslog.md) |
| 2026-08-08 | 0.6 | 窗口/主循环 | 顶/底屏两种纯色区分（上蓝下绿），`SCREEN_W/H` 单屏常量 | [main](src/mainlog.md) |
