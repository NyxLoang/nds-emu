# tests 模块日志

> 统一测试入口：`tests/test_nds.c`。只链核心库 `ndscore`（不依赖 SDL/窗口），
> 由 `ctest --test-dir build` 收集，也可直接运行 `build/test_nds.exe`。

## 5.1 统一测试入口

- CMake 重构：抽 `ndscore` 静态库（nds/bus/cpu/exec/cart/arm9/arm7，均无 SDL 依赖）；
  `nds-emu`（+SDL 部分）与 `test_nds`（仅核心）都链它；`enable_testing()` + `add_test`。
- 迷你测试框架：`CHECK_EQ(name, got, want)` 宏 + 全局统计（g_checks/g_failures），
  退出码 0=全过 / 非 0=有失败；`run_program()` 通用驱动（装程序 → 跑 CPU → 到停机/上限）。
- 从 `main.c` 迁移已验证的自测：
  - `test_bus_rw`：8/16/32 位小端读写、字节序、VRAM 区间、IO 桩（原阶段 2）；
  - `test_arm_instructions`：22 条指令序列 + 位运算 + 写 VRAM 综合（原阶段 3b）；
  - `test_cpu_draw_vram`：顶屏黄十字 + 底屏纯绿（原阶段 4.5）。
- `main.c` 移除 `selftest_3b` 与断言工具，保留显示 demo（测试图 + CPU 十字/绿屏）作窗口内容。

## 5.2 用例：清屏

- 程序：整屏 256×192 填约定底色 `0x0000`（一次 STR 32 位写 2 像素，计数 24576 字循环）。
- 测试先把顶屏 framebuffer 全涂 `0xFFFF`，跑程序后逐像素断言全为 `0x0000`，
  证明清屏确实由 CPU 覆盖完成。98308 步跑完。

## 5.3 用例：画矩形

- 程序：先整屏清黑，再在左上象限 x∈[0,128)、y∈[0,96) 画红色实心矩形。
- **发现并修复 2 像素 bug**：STR 是 32 位写 = 2 个 RGB555 像素，若只把颜色放进低半字
  （`0x7C00`），则每 2 像素只有第 1 个着色——矩形/十字实际是竖条纹（6144=96×64 个奇数像素全黑）。
  修复：颜色构造为两个字半同值（`0x7C007C00`，MOV + ORR 两条指令）。
- 逐像素全屏扫描：矩形内全红、矩形外全黑，123272 步跑完。

## 修复：十字 demo 两缺陷（随 5 阶段验收发现）

窗口 demo 检查出十字不完整/粗细不一，根因在 `drawprog`（`main.c` 与
`test_cpu_draw_vram` 共用同一套程序）：

1. **竖线缺下半屏**：终点 `r4 = 起点 + 0xC000` 只到 y=95；改 `+ 0x18000` 到末行。
2. **横竖粗细不一**：横线 1 行 vs 竖线 2 列；横线改双行嵌套循环（y=95、y=96）。

`test_cpu_draw_vram` 新增断言覆盖：`(128,191)`、`(0,95)`、`(0,96)`、`(128,96)`、
`(128,97)`、`(255,95)`、十字外 `(10,10)/(10,190)`，全部通过（48 项检查 0 失败）。

## 5.4 用例：死循环保活

- 程序：`B self`（`0xEAFFFFFE`）。跑 10 万步断言不崩、PC 稳定停在原地、cycles=100000。

## 5.5 README：如何跑测试

- `README.md` 新增「运行测试」小节：配置/构建/`ctest`/直接运行 `test_nds.exe`、退出码含义、新增用例方法。

## 5.6 可选文档：未来 devkitARM（仅文档）

- 新增 `docs/06-devkitarm.md`：devkitARM 是什么、当前为何不需要、未来接入清单、
  与真机地址/阶段 9 的关系。只写文档不实现。

## 6.2 — 用例：中断寄存器 IME / IE / IF

- 写读回：IME/IE 32 位写读一致；`io_set_vblank` 后 IF bit3=1。
- **IF 写 1 清除**：写 `0xFFFFFFFF` 全清、只写 `0x8` 只清 VBlank 位（写 0 的位不受影响）。

## 6.3 / 6.4 — 用例：VBlank 标志 + 最小 IRQ 检测

- `IF == 0x0008` 即 VBlank 已置位。
- pending 四态：只挂起→0；+使能→0；+总开关→1；程序清 IF→0。验证「IF&IE&IME 缺一不可」。

## 6.5 — 用例：定时器 0-3

- `run_program` 型驱动（B self 死循环）跑 640 步：TM0(1:1)=640、TM1(1:64)=10、TM2(禁止)=0。
- 修 `timer.c` 字节偏移 bug（`% IO_TIMER_STRIDE`，见 iolog）。

## 6.6 — 用例：KEYINPUT

- 未按键读 `0xFFFF`（高 4 位恒 1）；按 A 读 `0xFFFE`；按 UP+B 读 `0xFFBD`（按下=0）。

## 6.7 — 用例：等 VBlank 轮询 + 读键程序

- 等 VBlank 程序两轮跑：不置 VBlank 停在轮询循环（PC 在 0x0C-0x18、不写屏）；置 VBlank 后走出循环、写黄屏停机（PC=0x28、VRAM[0]=0xFF00）。
- 读键程序：按 A 写蓝 `0x001F`、未按写红 `0x7C00`。
- 修 3 个手写汇编编码错误（ADD 立即数旋转、STR 寄存器位序、B offset），排查靠 `exec_set_trace(1)` 逐步打印（详见 iolog）。

