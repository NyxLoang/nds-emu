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
| 显示 | [`src/ppu/ppulog.md`](src/ppu/ppulog.md) | 读 VRAM framebuffer 转 SDL 纹理上屏、缩放（`src/ppu/ppu.h` / `src/ppu/ppu.c`） |
| IO 寄存器 | [`src/io/iolog.md`](src/io/iolog.md) | 中断 IME/IE/IF、定时器 0-3、KEYINPUT（`src/io/io.h/.c` + 功能文件 irq/timer/key） |
| 测试 | [`tests/testlog.md`](tests/testlog.md) | 统一测试入口 `tests/test_nds.c`（bus/指令/显示/清屏/矩形/死循环/中断/定时器/按键） |

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
| 2026-08-09 | 4.1 | 显示 | 新增 `docs/05-framebuffer.md`：像素与 RGB555/888、线性 framebuffer | [05](docs/05-framebuffer.md) |
| 2026-08-09 | 4.2 | 显示 | 约定：顶屏=VRAM 起 256×192 RGB555，底屏=VRAM+`0x18000` | [ppu](src/ppu/ppulog.md) |
| 2026-08-09 | 4.3 | 显示 | `ppu` 模块：`bus_read16` 读 VRAM → RGB555 转 RGB888 → SDL 纹理上屏 | [ppu](src/ppu/ppulog.md) |
| 2026-08-09 | 4.4 | 显示 | 顶屏 6 色带+白框测试图、底屏纯蓝（主机直写 VRAM 验证管线） | [ppu](src/ppu/ppulog.md) |
| 2026-08-09 | 4.5 | 显示 | 模拟 CPU 跑写 VRAM 的测试码出图（黄十字顶屏+底屏纯绿），修复 3 个手工汇编编码错误 | [ppu](src/ppu/ppulog.md) |
| 2026-08-09 | 4.6 | 显示 | 默认 2× 缩放启动，菜单可切 1x/2x，双屏比例不变，阶段 4 完成 | [ppu](src/ppu/ppulog.md) |
| 2026-08-09 | 5.1 | 测试 | 抽 `ndscore` 核心库 + `tests/test_nds.c` 统一测试入口 + `ctest`，迁移 bus/3b/4.5 自测 | [tests](tests/testlog.md) |
| 2026-08-09 | 5.2 | 测试 | 用例：清屏（CPU 整屏填约定底色 `0x0000`，先涂乱码再验证全清） | [tests](tests/testlog.md) |
| 2026-08-09 | 5.3 | 测试 | 用例：画矩形（左上象限 128×96 红）；修复「STR 32 位写只着 1 像素」bug | [tests](tests/testlog.md) |
| 2026-08-09 | 5.4 | 测试 | 用例：死循环保活（10 万步不崩、PC 稳定、cycles=100000） | [tests](tests/testlog.md) |
| 2026-08-09 | 5.5 | 测试 | README 新增「运行测试」小节（构建/ctest/直接运行/退出码/新增用例） | [tests](tests/testlog.md) |
| 2026-08-09 | 5.6 | 测试 | 新增 `docs/06-devkitarm.md`：未来 devkitARM 接入说明（仅文档） | [tests](tests/testlog.md) |
| 2026-08-09 | 4.5修 | 显示 | 修复十字 demo：竖线只画上半屏（终点 `+0xC000` 应 `+0x18000`）；横线 1px vs 竖线 2px 粗细不一（横线改双行嵌套循环） | [ppu](src/ppu/ppulog.md) |
| 2026-08-09 | 6.1 | 预习 | 新增 `docs/07-interrupts.md`：中断=CPU 被打断去跑处理程序；IME/IE/IF 三者配合 | [07](docs/07-interrupts.md) |
| 2026-08-09 | 6.2 | IO 寄存器 | 新建 `src/io/` 模块（接口 + irq/timer/key 功能文件）；bus IO 桩改路由 io 模块；IME/IE/IF 读写、IF 写 1 清除 | [io](src/io/iolog.md) |
| 2026-08-09 | 6.3 | IO 寄存器 | 指令计数近似产生 VBlank：主循环每帧跑完 N 步 `io_set_vblank`，IF bit3 置位 | [io](src/io/iolog.md) |
| 2026-08-09 | 6.4 | IO 寄存器 | 最小 IRQ 检测：`(IF & IE & IME)!=0`，首次 pending 打印一次（不进异常向量） | [io](src/io/iolog.md) |
| 2026-08-09 | 6.5 | IO 寄存器 | Timers 0-3：CNT_L/H、bit7 使能、bit0-1 分频(1/64/256/1024)、每条指令 tick；修字节偏移 `<<32` UB bug | [io](src/io/iolog.md) · [cpu](src/cpu/cpulog.md) |
| 2026-08-09 | 6.6 | IO 寄存器 | KEYINPUT（按下=0）+ SDL 键映射（main.c 组合根，window/menu 不依赖机器） | [io](src/io/iolog.md) · [main](src/mainlog.md) |
| 2026-08-09 | 6.7 | 测试 | 等 VBlank 轮询程序（置位前卡循环、置位后写屏）+ 读键程序（按 A 蓝/未按红）；修 3 个手写汇编编码错误 | [tests](tests/testlog.md) |
| 2026-08-09 | 6 完成 | 收尾 | 阶段 6 完成：io 模块 + VBlank/IRQ 检测/定时器/按键；70 项检查 0 失败 | [io](src/io/iolog.md) · [tests](tests/testlog.md) |
