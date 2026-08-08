# mainlog

> 覆盖：工程骨架、SDL2 引入、窗口与主循环，即 `src/main.c` 与根 `CMakeLists.txt` 相关部分。
> 按时间从旧到新记录。

## 2026-08-08 · 阶段 0.1 工具链

- **做了什么**：本机无编译器/CMake。winget 安装 WinLibs（gcc 16.1.0 + cmake 4.4.1 + ninja，POSIX/UCRT），自动写入用户 PATH。
- **怎么验证**：`gcc --version`、`cmake --version` 输出版本号。
- **结果**：✅ 通过。注意：已运行的程序需刷新 PATH 或重启才能识别新命令。

## 2026-08-08 · 阶段 0.2 工程骨架

- **做了什么**：根 `CMakeLists.txt`（C11，`add_executable(nds-emu src/main.c)`）+ `src/main.c` 打印 `nds-emu`；`cmake -S . -B build` + `cmake --build build` 产出 `build/nds-emu.exe`。
- **怎么验证**：运行 `.\build\nds-emu.exe` 输出 `nds-emu`；清空 PATH 仍可运行（无动态依赖）。
- **结果**：✅ 通过。

## 2026-08-08 · 阶段 0.3 引入 SDL2（FetchContent 本地 URL）

- **做了什么**：
  - 原计划 FetchContent 从 GitHub 拉源码 → 直连 GitHub 不通（`git ls-remote` 无响应）。
  - 改为经代理 `ghfast.top` 下载 SDL 2.26.3 官方源码包至 `tools/sdl2/`，`FetchContent_Declare` 用本地 `URL` + `URL_HASH`（SHA256 校验），构建完全离线。
  - 链接 `SDL2-static` 与 `SDL2main`；`main.c` 用标准签名 `int main(int, char**)` 并打印 SDL 版本。
- **踩坑**：
  1. SDL 2.26.3 的 CMake 脚本声明兼容 <3.5，被 CMake 4.4 拒绝 → 设 `CMAKE_POLICY_VERSION_MINIMUM=3.5`。
  2. Windows 下 `SDL_main.h` 把 `main` 宏替换为 `SDL_main`，要求标准签名并链接 `SDL2main`。
- **怎么验证**：运行输出 `nds-emu (SDL 2.26.3)`。
- **结果**：✅ 通过。

## 2026-08-08 · 分支工作流与开发日志体系

- **做了什么**（无代码，纯工程管理）：
  - git 分支：`main`（受保护，仅 PR 合并）+ `develop`（默认分支，日常开发）；bug 修复从 develop 拉 `debug/*`。
  - 新增本模块日志体系：根 `DEVLOG.md` 索引 + `devlog/<模块>.md` 详情。
- **怎么验证**：`git branch -a`、`gh repo view --json defaultBranchRef`。
- **结果**：✅ 通过。
