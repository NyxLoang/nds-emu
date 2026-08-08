# 开发日志（总索引）

> 本文件只做索引，不写细节。每个功能模块的详细日志放在**模块自己的目录**下，命名 `<模块名>log.md`（如 `src/cpu/cpulog.md`）。
> 新建模块日志时，先复制 `devlog/_template.md`，再在本页两个表中各加一行。

## 模块日志清单

| 模块 | 日志文件 | 职责 |
|------|----------|------|
| 窗口 / 主循环 | [`src/mainlog.md`](src/mainlog.md) | 工程骨架、SDL2、窗口与主循环（`src/main.c`） |

## 按时间索引

| 日期 | 微步 | 模块 | 一句话说明 | 详情 |
|------|------|------|------------|------|
| 2026-08-08 | 0.1 | 窗口/主循环 | 安装 WinLibs 工具链（gcc 16.1 / cmake 4.4） | [main](src/mainlog.md) |
| 2026-08-08 | 0.2 | 窗口/主循环 | CMakeLists + main.c 打印 `nds-emu` | [main](src/mainlog.md) |
| 2026-08-08 | 0.3 | 窗口/主循环 | FetchContent 本地 URL 引入 SDL2 2.26.3 | [main](src/mainlog.md) |
| 2026-08-08 | — | 工程管理 | git 分支工作流（main 保护 / develop 默认）+ 开发日志体系建立 | [main](src/mainlog.md) |
