# io 模块日志

> 覆盖：IO 寄存器区（`0x04000000` 起）的真实寄存器语义。
> 按模块结构规则拆功能文件：`irq.h/.c`（中断）、`timer.h/.c`（定时器）、`key.h/.c`（按键），
> 对外接口 `io.h/.c` 负责按地址分发；bus 在 IO 区间转发到 `io_read8/io_write8`。

## 6.2 — 模块创建 + bus 路由

- **做了什么**：
  - `io_t` 聚合三个功能单元：`irq_t irq` + `nds_timer_t timer[4]` + `keypad_t keypad`。
  - `io_read8/io_write8` 按地址区间分发：IRQ 区 → `irq.c`、定时器区 → `timer.c`、按键区 → `key.c`；未实现寄存器读 0 写忽略（沿用阶段 2 桩语义）。
  - `bus_t` 去掉 `io[64KB]` 桩数组，改持 `io_t *io` 指针；`bus_read8/write8` 对 IO 区间转发给 io 模块。`nds_create` 创建 io 并 `bus->io = nds->io` 建立联系，逆序销毁。
  - `CMakeLists.txt` 的 `ndscore` 加入 `io.c/irq.c/timer.c/key.c`。
- **IRQ 寄存器实现**（`0x04000208/10/14`）：
  - IME/IE/IF 按 32 位寄存器存储、按字节读写；**IF 写 1 清除**（`ifl &= ~val`）。
  - `irq_pending()`：`(IF & IE) != 0 && IME bit0`，三者缺一不可。
- **怎么验证**：`test_irq_regs`——IME/IE 写读回、IF 置 VBlank 位、IF 写 `0xFFFFFFFF` 全清、只清 VBlank 位，5 项断言全过。

## 6.3 — 指令计数产生 VBlank（IF bit3）

- `io_set_vblank()` → `irq_set_vblank()` 把 `IF |= 0x0008`。
- 主循环每帧跑完 `steps_per_frame` 步后调用（真机是显示硬件自动置位，这里用指令计数近似）。
- **怎么验证**：`test_vblank_flag` 断言 `IF == 0x0008`；`test_wait_vblank` 程序轮询 IF 后能走出循环。

## 6.4 — 最小 IRQ 检测（不进异常向量）

- 只检测不跳转：`io_irq_pending()` 返回「是否真会发生中断」。主循环首次检测到打印一次 `irq: IRQ pending (VBlank ...)`。
- 真正跳 `0x18` 异常向量留到阶段 10+（计划 6.4 明示「可先只置位」）。
- **怎么验证**：`test_irq_pending` 四种组合（只 IF / IF+IE / 三者齐 / 清 IF）断言 pending 0→0→1→0。

## 6.5 — Timers 0-3 启停 / 分频 / 计数

- 每个定时器 4 字节：`TMxCNT_L`（base+0/1，计数值）、`TMxCNT_H`（base+2/3，控制）。
- 控制位：bit7=使能、bit0-1=分频（1/64/256/1024）；**写 CNT_H 重启计数器**（cnt_l/acc 清零）。
- `cpu_step` 每条指令调 `io_advance_timers()`（一条指令 ≈ 一个周期）；`timer_advance` 用内部累加器 `acc` 实现分频：攒够分频数才给 cnt_l +1，16 位自然回绕。
- **踩坑修复**：`timer_read8/write8` 原本用「全局偏移」（`addr - 0x04000100`）算字节位移，TM1 起偏移 ≥6 导致 `<<32` 未定义行为、控制字节写不进。改为 `% IO_TIMER_STRIDE` 取相对本定时器的字节偏移。
- **怎么验证**：`test_timers` 跑 640 步——TM0（1:1）计 640、TM1（1:64）计 10、TM2（禁止）计 0。

## 6.6 — KEYINPUT + SDL 键映射

- `KEYINPUT`（`0x04000130`，16 位只读）：低 12 位是按键位，**按下=0**，高 4 位恒 1。
- `io_set_keyinput(pressed)` 内部取反：`input = (~pressed & 0x0FFF) | 0xF000`；写忽略。
- `main.c` 事件循环捕获 `SDL_KEYDOWN/UP`，把 SDL 键码映射成 NDS 键位（`z/x/s/d/a/f/回车/退格/方向键`），维护 pressed 掩码后 `io_set_keyinput`。窗口模块保持不解耦机器（映射放组合根 main.c）。
- **怎么验证**：`test_keyinput` 三种按键组合读回；`test_key_program` 程序读 KEYINPUT 按 A 显示蓝、未按显示红。

## 6.7 — 等 VBlank 轮询 + 读键程序

- **等 VBlank 程序**（手写机器码）：
  `MOV r0,#0x04000000 → ADD +0x200 → ADD +0x14` 得 IF 地址 → `LDR r1,[r0]` → `ANDS r2,r1,#8` → `BNE` 跳出 → 写 VRAM 黄色 → `B self`。
  测试两轮：不置 VBlank 跑 64 步停在轮询循环（PC 在 0x0C-0x18、不写屏）；`io_set_vblank` 后从头跑走出循环、写屏停机（PC=0x28、VRAM[0]=0xFF00）。
- **读键程序**：读 KEYINPUT → `ANDS r3,r1,r2`（r2=1 即 A 键）→ 按下写蓝 `0x001F`、未按写红 `0x7C00`。
- **踩坑（手写汇编编码错误，都是「字节位序」问题）**：
  1. `ADD r0,r0,#0x200` 写成 `0xE2800802`（rot4=8→ROR16→加了 `0x00020000`），正确 `0xE2800C02`（rot4=12→ROR24）。
  2. `STR r4,[r3]` 两度写错：`0xE5844000`（Rn=4）与 `0xE5843000`（Rn=4、Rd=3），正确 `0xE5834000`（Rn 在字节1低半字节、Rd 在字节2高半字节）。
  3. `B 0x28` 写成 offset 1（跳到 0x2C），正确 offset 0。
  排查方法：`exec_set_trace(1)` 逐步打印 `STR r3, [r4]` 暴露真实解码结果。
- **怎么验证**：两个用例全过，阶段 6 累计 70 项检查 0 失败（ctest 通过）。
