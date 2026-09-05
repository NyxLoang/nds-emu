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
| 卡带装载 | [`src/cart/cartlog.md`](src/cart/cartlog.md) | 读 `.nds` 文件、解析 ROM 头、KEY1 安全区解密、卡带总线 cartbus、存档 save（`src/cart/cart.h` / `cart.c` / `key1.*` / `cartbus.*` / `save.*`） |
| 内存总线 | [`src/bus/buslog.md`](src/bus/buslog.md) | Main RAM / VRAM / IO 地址换算与 8/16/32 读写（`src/bus/bus.h` / `src/bus/bus.c`） |
| CPU | [`src/cpu/cpulog.md`](src/cpu/cpulog.md) | ARM9/ARM7 状态、取指/单步框架、指令执行（`src/cpu/cpu.h/.c` + 功能文件 `src/cpu/arm9.h/.c`、`src/cpu/arm7.h/.c`、`src/cpu/exec.h/.c`） |
| 显示 | [`src/ppu/ppulog.md`](src/ppu/ppulog.md) | 真 2D PPU：读寄存器出图（`src/ppu/ppu.h/.c` SDL 侧 + 纯渲染 `src/ppu/render.h/.c`） |
| IO 寄存器 | [`src/io/iolog.md`](src/io/iolog.md) | 中断 IME/IE/IF（双核两套）、定时器 0-3、KEYINPUT、DMA 4 通道、IPC FIFO、显示控制 DISPCNT/BGxCNT、触摸屏 SPI、音频寄存器路由、卡带总线/存档接线（`src/io/io.h/.c` + 功能文件 irq/timer/key/dma/fifo/disp/touch） |
| 音频 | [`src/snd/sndlog.md`](src/snd/sndlog.md) | 16 通道音频寄存器 + 混音合成（PCM8/PCM16/IMA-ADPCM/PSG），`snd_render` 输出 32768Hz 立体声（`src/snd/snd.h/.c`） |
| 3D 几何 | [`src/gx/gxlog.md`](src/gx/gxlog.md) | 3D 几何引擎：DISP3DCNT/GXSTAT/GXFIFO 寄存器 + 命令解码 + 矩阵/顶点变换 + 软件光栅化（`src/gx/gx.h/.c`） |
| 测试 | [`tests/testlog.md`](tests/testlog.md) | 统一测试入口 `tests/test_nds.c`（bus/指令/显示/清屏/矩形/死循环/中断/定时器/按键/DMA/双核/FIFO/2D PPU/触摸/音频/3D） |

## 按时间索引

