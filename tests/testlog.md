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

## 全量验证

`ctest --test-dir build` 与直接运行均 40 项检查 0 失败。
阶段 6 收尾时共 70 项检查 0 失败。
