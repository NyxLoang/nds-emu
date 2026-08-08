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
