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

## 6.5 — cpu_step 接入定时器

- `cpu_step` 在 `cycles++` 后调 `io_advance_timers(cpu->nds->io)`：一条指令 ≈ 一个周期，
  推进所有使能定时器（分频逻辑在 io/timer.c 内）。cpu.c 增加对 io/io.h 的依赖。
- 定时器自测（阶段 6）：TM0 1:1 计 640、TM1 1:64 计 10、TM2 禁止计 0，全过。

## 8.2 — CPU 拆分出 ARM9/ARM7 功能文件 + is_arm7

- 沿用 cart 的「接口 + 功能文件」规则，`src/cpu/` 进一步拆分：
  - `cpu.h/.c`：对外接口——`arm_cpu_t` 状态（含新增 `is_arm7` 身份标志）、生命周期、`cpu_fetch`、`cpu_step`。
  - `arm9.h/.c`：ARM9 功能文件——`arm9_create(nds)` 内部 `cpu_create(nds, 0, 0)`。
  - `arm7.h/.c`：ARM7 功能文件——`arm7_create(nds)` 内部 `cpu_create(nds, 0, 1)`。
  - `exec.h/.c`：指令执行语义，两核共用（ARM9/ARM7 指令集差异极小，本阶段不区分）。
- `cpu_create` 签名加第 3 参 `is_arm7`；`cpu_step` 取指前 `nds->bus->active_is_arm7 = cpu->is_arm7`，
  让中断/FIFO 等按访问者身份分流。
- **怎么验证**：`test_dual_core_step`——两核各跑 NOP，step 后 PC 均 +4、`is_arm7` 正确；`test_interleave`
  交错 2:1 跑 6 步，ARM9 cycles=4、ARM7 cycles=2。

## 10.1 — 短文：为什么还需要这些指令

- 新增 `docs/11-arm-instructions-full.md`：真码会用到移位、MRS/MSR（读 CPSR 看模式/中断）、
  LDM/STM（PUSH/POP 进出栈）、乘法、字节/半字访存、SWI（进 BIOS）、SWP（同步原语）、MRC/MCR（CP15）。
- 这些是阶段 11 BIOS HLE、阶段 12 中断跳转、阶段 14 真 BIOS 跑起来的先决条件。

## 10.2 — 移位操作数 LSL/LSR/ASR/ROR

- 新增 `shift_apply()`：LSL/LSR/ASR/ROR/RRX，正确处理移位量（立即 0-31 / 寄存器低 8 位）、
  立即移位量 0 的语义（LSL#0 无移、LSR#0=0、ASR#0=符号扩展、ROR#0=RRX）与进位输出。
- `decode_op2()`：bit4=0 立即移位、bit4=1 寄存器移位（Rs 在 bit11-8）；I=1 走 `arm_rotate`。
- **踩坑**：初版把 bit4 含义搞反，导致 `ADD reg`/`SUB reg`/`AND`/`ORR`/`EOR` 全崩；查证后修正。

## 10.3 — 更多数据处理

- `exec_dataop` 补全 16 种 opcode：MVN/BIC/ADC/SBC/RSB/RSC（结果写 Rd）+ TST/TEQ/CMN（只更新标志）。
- ADC/SBC 把 C 计入；RSB/RSC 交换减数被减数；逻辑运算（AND/EOR/ORR/TST/TEQ/BIC/MVN）按 S 位只更新 N/Z。

## 10.4 — MRS/MSR

- `cpu.h` 增 `spsr`（阶段 12 扩为数组）、`swi_num`、`cp15[16]`。
- `msr_write()`：按 field 掩码写 CPSR/SPSR，模式位（低 5 位）受保护不可写。
- **踩坑**：MRS 掩码 `0x0FFFF0FF` 误把 Rd（bit15-12）掩掉，只匹配 Rd=0；改 `0x0FFF0FFF`。
  MSR 掩码改 `0x0FB0FFF0` 正确含 bit22（CPSR/SPSR 选择）。

## 10.5 — LDM/STM（PUSH/POP）

- `exec_block_transfer()`：IA/IB/DA/DB 四种寻址 + 写回；寄存器列表位图；PC 在列表时取 SPSR 恢复。
- PUSH/POP 是 STMDB/ LDMIA 别名，覆盖真码进出栈。

## 10.6 — 乘法