## 7.2/7.3 — 用例：DMA0 寄存器 + 立即模式

- 寄存器写读回：SAD/DAD/CNT_L/CNT_H 4 项。
- 立即模式：
  - 字块拷贝：Main RAM 4 个字 → VRAM，逐字断言 + 自动清使能（CNT_H 读回只剩 32BIT 位）。
  - 半字填色：SRC_FIX 固定源，8 个像素全 `0x7C00`。

## 7.4 — 用例：CPU 程序触发 DMA 填 VRAM

- 机器码程序：配 SAD(颜色单元)/DAD(VRAM)/CNT(`0x81000080`=SRC_FIX|ENABLE+128) 触发，停机。
- 断言 128 像素全红 + 自动清使能。
- 修 `ADD r1,r1,#0x1000` 编码：`0xE2811410`（imm8=0x10,ROR8）实得 `0x10000000`，
  正确 `0xE2811A01`（imm8=0x01,ROR20→0x1000）。教训：立即数要用 `arm_rotate` 反推验证。

## 8.2/8.4 — 用例：双核 step + 交错调度

- 新增 `run_cpu7` 驱动（与 `run_program` 对称，操作 `nds->cpu7`）。
- `test_dual_core_step`：两核各跑 NOP，step 后 PC 均 +4、`is_arm7` 正确。
  （初版用 `B self` 断言 PC 前进，实际 B self 死循环 PC 原地打转，改 NOP。）
- `test_interleave`：`i%3==2` 跑 ARM7 的交错 6 步，ARM9 cycles=4、ARM7 cycles=2。

## 8.3 — 用例：ARM7 WRAM + 入口取指

- `test_arm7_wram`：`0x03800000` 写读回、越界（`0x03900000`）读 0。
- `test_arm7_fetch`：WRAM 写 `MOV r0,#5`，`cpu_reset(cpu7, 0x03800000)` 后 step，r0=5、PC+4。

## 8.5 — 用例：中断按 CPU 分流

- `test_irq_split`：手动切 `bus->active_is_arm7`，ARM9 写 IME/IE 后 ARM7 读得独立清零值、
  ARM9 读回自己的值，验证 `irq[2]` 分流。

## 8.6 — 用例：IPC FIFO 收发/状态位/中断

- `test_fifo_basic`：使能两核 FIFO 后，ARM9 发 `0xDEADBEEF` → ARM7 收得原值（反向 `0x11223344` 同理）；
  空队列 send/recv empty 位、发送后 send 非空、对方 recv 非空。
- `test_fifo_irq`：使能 send-empty IRQ → IF17 置位；使能 recv-not-empty IRQ 后由 ARM7 发一字 →
  ARM9 的 IF18 置位（边沿触发）。

## 8.7 — 用例：双核 FIFO 传值 + 底屏体现

- ARM7 程序（WRAM）：发 `0x7C00`（红）到 SEND；ARM9 程序（Main RAM）：读 RECV 后写底屏 VRAM。
- 两程序都用「数据区预写地址 + LDR 加载」避免复杂立即数（SEND/RECV 地址不易用 MOV 立即数表示）。
- 先跑 ARM7 发送、再跑 ARM9 接收写屏；断言两核停机 PC 与底屏首像素 `0x7C00`。

## 全量验证

`ctest --test-dir build` 与直接运行均通过。
阶段 6 收尾时 70 项检查 0 失败；阶段 7 收尾时 90 项检查 0 失败；阶段 8 收尾时 **117 项检查 0 失败**；阶段 9 收尾时 **148 项检查 0 失败**。

## 9.2 — 用例：显示控制寄存器 + 调色板 RAM

- `test_disp_regs`：主/副 DISPCNT（32 位）、BGxCNT（16 位）、滚动 HOFS/VOFS 写读回；
  调色板 RAM 主 `0x05000000`/副 `0x05000400`；副 BG VRAM 窗口 `0x06200000` ↔ 物理 `vram[0x40000]`。

## 9.3 — 用例：直色位图渲染

- `test_bitmap_render`：mode 5 + BG2 直色位图，写红/蓝两像素，断言 RGB888 输出与未写区黑。

## 9.4 — 用例：Mode 0 tile 图层

- `test_tile_render`：16 色 tile 像素(0,0)=索引1 → 红，其余透明露绿背景；切 256 色同理。

## 9.5 — 用例：副引擎（Engine B）对称渲染

- `test_engine_b`：副引擎直色位图（`0x06200000`）+ tile 渲染各验一次（副调色板 `0x05000400`）。
- **踩坑（测试）**：前一步位图数据残留在 `0x06200000` 会污染 tile 位面，tile 用例前需显式清
  位面 1-3，只留位面 0 的 `0x80`。

## 9.6 — 用例：OBJ 最小（一个 sprite）

- `test_obj_render`：OAM[0] 配 8×8 sprite，16 色在 (20,10) 显示红、透明/屏外露背景；切 256 色显示绿。
- **踩坑（测试）**：OAM 默认全 0 = 原点可见 sprite，需先对 128 条 OAM 统一置 bit9 禁用再配 entry 0。

## 9.7 — 用例：自造数据 2D 场景验收

- `test_2d_scene`：顶屏 8bpp tile+tilemap+调色板 拼「红 tile0 + 绿 tile1 + 透明底」+ OBJ 蓝方块；
  底屏副引擎 8bpp tile 纯青。整条链路只经 DISPCNT/BGxCNT/OAM 寄存器，无线性 FB 写入。
