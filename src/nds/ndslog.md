# ndslog

> 覆盖：整机状态容器 `nds_t`，即 `src/nds/nds.h` / `src/nds/nds.c`。后续 bus / cpu / ppu 等模块都挂到这台机器上。
> 按时间从旧到新记录。

## 2026-08-08 · 阶段 0.5 定义空 nds_t

- **做了什么**：
  - 新建 `src/nds/nds.h` / `src/nds/nds.c`：定义空结构体 `nds_t`（注释占位，等待 bus/cpu/ppu 挂入），并提供 `nds_create` / `nds_destroy`。
  - `main.c` 在初始化 SDL/TTF 后 `nds_create()` 创建一台机器，退出前 `nds_destroy`；失败则报错退出。
  - `CMakeLists.txt` 的 `add_executable` 加入 `src/nds/nds.c`。
  - 2026-08-08：模块目录化，`nds.h` / `nds.c` / 本日志移入 `src/nds/`；`main.c` include 改为 `"nds/nds.h"`。
- **怎么验证**：`cmake --build build` 编译链接通过（首次链接因旧 `nds-emu.exe` 被短暂占用报 Permission denied，重试即通过）；运行 `.\build\nds-emu.exe` 行为与 0.4 一致（窗口/菜单无可见变化，本步仅建结构体）。
- **结果**：✅ 编译链接通过（待用户运行确认）。
