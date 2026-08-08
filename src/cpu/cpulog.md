# cpu 模块日志

## 重构：指令执行拆分为 exec 功能文件

- 遵循模块结构规则，`src/cpu/` 拆为：
  - `cpu.h/.c`：对外接口——`arm_cpu_t` 状态、生命周期、`cpu_fetch`、`cpu_step`（取指 + 计数，委托 exec）。
  - `exec.h/.c`：功能文件——指令执行语义（条件码、标志更新、`arm_rotate`、数据运算、B/BL/BX、LDR/STR、未实现打印）。
- `cpu_step` 简化为三行：取指 → cycles++ → `exec_step(cpu, insn)`。
- 行为零变化：3b 自测 12 项断言 + flags 全部 PASS，死循环 PC 稳定。

## 3a.2 — arm_cpu_t

- 新建 `src/cpu/cpu.h` / `cpu.c`（模块目录结构见 `docs/01`）。
- `arm_cpu_t`：`nds`（所属机器）、`r[16]`（r15 即 PC）、`cpsr`、`cycles`。
- `cpu_create(nds, reset_pc)` / `cpu_destroy` / `cpu_reset(reset_pc)`。
- `nds_t` 前向声明 `arm_cpu_t` 并挂入 `nds->cpu`；`nds_create` 中创建。

## 3a.3 — cpu_fetch

- `cpu_fetch()` 按 `r[15]` 从 bus 读 32 位指令字（复用阶段 2 的 `bus_read32`）。

## 3a.4 / 3a.5 — cpu_step + 死循环识别

- `cpu_step()`：取指 → 译码高 4 位条件码（`insn>>28`）与 3 位大类（`insn>>25 & 7`）。
- 识别「无条件 B（AL + 101 大类）且立即数 = -1」：目标 = `PC+8-8 = PC`，原地打转。
- `deadloop_reported` 标志：死循环只打印一次，避免主循环刷屏。

## 3a.6 — 未实现指令

- 其它指令：打印机器码与 PC，PC += 4 继续（阶段 3b 逐类实现）。

## 3a.7 — 主循环接入

- 装载镜像后 `cpu_reset(nds->cpu, hdr.arm9.entry)`。
- 主循环每帧执行固定 8 步，每 60 帧打印 PC/cycles。

## 修复：mini.nds 布局错误

- 假 ROM 原设 `ram=0x02000000` 但 `entry=0x02000800`，入口落在镜像外（读 0）。
- `make_fake_rom.py` 改为 `ram = entry = 0x02000800`，重新生成 `mini.nds`；
  取指读回 `EAFFFFFE`（死循环指令），PC 稳定停在入口。

## 3b.1 — 条件码执行框架

- `cond_ok()`：按 CPSR 的 N/Z/C/V 判断 15 种条件（EQ/NE/CS/CC/MI/PL/VS/VC/HI/LS/GE/LT/GT/LE/AL）。
- `cpu_step` 开头检查条件，不满足则跳过指令（无副作用，仅 PC+4）。

## 3b.2 — MOV 立即数

- `arm_rotate()`：ARM 立即数 = `imm8 ROR (rot4*2)`（经典 8 位立即数旋转编码）。
- `decode_op2()`：I=1 立即数 / I=0 寄存器。
- `exec_dataop()`：opcode=1101(MOV) 写入 `Rd`。

## 3b.3 — ADD / SUB

- opcode=0100(ADD)、0010(SUB)；支持立即数与寄存器操作数。
- 更新标志：`set_carry_add`（C=进位，V=同号相加异号）、`set_carry_sub`（C=无借位）。

## 3b.4 — CMP + flags

- opcode=1010：`Rn - op2` 只更新 N/Z/C/V，不写寄存器（后接条件分支的基础）。

## 3b.5 / 3b.6 — LDR / STR 立即偏移

- bit27-26=01 大类：P=1 前变址、U=±偏移、L=读/写。
- 地址 = `Rn ± offset12`，`bus_read32` / `bus_write32` 访存。

## 3b.7 — B 相对跳转

- 分支目标 = `PC+8 + 符号扩展(offset24<<2)`（ARM 流水线 PC+8 语义）。
- 「B 自己」死循环识别保留，只打印一次。

## 3b.8 — BL + BX lr

- BL：`lr = PC+4`（返回地址），再跳转；BX Rm：`PC = Rm`（用于 `BX lr` 返回）。
- 自测：`BL 0x70` → 子程序 `MOV r10,#0x33` → `BX lr` → 返回执行 `MOV r9,#0x22`。

## 修复：分支立即数符号扩展

- 3b 重写时误写成 `(int32_t)((imm24 << 8) >> 8)`：cast 在移位之后，无符号移位成
  逻辑右移，`0xFFFFFE` 被当成 +16777214 而非 -2，死循环跳到 `0x06000800`。
- 改为 `((int32_t)(imm24 << 8) >> 8) << 2`：先转 int32 再算术右移，符号扩展正确。

## 3b 自测（main.c `selftest_3b`）

- 手工汇编 22 条指令写入 Main RAM（0x02000000 起），逐条执行后断言寄存器。
- 覆盖：MOV/ADD/SUB/CMP、STR/LDR 立即偏移、B 跳转、BL/BX lr 调用返回。
- 12 项断言 + flags 全部 PASS；阶段 5 换正式单元测试。

## 3b.9 — AND / ORR / EOR 位运算

- `exec_dataop` 增加 opcode 0x0(AND)、0x1(EOR)、0xC(ORR)，支持立即数/寄存器操作数。
- 逻辑运算按 S 位更新 N/Z（不更新 C/V，与真机一致）。
- 自测：`0x0F & 0x33 = 0x03`、`0x0F | 0x33 = 0x3F`、`0x0F ^ 0x33 = 0x3C` 全部 PASS。

## 3b.10 — 综合：机器码写 VRAM

- 纯机器码程序（MOV r0,#0x06000000 → MOV r1,#0x7C00 → STR r1,[r0] → LDR r2,[r0]）
  把 RGB555 红色 `0x7C00` 写进 VRAM 基址，再读回。
- 自测：`r2 = 0x7C00`、`bus_read32(VRAM) = 0x7C00` 全部 PASS。
- 意义：模拟 CPU 已能直接驱动显存，为阶段 4 出图铺路。
