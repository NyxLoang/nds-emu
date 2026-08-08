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
| 卡带装载 | [`src/cart/cartlog.md`](src/cart/cartlog.md) | 读 `.nds` 文件、解析 ROM 头（`src/cart/cart.h` / `src/cart/cart.c`） |
| 内存总线 | [`src/bus/buslog.md`](src/bus/buslog.md) | Main RAM / VRAM / IO 地址换算与 8/16/32 读写（`src/bus/bus.h` / `src/bus/bus.c`） |
| CPU | [`src/cpu/cpulog.md`](src/cpu/cpulog.md) | ARM9 状态、取指/单步框架、指令执行（`src/cpu/cpu.h/.c` + 功能文件 `src/cpu/exec.h/.c`） |

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
| 2026-08-08 | 1.1 | 预习/装载 | 新增 `docs/02-nds-rom-header.md`：ARM9/ARM7 头四字段与偏移 | [02](docs/02-nds-rom-header.md) |
| 2026-08-08 | 1.2 | 卡带装载 | `cart` 读整个文件到缓冲区，`argv[1]` 传路径，打印文件大小 | [cart](src/cart/cartlog.md) |
| 2026-08-08 | 1.3 | 卡带装载 | 按偏移解析 ARM9 头四字段并打印（小端 `read_le32`） | [cart](src/cart/cartlog.md) |
| 2026-08-08 | 1.3+ | 卡带装载 | 修复中文/Unicode ROM 路径打不开（`cart_load_w` + `CommandLineToArgvW`） | [cart](src/cart/cartlog.md) |
| 2026-08-08 | 1.4 | 卡带装载 | cart 拆 arm9/arm7 功能文件（接口+功能），解析 ARM7 头四字段 | [cart](src/cart/cartlog.md) |
| 2026-08-08 | 1.5 | 卡带装载 | 最小假 `.nds`：`homebrew/make_fake_rom.py` + `mini.nds` | [cart](src/cart/cartlog.md) |
| 2026-08-08 | 1.6 | 卡带装载 | ARM9 镜像拷入 RAM 缓冲区（裸数组过渡），首字节对照 | [cart](src/cart/cartlog.md) |
| 2026-08-08 | 1.7 | 卡带装载 | CLI 装载摘要 `=== NDS cartridge ===`（file/arm9/arm7/image） | [cart](src/cart/cartlog.md) |
| 2026-08-08 | 2.1 | 内存总线 | 新增 `docs/03-memory-map.md`：Main RAM / VRAM / IO 地址区间与换算 | [03](docs/03-memory-map.md) |
| 2026-08-08 | 2.2 | 内存总线 | `bus` 分配 Main RAM/VRAM/IO 数组，挂到 `nds_t`，`calloc` 清零 | [bus](src/bus/buslog.md) |
| 2026-08-08 | 2.3 | 内存总线 | `bus_read8/write8` + `bus_resolve` 区间命中换算，未映射读 0 写忽略 | [bus](src/bus/buslog.md) |
| 2026-08-08 | 2.4 | 内存总线 | `read16/write16` 小端拼拆（写 `0xABCD` 字节序 `CD AB`） | [bus](src/bus/buslog.md) |
| 2026-08-08 | 2.5 | 内存总线 | `read32/write32` 小端拼拆（`0x12345678` → `78 56 34 12`） | [bus](src/bus/buslog.md) |
| 2026-08-08 | 2.6 | 内存总线 | VRAM 区间映射（`0x06000000` 656KB），写读回一致 | [bus](src/bus/buslog.md) |
| 2026-08-08 | 2.7 | 内存总线 | IO 桩：`0x04000000` 区间读 0、写忽略，访问不崩 | [bus](src/bus/buslog.md) |
| 2026-08-08 | 2.8 | 内存总线 | 装载衔接：ARM9 镜像经 `bus_write8` 装入 Main RAM，`bus_read32` 读回 `EAFFFFFE` | [bus](src/bus/buslog.md) |
| 2026-08-08 | 3a.1 | CPU | 新增 `docs/04-cpu-loop.md`：取指-执行循环、PC、B 指令位模式 | [04](docs/04-cpu-loop.md) |
| 2026-08-08 | 3a.2 | CPU | `arm_cpu_t`：`r[16]`（r15=PC）、`cpsr`、`cycles`，挂入 `nds_t` | [cpu](src/cpu/cpulog.md) |
| 2026-08-08 | 3a.3 | CPU | `cpu_fetch`：按 PC 从 bus 读 32 位指令字 | [cpu](src/cpu/cpulog.md) |
| 2026-08-08 | 3a.4 | CPU | `cpu_step`：取指后 PC+=4（非分支默认推进） | [cpu](src/cpu/cpulog.md) |
| 2026-08-08 | 3a.5 | CPU | 识别「无条件 B 跳自己」死循环（`EAFFFFFE`），PC 原地打转 | [cpu](src/cpu/cpulog.md) |
| 2026-08-08 | 3a.6 | CPU | 未实现指令：打印机器码并计数（阶段 3b 逐类实现） | [cpu](src/cpu/cpulog.md) |
| 2026-08-08 | 3a.7 | CPU | 主循环每帧执行固定 N 步；修复 `mini.nds` 布局（ram=entry） | [cpu](src/cpu/cpulog.md) |
| 2026-08-08 | 3b.1 | CPU | 条件码执行框架（15 种条件按 N/Z/C/V 判断，不满足跳过） | [cpu](src/cpu/cpulog.md) |
| 2026-08-08 | 3b.2 | CPU | `MOV` 立即数（`imm8 ROR (rot4*2)` 旋转编码） | [cpu](src/cpu/cpulog.md) |
| 2026-08-08 | 3b.3 | CPU | `ADD`/`SUB` 立即数或寄存器，更新进位/溢出标志 | [cpu](src/cpu/cpulog.md) |
| 2026-08-08 | 3b.4 | CPU | `CMP` + flags 更新（N/Z/C/V，只比较不写回） | [cpu](src/cpu/cpulog.md) |
| 2026-08-08 | 3b.5 | CPU | `LDR` 立即偏移（前变址 ±offset，`bus_read32`） | [cpu](src/cpu/cpulog.md) |
| 2026-08-08 | 3b.6 | CPU | `STR` 立即偏移（`bus_write32`） | [cpu](src/cpu/cpulog.md) |
| 2026-08-08 | 3b.7 | CPU | `B` 相对跳转（PC+8 语义）；修复分支符号扩展 bug | [cpu](src/cpu/cpulog.md) |
| 2026-08-08 | 3b.8 | CPU | `BL`+`BX lr` 最小调用返回（lr=PC+4） | [cpu](src/cpu/cpulog.md) |
| 2026-08-08 | 重构 | CPU | 指令执行拆分为 `src/cpu/exec.h/.c` 功能文件（接口 `cpu.h/.c` 只留框架），行为不变 | [cpu](src/cpu/cpulog.md) |
| 2026-08-08 | 3b.9 | CPU | `AND`/`ORR`/`EOR` 位运算（逻辑运算按 S 位更新 N/Z） | [cpu](src/cpu/cpulog.md) |
| 2026-08-08 | 3b.10 | CPU | 综合：纯机器码把 RGB555 红色 `0x7C00` 写进 VRAM 并读回，阶段 3b 完成 | [cpu](src/cpu/cpulog.md) |
