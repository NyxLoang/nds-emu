# nds-emu

一个学习用 NDS 模拟器（C11 + CMake + SDL2），按 `NDS-微步骤拆解.md` 分阶段开发。

## 依赖

- **工具链**：C11 编译器 + CMake 3.16+（本机用 WinLibs 的 gcc 16.1 / cmake 4.4）
- **SDL2 2.26.3**：源码包放在 `tools/sdl2/`，由 CMake FetchContent 在配置时自动解压编译，构建过程完全离线，不访问 GitHub。

  > 若需更新 SDL2：把新源码包放进 `tools/sdl2/`，改 `CMakeLists.txt` 里 `FetchContent_Declare(SDL2 ...)` 的 `URL` 与 `URL_HASH`（`Get-FileHash -Algorithm SHA256` 可得）。

## 构建与运行

```sh
cmake -S . -B build
cmake --build build
build/nds-emu.exe
```

`main` 可执行文件需要 SDL 的 `SDL2main` 入口（Windows 窗口子系统），CMakeLists 已链接。

## 目录结构

见 `docs/01-project-layout.md`。

## 分支工作流

采用 git-flow 精简版：

```
main ──────────── 只放稳定版本（受保护，只能 PR 合并）
develop ───────── 日常开发主线（默认分支）
  ├─ feature/*   功能分支：从 develop 拉出，完成后 PR 合回 develop
  └─ debug/*     Bug 修复分支：从 develop 拉出，修复后 PR 合回 develop
```

- **develop**：GitHub 默认分支，所有开发都在此进行。
- **main**：受保护分支，`enforce_admins=true` 且要求 1 个 review，只能通过 PR 合并，禁止直接 push。
- **Bug 修复**：从 `develop` 拉出 `debug/<名字>` 分支，修复并自测后 PR 合回 `develop`；功能稳定需要发布时再从 `develop` PR 合到 `main`。
- 本机推送需走代理 `http://127.0.0.1:7897`（已写入本仓库 git 配置 `http.proxy`）。

