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

## 运行测试

测试是**统一入口** `tests/test_nds.c`，只链核心库 `ndscore`（不依赖 SDL、不开窗口），
覆盖：bus 读写换算/小端、ARM 指令集（MOV/ADD/SUB/CMP/LDR/STR/B/BL/BX/AND/ORR/EOR）、
CPU 写 VRAM 出图、以及三个验收用例（清屏、画矩形、死循环保活）。

```sh
cmake -S . -B build        # 配置（生成 nds_test 目标）
cmake --build build        # 构建模拟器 + 测试
ctest --test-dir build     # 一键跑全部测试（等价的直接命令见下）
```

`ctest` 内部就是运行 `build/test_nds.exe`，也可以直接执行它：

```sh
./build/test_nds.exe
```

- 退出码 `0` = 全部通过；非 `0` = 有失败（并打印 `FAIL` 与期望/实际值）。
- 新增用例：在 `tests/test_nds.c` 里加一个 `test_xxx(nds)` 函数并在 `main` 中调用即可；
  断言用 `CHECK_EQ(name, got, want)` 宏。

## 目录结构

见 `docs/01-project-layout.md`。

## 开发日志

- **总索引**：[`DEVLOG.md`](DEVLOG.md)（只做索引）
- **模块详情**：放在各模块自己的目录下，命名 `<模块名>log.md`（如 `src/mainlog.md`），模板见 `devlog/_template.md`

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

