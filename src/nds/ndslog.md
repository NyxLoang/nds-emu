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

## 2026-08-09 · 阶段 6 io 模块挂入整机

- `nds_t` 新增 `io_t *io` 字段（前向声明），`nds_create` 依次创建 bus → io → cpu，并 `bus->io = nds->io`；`nds_destroy` 逆序释放 cpu → io → bus。
- 职责划分：bus 负责地址换算，io 负责寄存器语义（中断/定时器/按键），CPU 经 io 接口读 IF/KEYINPUT、推进定时器。

## 2026-08-09 · 阶段 7 io 增加 bus 反指

- `nds_create` 里 `io->bus = nds->bus`：io 模块需要经 bus 访存（DMA 搬运的源/目的可落在任意区间），与 `bus->io` 对称建立双向联系。

## 2026-08-09 · 阶段 8 挂入第二颗 CPU（cpu7）

- `nds_t` 新增 `arm_cpu_t *cpu7`（ARM7）；`nds_create` 改用 `arm9_create(nds)` + `arm7_create(nds)`
  创建两核（不再直接 `cpu_create`），`nds_destroy` 逆序释放 cpu7 → cpu → io → bus。
- ARM7 镜像装载后由 main 调 `cpu_reset(nds->cpu7, arm7.entry)` 指到入口（`0x03800000`）。
