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

## 2026-08-08 · 拆分 window 与 menu 模块

- **做了什么**：
  - `main.c`（325 行）拆成三部分：`src/window/window.h/.c`（窗口/渲染器/缩放/退出）、`src/menu/menu.h/.c`（字体/文本/菜单 UI/语言），`main.c` 只留主循环编排与游戏区占位。
  - `CMakeLists.txt` 加入两个新源文件，并加 `target_include_directories(nds-emu PRIVATE src)` 统一 include 前缀。
  - 依赖方向单向：menu 的缩放变更经返回值通知 main → `window_set_scale`，window/menu 互不直接调用。
- **怎么验证**：构建通过；运行后缩放、语言、点击行为与拆分前一致。
- **结果**：✅ 通过。

## 2026-08-08 · 阶段 0.6 顶/底屏两种纯色

- **做了什么**：
  - `window.h` 新增单屏尺寸常量 `SCREEN_W 256` / `SCREEN_H 192`，`GAME_W`/`GAME_H` 改为引用（`GAME_H = SCREEN_H * 2`）。
  - `main.c` 游戏区占位从一块纯黑拆成两块纯色：顶屏蓝 `(24,90,200)`、底屏绿 `(24,160,60)`，各 `SCREEN_W × SCREEN_H`，随 scale 缩放。
- **怎么验证**：`cmake --build build` 通过；运行 `.\build\nds-emu.exe` 窗口菜单栏下方上下两屏颜色不同（上蓝下绿）。
- **结果**：✅ 编译通过（待用户运行确认）。

## 2026-08-09 · 阶段 6 中断/定时器/按键接入

- **做了什么**：
  - 主循环每帧跑完固定步数后 `io_set_vblank(nds->io)`：指令计数近似产生 VBlank（IF bit3）。
  - 最小 IRQ 检测：`io_irq_pending()` 首次为真时打印一次 `irq: IRQ pending (VBlank ...)`（不进异常向量）。
  - SDL 键映射：事件循环捕获 `SDL_KEYDOWN/UP`，`z/x/s/d/a/f/回车/退格/方向键` → NDS 键位，维护 pressed 掩码后 `io_set_keyinput`（组合根在 main.c，window/menu 不依赖机器）。
- **怎么验证**：`cmake --build build` + `ctest` 70 项检查 0 失败；运行 `nds-emu.exe` 看控制台 VBlank/IRQ 打印，按方向键/ABXY 验证按键映射。
- **结果**：✅ 编译与自动化测试通过（窗口按键行为待用户运行确认）。

## 2026-08-09 · 阶段 8 装载 ARM7 镜像 + 双核交错调度

- **做了什么**：
  - 装载 ARM7 镜像：与 ARM9 装载对称，把 ROM 头 ARM7 镜像逐字节经 `bus_write8` 写入
    ARM7 WRAM（`0x03800000`），`bus_read32` 读回验证，`cpu_reset(nds->cpu7, arm7.entry)` 指到入口。
  - 交错调度：主循环每帧 `steps_per_frame` 步按 `i%3==2` 跑 ARM7、其余跑 ARM9（约 2:1），
    每 60 周期打印一次双核 PC/cycles。
- **配套**：`make_fake_rom.py` 的 ARM7 镜像由 `BX lr` 占位改为 `B self` 死循环，让 ARM7 停在自己
  WRAM 里空转（与 ARM9 对称），避免跳到地址 0 空转。
- **怎么验证**：`ctest` 117 项检查 0 失败；运行 `nds-emu.exe ..\homebrew\mini.nds` 看到
  `image : loaded 128 bytes into ARM7 WRAM @ 03800000`、`cpu7 : reset PC=03800000`，主循环打印
  `ARM9 PC=02000800 | ARM7 PC=03800000`（两核各自死循环）。
- **结果**：✅ 编译与自动化测试通过。

## 2026-08-15 · 阶段 14.4 装载时解密安全区

- **做了什么**：`main.c` 在解析 ROM 头后、拷贝 ARM9 镜像前调用 `cart_decrypt_secure_area(cart)`，
  就地解密 ROM 缓冲里 ARM9 镜像开头的 0x800 字节安全区（KEY1），使后续 `bus_write8` 拷进 Main RAM 的
  已是明文；打印 `secure: KEY1 area decrypted (encryObj OK)` 或 `secure: not encrypted (homebrew) or decrypt failed`。
- **怎么验证**：`test_secure_area_load` 走完整装载流程通过；`ctest` 360 项检查 0 失败。
- **结果**：✅ 通过。