| 日期 | 微步 | 模块 | 一句话说明 | 详情 |
|------|------|------|------------|------|
| 2026-09-05 | 21-B9j（前半） | IO/测试 | 定时器 CNT_L reload 语义 + VBlank 双核置 IF；ARM7 能收到帧事件，service6 完成回执仍待追（664 项检查 0 失败） | [io](src/io/iolog.md) · [tests](tests/testlog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-05 | 21-B9i | CPU/IO/测试 | FIFO CNT 合并写修复（错误应答不吞使能位）+ ARM7 IRQ 槽跳板（0x0380FFFC）；ARM9 FIFO 命令能送达 ARM7 并完成 handler（659 项检查 0 失败），忙位回执待 B9j | [cpu](src/cpu/cpulog.md) · [io](src/io/iolog.md) · [tests](tests/testlog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-05 | 21-B9h | CPU/IO/测试 | LDM/STM ^ 的 User 槽语义（FFXII 任务队列不再成环）；VBlank 改回 bit0、Timer0-3 溢出置 IF bit3-6；headless 进度带 CPSR/IRQ 状态（649 项检查 0 失败），多帧 VBlank IRQ 可正常进入/恢复，新卡点 0x0200EA84 忙位待 B9i | [cpu](src/cpu/cpulog.md) · [io](src/io/iolog.md) · [tests](tests/testlog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-05 | 21-B9g | CPU | 模式私有 r13/r14（banked registers）：MSR/异常/返回统一走模式同步；FFXII 三栈隔离，ARM9 不再跑飞（643 项 0 失败） | [cpu](src/cpu/cpulog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-05 | 21-B9f | CPU | IRQ HLE 桩：跳 handler 前保存 r0-r3/r12/CPSR/返回点，返回后恢复；第一次 VBlank 链执行完，新卡点=模式共用 SP（633 项 0 失败） | [cpu](src/cpu/cpulog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-05 | 21-B9e | CPU | ARM BLX Rm 寄存器间接调用；ARM9 越过 0x02006488 回到主等待循环，8M 步无固定卡点（628 项 0 失败） | [cpu](src/cpu/cpulog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-05 | 21-B9d | CPU | CLZ 前导零指令（ARMv5）；FFXII IRQ 分发循环跑完，ARM9 前进到 0x02006488，新卡点=ARM BLX Rm（618 项 0 失败） | [cpu](src/cpu/cpulog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-05 | 21-B9c | CPU | ARM9 IRQ 槽跳板：异常后按 DTCM+0x3FFC 槽跳用户 handler；FFXII 进入 ITCM 中断分发器，新卡点=CLZ 缺失（608 项 0 失败） | [cpu](src/cpu/cpulog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-05 | 21-B9b | CPU | 首中断现场快照（IME/IE/IF + IRQ 槽）；确认 FFXII 把 IRQ handler 装在 DTCM+0x3FFC=0x01FF8000，异常 trace 加核标识（600 项 0 失败） | [cpu](src/cpu/cpulog.md) · [21](docs/21-rom-bringup.md) |
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
| 2026-08-09 | 7.1 | 预习 | 新增 `docs/08-dma.md`：DMA = 硬件搬内存（源/目的/长度/启动，CPU 不参与） | [08](docs/08-dma.md) |
| 2026-08-09 | 7.2 | IO 寄存器 | `src/io/dma.h/.c` 功能文件：DMA0 的 SAD/DAD/CNT_L/CNT_H 读写；io 加 bus 反指并路由 | [io](src/io/iolog.md) |
| 2026-08-09 | 7.3 | IO 寄存器 | 立即模式：写 CNT_H 使能位同步拷贝（16/32 位块、源/目的可固定），搬完自动清使能 | [io](src/io/iolog.md) |
| 2026-08-09 | 7.4 | 测试 | CPU 程序触发 DMA 填 VRAM 色块（128 像素红）；修 `ADD #0x1000` 立即数旋转编码 bug | [tests](tests/testlog.md) |
| 2026-08-09 | 7 完成 | 收尾 | 阶段 7 完成：DMA0 立即模式搬运/填色；90 项检查 0 失败 | [io](src/io/iolog.md) · [tests](tests/testlog.md) |
| 2026-08-09 | 8.1 | 预习 | 新增 `docs/09-dual-core.md`：双核分工、共享内存、IPC FIFO 概念 | [09](docs/09-dual-core.md) |
| 2026-08-09 | 8.2 | CPU | 拆分 `arm9.h/.c`/`arm7.h/.c` 功能文件（`cpu.h/.c` 作接口），`arm_cpu_t` 加 `is_arm7` | [cpu](src/cpu/cpulog.md) |
| 2026-08-09 | 8.3 | 内存总线 | ARM7 WRAM（`0x03800000` 64KB）映射 + 装载 ARM7 镜像 + cpu7 指入口 | [bus](src/bus/buslog.md) · [nds](src/nds/ndslog.md) · [main](src/mainlog.md) |
| 2026-08-09 | 8.4 | CPU | 双核交错调度 ARM9:ARM7=2:1（主循环 + 测试驱动） | [main](src/mainlog.md) · [cpu](src/cpu/cpulog.md) |
| 2026-08-09 | 8.5 | IO 寄存器 | 中断按 CPU 分流：IME/IE/IF 拆成 `irq[2]` 两套，同址按访问者身份选择 | [io](src/io/iolog.md) |
| 2026-08-09 | 8.6 | IO 寄存器 | IPC FIFO 完整：双向 16 字队列 + CNT 状态位 + SEND/RECV + 边沿中断 IF17/18 | [io](src/io/iolog.md) |
| 2026-08-09 | 8.7 | 测试 | 双核 FIFO 传值 + 底屏体现；117 项检查 0 失败 | [tests](tests/testlog.md) |
| 2026-08-09 | 8 完成 | 收尾 | 阶段 8 完成：ARM7 + IPC（双核调度、共享内存、FIFO 传字） | [cpu](src/cpu/cpulog.md) · [io](src/io/iolog.md) · [tests](tests/testlog.md) |
| 2026-08-14 | 9.1 | 显示 | 新增 `docs/10-bg-tile-palette.md`：BG/tile/tilemap/调色板、位图 vs tile 模式 | [10](docs/10-bg-tile-palette.md) |
| 2026-08-14 | 9.2 | 显示/IO | 新建 `src/io/disp.h/.c` 显示寄存器（DISPCNT/BGxCNT/滚动，主副引擎）+ bus 调色板 RAM/OAM/VRAM 固定窗口 | [disp](src/io/iolog.md) · [bus](src/bus/buslog.md) |
| 2026-08-14 | 9.3 | 显示 | 新建 `src/ppu/render.h/.c` 纯渲染器：直色位图模式（Mode 3-5，BG2/BG3 直色 16bpp） | [ppu](src/ppu/ppulog.md) |
| 2026-08-14 | 9.4 | 显示 | Mode 0 tile 图层渲染（4bpp/8bpp 位面、tilemap、调色板、透明、翻转、优先级） | [ppu](src/ppu/ppulog.md) |
| 2026-08-14 | 9.5 | 显示 | Engine B 对称渲染（副 DISPCNT/BGxCNT/调色板/VRAM 窗口 `0x06200000`） | [ppu](src/ppu/ppulog.md) |
| 2026-08-14 | 9.6 | 显示 | OBJ 最小：OAM 读取 + 1D tile 映射 + OBJ 调色板，一个 8×8 sprite（16/256 色） | [ppu](src/ppu/ppulog.md) |
| 2026-08-14 | 9.7 | 显示/测试 | 自造数据 2D 场景验收 + main 演示迁移（弃假 FB，改 DISPCNT/BGxCNT/OAM 驱动）；148 项检查 0 失败 | [ppu](src/ppu/ppulog.md) · [tests](tests/testlog.md) · [main](src/mainlog.md) |
| 2026-08-14 | 9 完成 | 收尾 | 阶段 9 完成：真 2D PPU（DISPCNT + BG tile + OBJ，主副引擎对称） | [ppu](src/ppu/ppulog.md) |
| 2026-08-15 | 10.1 | 预习/CPU | 新增 `docs/11-arm-instructions-full.md`：真码为何需要移位/MRS/MSR/LDM/STM/乘法/半字访存/SWI | [11](docs/11-arm-instructions-full.md) |
| 2026-08-15 | 10.2 | CPU | 移位操作数 LSL/LSR/ASR/ROR（立即 + 寄存器移位）+ 进位输出 | [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 10.3 | CPU | 更多数据处理 MVN/BIC/ADC/SBC/RSB/RSC/TST/TEQ/CMN | [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 10.4 | CPU | MRS/MSR 读写 CPSR/SPSR（字段掩码 + 模式位保护）；cpu.h 增 spsr/swi_num/cp15[16] | [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 10.5 | CPU | LDM/STM（PUSH/POP 别名，IA/IB/DA/DB + 写回） | [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 10.6 | CPU | 乘法 MUL/MLA + 长乘 UMULL/UMLAL/SMULL/SMLAL | [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 10.7 | CPU | 字节/半字访存 LDRB/STRB/LDRH/STRH/LDRSB/LDRSH + 前后变址/寄存器偏移 | [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 10.8 | CPU | SWI 记录软件中断号（暂不跳异常，阶段 11 BIOS HLE 拦截） | [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 10.9 | CPU | SWP/SWPB 寄存器与内存交换 | [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 10.10 | CPU | MRC/MCR 协处理器访问（CP15 桩，按 CRn 索引） | [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 10.11 | CPU/测试 | 综合真码（PUSH/POP + SWI + MUL + STRH 循环搬 VRAM）；204 项检查 0 失败 | [cpu](src/cpu/cpulog.md) · [tests](tests/testlog.md) |
| 2026-08-15 | 10 完成 | 收尾 | 阶段 10 完成：ARM 指令集补全 + SWI（移位/MRS/MSR/LDM/STM/乘法/半字访存/SWI/SWP/MRC/MCR） | [cpu](src/cpu/cpulog.md) · [tests](tests/testlog.md) |
| 2026-08-15 | 11.1 | 预习/BIOS | 新增 `docs/12-bios-hle.md`：BIOS/SWI/HLE 概念 + NDS SWI 编号表 + 寄存器约定 | [12](docs/12-bios-hle.md) |
| 2026-08-15 | 11.2 | BIOS | 新建 `src/bios/` 模块：`bios_dispatch` 分发框架；exec.c SWI 改 HLE 拦截（等待不前进 PC） | [bios](src/bios/bioslog.md) |
| 2026-08-15 | 11.3 | BIOS | Div(0x09)/Sqrt(0x0D)；注 DivArm 为 GBA 专属 | [bios](src/bios/bioslog.md) |
| 2026-08-15 | 11.4 | BIOS | CpuSet(0x0B)/CpuFastSet(0x0C) 16/32 位搬移与填充 | [bios](src/bios/bioslog.md) |
| 2026-08-15 | 11.5 | BIOS | 解压 BitUnPack(0x10)/LZ77(0x11)/RL(0x14)/Huffman(0x13) | [bios](src/bios/bioslog.md) |
| 2026-08-15 | 11.6 | BIOS | 等待 Halt(0x06)/IntrWait(0x04)/VBlankIntrWait(0x05)（PC 不动等价等待） | [bios](src/bios/bioslog.md) |
| 2026-08-15 | 11.7 | BIOS/测试 | 综合真码（LZ77 解压到 VRAM + Div + Sqrt 串行）；256 项检查 0 失败 | [bios](src/bios/bioslog.md) · [tests](tests/testlog.md) |
| 2026-08-15 | 11 完成 | 收尾 | 阶段 11 完成：BIOS HLE + SWI 分发（`src/bios/` 模块 + 除法/开方/搬移/解压/等待） | [bios](src/bios/bioslog.md) · [tests](tests/testlog.md) |
| 2026-08-15 | 12.1 | 预习/CPU | 新增 `docs/13-exceptions-modes.md`：特权模式/CPSR 模式位/异常向量表 | [13](docs/13-exceptions-modes.md) · [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 12.2 | CPU | 异常向量：`arm_exception` + `vector_base`（ARM9 高/ARM7 低）；未定义→0x04、未知 SWI→0x08 | [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 12.3 | CPU | CPSR 模式切换 + SPSR 分槽 `spsr[5]` 保存/恢复（MSR 切模式、`SUBS pc`/`LDM ^` 恢复）；修「写 PC 后又 +4」跳转 bug | [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 12.4 | CPU | CP15 c1 的 V 位联动 `vector_base`（cache/MMU 使能位先存不生效） | [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 12.5 | CPU | IRQ 真实响应：取指前查 pending 进 handler、`SUBS pc,lr,#4` 返回 | [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 12 完成 | 收尾 | 阶段 12 完成：CP15 + 异常向量 + 中断真实化；282 项检查 0 失败 | [cpu](src/cpu/cpulog.md) · [tests](tests/testlog.md) |
| 2026-08-15 | 13.1 | 预习/CPU | 新增 `docs/14-thumb.md`：Thumb 16 位编码 / T 位 / BX-BLX 切换 / PC 约定 | [14](docs/14-thumb.md) · [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 13.2 | CPU | Thumb 译码框架：`thumb.c` + `thumb_step` + `cpu_fetch16` + T 位分发 | [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 13.3 | CPU | Thumb 数据处理：移位/立即数/16 种 ALU（全更新标志） | [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 13.4 | CPU | Thumb 访存：字/字节/半字/SP 相对/寄存器偏移/字面量池 | [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 13.5 | CPU | Thumb 分支 B/BL/BX/BLX/条件分支 + PUSH/POP + STMIA/LDMIA | [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 13.6 | CPU | Thumb 杂项：高寄存器 ADD/CMP/MOV、取地址、SWI 进 HLE | [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 13.7 | CPU/测试 | Thumb 真码写 VRAM + 调 SWI；336 项检查 0 失败 | [cpu](src/cpu/cpulog.md) · [tests](tests/testlog.md) |
| 2026-08-15 | 13 完成 | 收尾 | 阶段 13 完成：Thumb 指令集（16 位译码 + 全指令语义 + T 位切换） | [cpu](src/cpu/cpulog.md) · [tests](tests/testlog.md) |
| 2026-08-15 | 14.1 | 预习/卡带装载 | 新增 `docs/15-secure-area.md`：安全区加密（KEY1/Blowfish + 子密钥派生 + encryObj） | [15](docs/15-secure-area.md) |
| 2026-08-15 | 14.2 | 卡带装载 | KEY1（Blowfish）实现：块加解密 + 三级密钥调度 + 密钥表常量 0x1048 字节 | [cart](src/cart/cartlog.md) |
| 2026-08-15 | 14.3 | 卡带装载 | 安全区解密 + 游戏子密钥派生 + `cart_decrypt_secure_area` | [cart](src/cart/cartlog.md) |
| 2026-08-15 | 14.4 | 卡带装载/主循环 | 装载时解密安全区后再拷 RAM（`main.c` 接入） | [cart](src/cart/cartlog.md) · [main](src/mainlog.md) |
| 2026-08-15 | 14.5 | 测试 | 自制含加密安全区 ROM 完整装载；360 项检查 0 失败 | [tests](tests/testlog.md) |
| 2026-08-15 | 14 完成 | 收尾 | 阶段 14 完成：商业 ROM 装载 + 安全区解密（KEY1/Blowfish） | [cart](src/cart/cartlog.md) · [tests](tests/testlog.md) |
| 2026-08-15 | 15.1 | 预习/卡带装载 | 新增 `docs/16-cartridge-protocol.md`：卡带命令协议（ROMCTRL/AUXSPICNT/CARD_COMMAND/CARD_DATA + B7/B8） | [16](docs/16-cartridge-protocol.md) · [cart](src/cart/cartlog.md) |
| 2026-08-15 | 15.2 | 卡带装载 | 新建 `cartbus`：ROMCTRL/命令解码 + CARD_DATA 数据端口（瞬时传输模型） | [cart](src/cart/cartlog.md) |
| 2026-08-15 | 15.3 | IO 寄存器 | DMA 补全：4 通道 + 地址控制（增/减/固定）+ repeat + 触发源（立即/VBlank/卡带） | [io](src/io/iolog.md) |
| 2026-08-15 | 15.4 | 卡带装载/IO/主循环 | io/bus/nds 接线 + `main.c` 挂载卡带 + 卡带 DMA 读路径 + 卡带 IRQ（IF bit19） | [cart](src/cart/cartlog.md) · [io](src/io/iolog.md) · [main](src/mainlog.md) |
| 2026-08-15 | 15.5 | 测试 | 综合：卡带命令读 + DMA 从 ROM 搬数据到 RAM（单元 + CPU 程序）；393 项检查 0 失败 | [tests](tests/testlog.md) |
| 2026-08-15 | 15 完成 | 收尾 | 阶段 15 完成：卡带协议 + DMA 补全（流式读卡带，真游戏运行时路径） | [cart](src/cart/cartlog.md) · [io](src/io/iolog.md) · [tests](tests/testlog.md) |
| 2026-08-15 | 16.1 | 预习/卡带装载 | 新增 `docs/17-save-memory.md`：存档类型（EEPROM/Flash/FRAM）+ AUXSPI 协议 | [17](docs/17-save-memory.md) · [cart](src/cart/cartlog.md) |
| 2026-08-15 | 16.2 | 卡带装载 | 新建 `save`：存档芯片 SPI 状态机（EEPROM/Flash 命令）+ cartbus 接 AUXSPI | [cart](src/cart/cartlog.md) · [io](src/io/iolog.md) |
| 2026-08-15 | 16.3 | 卡带装载/主循环 | 存档持久化：`save_load_file/save_save_file` + `main.c` 装载/写回 .sav | [cart](src/cart/cartlog.md) · [main](src/mainlog.md) |
| 2026-08-15 | 16.4 | 测试 | 综合：游戏经 AUXSPICNT/AUXSPIDATA 读写存档（单元 + CPU 程序）；423 项检查 0 失败 | [tests](tests/testlog.md) |
| 2026-08-15 | 16 完成 | 收尾 | 阶段 16 完成：存档（EEPROM/Flash + .sav 持久化，游戏能保存进度） | [cart](src/cart/cartlog.md) · [tests](tests/testlog.md) |
| 2026-08-15 | 17.1 | 预习/IO | 新增 `docs/18-touch-spi.md`：触摸屏 + 主 SPI 总线（SPICNT/SPIDATA + TSC 命令协议） | [18](docs/18-touch-spi.md) · [io](src/io/iolog.md) |
| 2026-08-15 | 17.2 | IO 寄存器 | 新建 `touch`：SPICNT/SPIDATA + TSC 命令状态机（12/8 位回传、X/Y/Z/电池通道）+ io 接线 | [io](src/io/iolog.md) |
| 2026-08-15 | 17.3 | 测试 | 综合：CPU 程序经 SPICNT/SPIDATA 读出 X/Y 坐标（单元 + 集成）；443 项检查 0 失败 | [tests](tests/testlog.md) |
| 2026-08-15 | 17 完成 | 收尾 | 阶段 17 完成：触摸 SPI（SPICNT/SPIDATA + TSC 坐标读取，游戏能读触控输入） | [io](src/io/iolog.md) · [tests](tests/testlog.md) |
| 2026-08-15 | 18.1 | 预习/音频 | 新增 `docs/19-audio.md`：16 通道音频（PCM8/PCM16/IMA-ADPCM/PSG）+ 寄存器/混音 | [19](docs/19-audio.md) · [snd](src/snd/sndlog.md) |
| 2026-08-15 | 18.2 | 音频 | 新建 `snd`：16 通道寄存器（SOUNDxCNT/SAD/TMR/PNT/LEN + SOUNDCNT/SOUNDBIAS）+ io 路由 | [snd](src/snd/sndlog.md) · [io](src/io/iolog.md) |
| 2026-08-15 | 18.3 | 音频 | 混音器：PCM8/PCM16/IMA-ADPCM/PSG 合成 + 音量/声相/主音量/偏置 + 单发/循环 | [snd](src/snd/sndlog.md) |
| 2026-08-15 | 18.4 | 音频/主循环 | SDL 音频回调 + `main.c` 接线 + demo 提示音（~256Hz 方波） | [snd](src/snd/sndlog.md) · [main](src/mainlog.md) |
| 2026-08-15 | 18.5 | 测试 | 综合：寄存器读写 + 混音样本 + CPU 程序配置通道；467 项检查 0 失败 | [tests](tests/testlog.md) |
| 2026-08-15 | 18 完成 | 收尾 | 阶段 18 完成：音频（16 通道 PCM/ADPCM/PSG 混音 + SDL 回调发声，游戏能出声音） | [snd](src/snd/sndlog.md) · [tests](tests/testlog.md) |
| 2026-08-15 | 19.1 | 预习/3D | 新增 `docs/20-3d.md`：3D 几何引擎（GE/RE、GXFIFO 命令、矩阵/顶点定点格式、图元） | [20](docs/20-3d.md) · [gx](src/gx/gxlog.md) |
| 2026-08-15 | 19.2 | 3D 几何 | 新建 `gx`：DISP3DCNT/GXSTAT/RAM_COUNT 寄存器 + GXFIFO/命令端口 32 位写 + 命令 FIFO 解码 | [gx](src/gx/gxlog.md) · [io](src/io/iolog.md) · [bus](src/bus/buslog.md) |
| 2026-08-15 | 19.3 | 3D 几何 | 几何命令：矩阵（LOAD/MULT/SCALE/TRANS/PUSH/POP/IDENTITY）+ 顶点提交 + 变换（pos×proj→透视除→视口） | [gx](src/gx/gxlog.md) |
| 2026-08-15 | 19.4 | 3D 几何/显示 | 三角形软件光栅化（半平面判定）+ render.c 合成 3D 图层进 2D 顶屏（DISP3DCNT 使能位） | [gx](src/gx/gxlog.md) · [ppu](src/ppu/ppulog.md) |
| 2026-08-15 | 19.5 | 测试 | 综合：顶点变换 + 光栅化 + GXFIFO 命令流出图 + 3D 图层合成；483 项检查 0 失败 | [tests](tests/testlog.md) |
| 2026-08-15 | 19 完成 | 收尾 | 阶段 19 完成：3D 几何引擎（GXFIFO 命令流 + 矩阵/顶点变换 + 软件光栅化 + 3D 图层合成） | [gx](src/gx/gxlog.md) · [tests](tests/testlog.md) |
| 2026-08-15 | 20.1 | 显示 | 仿射（旋转/缩放）背景：BGxPA-PD(1.7.8)/X-Y(1.19.8) 寄存器 + 逆仿射采样 + MODE1/2 出图 | [disp](src/io/iolog.md) · [ppu](src/ppu/ppulog.md) |
| 2026-08-15 | 20.2 | 显示/IO | 每像素合成器 + 颜色特效：Alpha 混合/增亮/减暗 + MASTER_BRIGHT + 显示捕获（顶屏→LCDC VRAM） | [ppu](src/ppu/ppulog.md) · [disp](src/io/iolog.md) · [bus](src/bus/buslog.md) |
| 2026-08-15 | 20.3 | 显示 | 窗口（WIN0/WIN1 限定图层显示区域）+ 更多混合效果；修 16 位寄存器高字节写入丢失 bug | [ppu](src/ppu/ppulog.md) · [disp](src/io/iolog.md) |
| 2026-08-15 | 20 完成 | 收尾 | 阶段 20 完成：2D PPU 补全（旋转/缩放 + 混合/亮度 + 窗口 + 显示捕获）；521 项检查 0 失败 | [ppu](src/ppu/ppulog.md) · [tests](tests/testlog.md) |
| 2026-08-15 | 21-A1 | 卡带装载/主循环 | 修 ARM7 镜像装载按目标区判大小（FFXII ARM7 载入 Main RAM 0x02380000，165KB） | [main](src/mainlog.md) |
| 2026-08-15 | 21-A2 | 诊断 | bring-up 诊断设施：`bus.diag` 开关 + 未知 SWI/未实现指令/未知 IO 只打一次 + `--headless N`/`--trace` 命令行 | [main](src/mainlog.md) · [bus](src/bus/buslog.md) · [io](src/io/iolog.md) · [bios](src/bios/bioslog.md) |
| 2026-08-15 | 21-A3 | CPU | 修 ARM r15 读缺 +8（`read_reg` 统一修正 PC 相对字面量池）；跑 FFXII 定位首个 gap：Shared WRAM 未映射 | [cpu](src/cpu/cpulog.md) |
| 2026-09-05 | 21-B0 | 预习/文档 | 新增 `docs/21-rom-bringup.md`：Phase A 总结 + Phase B 路线（真实 ROM 兼容性验收学习文档） | [21](docs/21-rom-bringup.md) |
| 2026-09-05 | 21-B1 | 内存总线 | Shared WRAM 映射：0x03000000 主区 + 0x037F8000 镜像区（32KB 同一物理内存）；ARM7 不再卡 0x037F8xxx | [bus](src/bus/buslog.md) |
| 2026-09-05 | 21-B2 | IO/IPC | IPCSYNC（0x04000180/81）同步寄存器：双核 out 交叉读写 + bit13 请求→对端 IF16 + bit14 门控；真 ROM 不再报未知 IO（544 项检查 0 失败） | [io](src/io/iolog.md) · [tests](tests/testlog.md) |
| 2026-09-05 | 21-B3 | 内存总线 | Main RAM 无缓存镜像 0x02400000-0x027FFFFF（4MB 同一物理别名）；真 ROM ARM9 不再弹 PC=0，稳定停在主存代码区（552 项检查 0 失败） | [bus](src/bus/buslog.md) · [tests](tests/testlog.md) |
| 2026-09-05 | 21-B4 | CPU | ARM BX 奇地址切换 Thumb（T 位按目标 LSB 置/清 + PC 清 LSB）；真 ROM ARM7 不再按 ARM 误译 Thumb 区（555 项检查 0 失败） | [cpu](src/cpu/cpulog.md) · [tests](tests/testlog.md) |
| 2026-09-05 | 21-B5 | CPU | Thumb BX/BLX 寄存器号解码修复（Rm 在 bit6:3，BX lr=0x4770 不再变成 BX r8）+ Thumb 逐条 trace；真 ROM IO 刷屏消失（557 项检查 0 失败） | [cpu](src/cpu/cpulog.md) · [tests](tests/testlog.md) |
| 2026-09-05 | 21-B6 | 内存总线 | Main RAM 无缓存镜像仅 ARM9 可见（ARM7 写访问落空）；修复 ARM7 块拷贝覆盖 ARM9 栈导致的弹 PC=0xE1C010B0（559 项检查 0 失败） | [bus](src/bus/buslog.md) · [cpu](src/cpu/cpulog.md) · [tests](tests/testlog.md) |
| 2026-09-05 | 21-B7 | IO 寄存器/内存总线 | EXMEMCNT/WRAMCNT 内存控制寄存器 + Shared WRAM 按 WRAMCNT 双核切分；真 ROM 3 条未知 IO 消除（587 项检查 0 失败），ARM7 轮询卡点排入 B8 | [io](src/io/iolog.md) · [bus](src/bus/buslog.md) · [tests](tests/testlog.md) |
| 2026-09-05 | 21-B8 | 内存总线/CPU/主循环 | 解码 0x027FFxxx 信箱约定；根因是 ARM9 DTCM/ITCM 未实现——新增 CP15 DTCM/固定 ITCM、镜像恢复双核别名、直接启动卡带表（594 项检查 0 失败），真 ROM 越过旧轮询进入 boot 初始化 | [bus](src/bus/buslog.md) · [cpu](src/cpu/cpulog.md) · [main](src/mainlog.md) · [tests](tests/testlog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-05 | 21-B9 | BIOS | SWI 0x0E GetCRC16（CRC-16/IBM）：新增 `bios_crc16.*`，ARM/Thumb/ARM7 单测（600 项检查 0 失败）；真 ROM unknown SWI 消失，ARM7 前进到 0x037FC89C | [bios](src/bios/bioslog.md) · [tests](tests/testlog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-05 | — | 工程管理 | 程序入口自动切控制台 UTF-8（`SetConsoleOutputCP(CP_UTF8)`），修复中文日志乱码 | [main](src/mainlog.md) · [tests](tests/testlog.md) |