- `exec_mul`：MUL/MLA；`exec_mul_long`：UMULL/UMLAL/SMULL/SMLAL（64 位结果写 RdHi:RdLo）。
- **踩坑**：长乘 U-bit 与无符号/有符号语义相反——U=0 无符号、U=1 有符号，初版写反。

## 10.7 — 字节/半字访存

- `exec_single_transfer` 扩展 LDRB/STRB；新增 `exec_extra_transfer` 处理 LDRH/STRH/LDRSB/LDRSH。
- 支持前/后变址、立即/寄存器偏移；LDRSB/LDRSH 符号扩展，LDRB/LDRH 零扩展。

## 10.8 — SWI

- 记录 24 位编号到 `cpu->swi_num`，本阶段不跳异常向量（阶段 11 BIOS HLE 在 `cpu_step` 前拦截）。

## 10.9 — SWP/SWPB

- `exec_swp`：`temp=mem[Rn]; mem[Rn]=Rm; Rd=temp`，寄存器与内存原子交换。

## 10.10 — MRC/MCR（CP15 桩）

- `exec_coprocessor`：CRn 索引写入/读回 `cpu->cp15[]`，暂不含真实控制语义（阶段 14 真 BIOS 前补齐）。

## 10.11 — 综合：新指令真码

- 真码序列：PUSH 保存现场 → SWI 记录号 → MUL 乘法 → LDM/STM 搬数 → STRH 循环写 VRAM → POP 恢复。
- 204 项检查 0 失败。

## 12.1 — 短文：特权模式 / CPSR 模式位 / 异常向量表

- 新增 `docs/13-exceptions-modes.md`：7 种特权模式（M[4:0]）、CPSR 的 I/F/T 控制位、异常向量偏移表
  （复位 0x00 / 未定义 0x04 / SWI 0x08 / 取指中止 0x0C / 数据中止 0x10 / IRQ 0x18 / FIQ 0x1C）。

## 12.2 — 异常向量

- `exec.h` 增 CPSR I/F/T/模式位常量、`ARM_MODE_*`、`EXC_*_OFF` 向量偏移。
- `cpu.h` 的 `spsr` 扩为 `spsr[5]`（FIQ/IRQ/SVC/ABT/UND 各一份，User/System 无），增 `vector_base`。
- 新增 `arm_exception()`：SPSR_<模式>=CPSR → 切模式 → IRQ 关 I / FIQ 关 I+F → LR=PC+lr_adjust → PC=向量。
- `exec_step`：未实现指令不再「打印后继续」，改触发未定义异常（0x04）；未知 SWI 落入 SWI 异常（0x08）。
- `cpu_create`：`vector_base` = ARM9 高向量 `0xFFFF0000`、ARM7 低向量 `0x00000000`。

## 12.3 — CPSR 模式切换 + SPSR 保存/恢复

- `msr_write` 放开模式位保护，`MSR CPSR_c` 可切特权模式。
- `exec_spsr_index(mode)`：特权模式 → `spsr[5]` 下标；MRS/MSR SPSR 改为读写当前模式的 SPSR。
- `exec_dataop`：S=1 且 Rd=PC（`SUBS pc`/`MOVS pc`）时用当前模式 SPSR 恢复 CPSR（异常返回）。
- `exec_block_transfer`：`LDM ... ^`（S 且列表含 PC）加载 PC 后用 SPSR 恢复 CPSR。
- **修复**：`exec_step` 在 LDM/数据运算/LDR/LDRH 等写 PC 后仍 `r[15]+=4`，覆盖跳转地址；改为
  「写 PC（Rd=15 / 列表含 PC）则不再 +4」，异常返回与 `LDR pc,[sp],#4` 才正确。

## 12.4 — CP15 控制寄存器

- `exec_coprocessor`：MCR 写 c1 后按 bit13(V) 联动 `vector_base`（0=低向量 / 1=高向量）；
  cache/MMU 使能位只存储不生效（后续阶段用）。其余 CRn 仍按索引读写。

## 12.5 — IRQ 真实响应

- `cpu_step`：取指前检查本核 `irq_pending && !(cpsr & CPSR_I)`，满足则进 IRQ 异常（0x18），
  被打断指令地址经 LR 留给 handler；handler `SUBS pc, lr, #4` 返回并恢复 CPSR。
- 阶段 12 完成：282 项检查 0 失败。

