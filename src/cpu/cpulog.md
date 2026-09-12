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

## 13.1 — 短文：Thumb 16 位编码 / T 位 / BX-BLX

- 新增 `docs/14-thumb.md`：为什么游戏主体是 Thumb（省 ROM）、CPSR.T 位、取指差异（16 位 vs 32 位）、
  Thumb 的 PC 约定（读 PC = 当前指令地址 + 4，PC 相对寻址需字对齐 `(pc&~3)`）、
  按高 5 位分的 18 组指令、BX/BLX 与 T 位联动。
- 新增 `thumb.h`：声明 `thumb_step`（16 位指令执行入口）与 `thumb_set_trace`。

## 13.2 — Thumb 译码框架

- 新增 `thumb.c`：`thumb_step(cpu, insn16)` 按高 5/6 位分派 18 组指令。
- `cpu_step` 按 `cpsr & CPSR_T` 分发：T=1 走 `cpu_fetch16`（`bus_read16`）+ `thumb_step`，T=0 走原 ARM 路径。
- `cond_ok` 从 exec.c 的 static 提升为导出（`exec.h` 声明），供 Thumb 条件分支复用。
- PC 约定：`thumb_step` 内令 `pc = r[15] + 4`、`pc_word = pc & ~3`，所有 PC 相对寻址/分支基于此。

## 13.3 — Thumb 数据处理

- 格式 1/2（移位立即数、ADD/SUB 寄存器或 #imm3）、格式 3（MOV/CMP/ADD/SUB #imm8）、
  格式 4（16 种 ALU：AND/EOR/LSL/LSR/ASR/ADC/SBC/ROR/TST/NEG/CMP/CMN/ORR/MUL/BIC/MVN，全更新标志）。
- 移位带进位（LSL#0 保持 C、LSR#0=#32、ASR#0=#32、ROR#0 无操作）。

## 13.4 — Thumb 访存

- 格式 6（LDR 字面量池 [PC,#imm8*4]）、格式 7（寄存器偏移 STR/LDR/STRB/LDRB/STRH/LDRH/LDSB/LDSH）、
  格式 8（立即偏移字/字节 #imm5*4/#imm5）、格式 9（半字 #imm5*2）、格式 10（SP 相对 #imm8*4）。

## 13.5 — Thumb 分支 + 块操作

- 格式 5 的 BX/BLX（T=Rm[0]，PC=Rm&~1，BLX 先 lr=pc|1）；格式 15 条件分支（11011111=SWI）；
  格式 17 无条件 B（imm11）；格式 18 BL/BLX（两半字拼 22 位，第一半字 LR=pc+SE(off11)<<12，
  第二半字 PC=LR+(off11<<1)，LR=pc|1，BLX 再清 T）。
- 格式 13 PUSH/POP（STMDB sp!/LDMIA sp!，含 LR/PC 位）、格式 14 STMIA/LDMIA（写回）。

## 13.6 — Thumb 杂项 + SWI 进 HLE

- 格式 5 高寄存器 ADD/CMP/MOV（Rd=bit7:bits2-0，Rm=bit6:bits5-3，可含 r8-r15）。
- 格式 11 ADD Rd, PC/SP（取地址）、格式 12 ADD/SUB SP, #imm7*4。
- SWI：`fn = imm8` 直接 `bios_dispatch(fn)`（与 ARM 的 `swi_num>>16` 不同），已知号处理、未知号落 SWI 异常。
- **踩坑**：① BL 第一半字偏移应 `SE(off11)<<12`，即 `((int32_t)(x<<21))>>9`（初写 >>20 变成 <<1）；
  ② BX 的 Rm 是 `bit6:bits2-0`（bits5-3 恒 0），初版误用 `bits5-3` 作低位，与 ADD/CMP/MOV 的 Rs 布局混了。

## 13.7 — 综合：Thumb 真码写 VRAM + 调 SWI

- 真码序列：LDR 字面量取 VRAM 基址 → MOV #0x1F → STRH 写 VRAM[0] → MOV 准备被除数/除数 →
  SWI #0x09 Div → B . 保活；验证 VRAM 像素 0x001F + Div 商/余数/|商|。
- 阶段 13 完成：336 项检查 0 失败。

## 2026-09-05 · 21-B4 — ARM BX 奇地址切 Thumb

- **做了什么**：`exec.c` 的 ARM BX（0x012FFF1x）原来只写 `r15 = r[rm]`，不更新 T 位。
  真 ROM ARM7 `BX r12 -> 0x038043C9` 后仍按 ARM 模式把 Thumb 区数据当 32 位指令译码，
  最终 `B 0x049E07B5` 跑进零区。改为：目标 LSB=1 置 `CPSR_T`、LSB=0 清 `CPSR_T`，
  PC 写 `r[rm] & ~1`，与 Thumb 侧 BX 行为一致；补中文注释说明 ARM↔Thumb 切换规则。
- **怎么验证**：新增 `test_arm_bx_thumb`——Thumb 区放 `MOVS r1,#5` + 自循环，ARM 驱动
  载入 0x02001000、ORR #1 后 BX；断言 T=1、r1=5、PC=0x02001002。全量
  **555 项检查 0 失败**。
- **真 ROM 效果**：ARM7 不再走“把数据当 ARM 码”的旧跑飞路径（旧终点 0x0626D1xx 消失），
  能正确进入 0x038043C8 Thumb 区；但随后在 Thumb 段内跳到 IO 区 0x04000182 附近，
  headless 出现约 65k 行 `io: read unknown addr=0400xxxx (arm7=1)` 逐字节刷屏，
  20M 步终点 ARM7 PC=0x0178CBD8。此二次跑飞根因留待 B5：需要先补 Thumb 逐条 trace
  才能定位具体跳转来源。
- **结果**：✅ 用户验收通过（2026-09-05）。

## 2026-09-05 · 21-B5 — Thumb BX/BLX 寄存器号解码修复 + Thumb 逐条 trace

- **做了什么**：
  - `thumb.c` 给每条 Thumb 指令加逐条 trace（`thumb: PC=... insn=...`），用于真 ROM
    bring-up 定位 Thumb 段内的跳转错误（此前普通 Thumb 指令不打印，跑飞前无迹可寻）。
  - trace 抓到真 ROM 二次跑飞根因：ARM7 在 0x038043C8 执行 `SWI 3` + `BX lr`
    （0x4770），旧解码把 BX/BLX 的寄存器号按 bit2:0+bit6 拼（0x4770 → r8），
    实际 Thumb 编码是寄存器号在 bit6:3（0x4770 → r14=lr）；于是跳到 r8 里的
    0x04000180，开始逐字读 IO 区。改为 `rm = (insn >> 3) & 0xF` 并修正注释。
  - 测试：把阶段 13.5 里不符合硬件编码的 `BX r1` 测试指令 0x4701 改为规范编码
    0x4708（旧写法只是凑合旧解码器）；新增 `BX lr`（0x4770）回归用例。
- **怎么验证**：`test_nds.exe` **557 项检查 0 失败**；真 ROM `--headless 20000000`：
  IO 区逐字刷屏消失（unknown io = 0），ARM7 能正确进出 SWI3/BX-lr 桩并停在
  0x037FC0xx 的轮询循环；ARM9 走出旧等待循环后出现新卡点：约 cycle 463,554 在
  0x0200B9B0 `LDMFD sp!, {r4,r5,lr}` 后 `BX r14 -> 0xE1C010B0`——被弹回的 LR 已被
  污染，留待 B6 定位栈污染来源。

## 2026-09-05 · 21-B6 — LDM/STM trace 增强（协助定位栈污染）

- `exec.c` 的 LDM/STM 逐条 trace 增加 `base`（rn=13 时 SP 变化前）与 `pc_new`
  （LDM 含 PC 时弹出的目标），便于 bring-up 阶段追“返回地址被污染”问题。
- 用它确认 B6 污染点：ARM9 LR 槽 0x027E3B34 在 ARM7 块拷贝时被写入 0xE1C010B0；
  修复（镜像仅 ARM9 可见）后 `BX r14` 正常返回，见 buslog 21-B6。

## 2026-09-05 · 21-B8 — CP15 c9 子寄存器 + ARM9 DTCM 映射

- FFXII 复位例程 `0x02000A6C` 的 CP15 配置不止 c1：
  - `MCR p15,0,rX,c9,c1,0`：DTCM 配置（0x027E000A → 基址 0x027E0000、16KB）；
  - `MCR p15,0,rX,c9,c1,1`：ITCM 配置（先存储）；
  - c1 bit16 = DTCM 使能（复位在 0x02000B1C 写 0x0005707D 打开）。
- `exec_coprocessor` 现在按 crm/op2 读 c9 子寄存器；写 c9,c1,0 或 c1 后调用
  `exec_arm9_dtcm_update` 同步 bus 的 DTCM 窗口（大小编码
  `0x200 << ((val>>1)&0x1F)`，最小 4KB、封顶 16KB）。
- ARM9 ITCM 固定 0x01FF8000，由 bus 直接映射（见 buslog 21-B8），本步先不按
  c9,c1,1 动态关开。
- **验证**：21-B8 用例覆盖 ARM9 写 DTCM 不落主存、ARM7 同址写主存不碰 DTCM；
  `test_nds.exe` 594 项 0 失败。

## 2026-09-05 · 21-B9b — 首中断现场快照（确认 FFXII 的 ARM9 IRQ 槽协议）

- **做了什么**：
  - `cpu.c`：IRQ 真正触发时（诊断模式开）只打印一次“首中断现场”：哪颗核、被打断
    PC、CPSR、IME/IE/IF，以及 IRQ 约定槽末 8 字节——ARM9 取 DTCM+0x3FF8/FC，
    ARM7 取 0x03FFFFF8/FC。ARM9 槽基址从 bus 的 DTCM 配置读取，未使能时回退
    FFXII 已知的 0x027E0000，避免写死地址。
  - `exec.c`：`arm_exception` 的逐条 trace 加 `arm9`/`arm7` 前缀，中断后属于哪颗核
    的执行流不再靠猜。
- **怎么验证**：全量 **600 项检查 0 失败**（纯诊断，无新指令语义）。
- **真 ROM 证据**：FFXII ARM9 第一次 IRQ 发生在 cycles=699052（ARM9 PC=0x02009EC0）：
  `ie=0x00042009`（VBlank bit3 + FIFO bit17/18）、`ifl=0x00000008`，
  **槽 +0x3FFC = 0x01FF8000**——游戏确实把用户 IRQ handler 指针装进
  “DTCM 末 4 字节”约定槽，指向 ITCM 开头（0x01FF8000，复位时拷入的系统例程）。
  当前模拟器把 IRQ 直接跳到高向量 0xFFFF0018 读 0 跑飞，根因确认：
  下一步 B9c 让 IRQ 入口按槽取 handler 指针跳转（模拟 BIOS 高向量跳板）。

## 2026-09-05 · 21-B9c — ARM9 IRQ 槽跳板（模拟 BIOS 高向量分发）

- **做了什么**：`cpu.c` 在 IRQ 异常现场建好后，若 ARM9 的 DTCM 已使能且槽
  `DTCM+0x3FFC` 非 0，就把 PC 改成槽里的用户 handler（目标 LSB=1 置 Thumb），
  等价于真 BIOS 在 0xFFFF0018 做的事；槽为 0/未配置时保留旧向量路径便于诊断。
  ARM7 尚无 IRQ 触发证据，未按 0x03FFFFFC 槽对称实现（留待真机出现再补）。
- **怎么验证**：新增 `[case 21-B9c]`：DTCM 槽指向 ITCM 0x01FF8000，handler 放
  `SUBS pc, lr, #4`；触发 IRQ 后断言 PC=handler、IRQ 模式/I 位/SPSR/LR 正确，
  再走一步验证返回原 PC 并恢复 CPSR。全量 **608 项检查 0 失败**。
- **真 ROM 效果**：FFXII ARM9 不再跳 0xFFFF0018 读 0 漂到 0x000117C0，而是进入
  ITCM 的 IRQ 分发器（0x01FF8000），随后停在 0x01FF8028。对 ITCM 段做反汇编：
  这是 FFXII 的中断分发循环——`CLZ r0,r1`（数最高 pending 位）→ `BICS` 逐位
  清除并分发；模拟器未实现 CLZ（ARMv5），把它误译成别的数据运算，循环永不退出，
  B9d 修。

## 2026-09-05 · 21-B9d — CLZ 指令（前导零计数）

- **做了什么**：`exec.c` 实现 ARMv5 `CLZ Rd, Rm`（模式掩码
  `(insn & 0x0FFFFFF0) == 0x016F0F10`）：返回 Rm 二进制前导零个数，0 定义为
  32。放在 MRS/数据运算分支之前，避免被误译成数据运算。
- **怎么验证**：新增 `[case 21-B9d]`：单步执行 `CLZ r0,r1`，覆盖 0→32、
  0x80000000→0、0x00000001→31、FFXII 的 IE 值 0x00042009→13、VBlank bit3→28，
  每次断言 PC 前进。全量 **618 项检查 0 失败**。
- **真 ROM 效果**：FFXII 的 IRQ 分发循环（0x01FF8028-8030）不再死等——CLZ 找到
  最高 pending 位后由 `BICS` 逐位清除，逐个分发完 VBlank 后返回主程序；ARM9
  终点从 0x01FF8028 前进到 0x02006488。新卡点是 `BLX r1`（ARM 态寄存器间接
  调用，模拟器只实现了 BX），排入 B9e。

## 2026-09-05 · 21-B9e — ARM BLX Rm（寄存器间接调用）

- **做了什么**：`exec.c` 在 BX 分支后补 `BLX Rm`（模式掩码
  `(insn & 0x0FFFFFF0) == 0x012FFF30`）：先保存返回地址 `r14 = PC+4`，再按
  目标 LSB 置/清 T、PC 写目标并清最低位——与 BX 同规则，区别仅在写 LR。
- **怎么验证**：新增 `[case 21-B9e]`：单步执行 `BLX r1`，覆盖 ARM→Thumb
  （0x02001001，执行目标 MOVS 后 PC 前进 2）与 ARM→ARM（0x02001100，执行
  MOV r0,#42 后 PC 前进 4）两条路径，含 LR/T 断言共 10 项检查。全量
  **628 项检查 0 失败**。
- **真 ROM 效果**：ARM9 越过 0x02006488 的回调分发函数，回到 0x02009EC0
  的主等待循环（PC 在 EC0-ECC 小幅游走，等下一次 VBlank）。headless 8M 步
  内不再出现固定单点卡死；后续要确认第二次及以后的 VBlank IRQ 是否照常触发、
  启动流程是否会随帧数继续推进。

## 2026-09-05 · 21-B9f — IRQ HLE 桩：被中断现场保存/恢复

- **做了什么**：
  - `cpu.h/.c`：IRQ 经槽跳用户 handler 前把被中断现场的 r0-r3、r12、CPSR、
    返回点（被打断 PC）存进模拟器私有槽；handler 弹栈返回（FFXII 分发器是
    `stmdb sp!,{lr}; ...; ldmfd sp!,{pc}` 风格）后，`cpu_step` 在取指前恢复
    现场并重执行被打断指令——等价真 BIOS 桩“保存 r0-r3/r12 → 调 handler →
    恢复 → 返回”，但没有可执行 BIOS 代码。
  - 修复 `arm_exception` trace 参数顺序 bug（vec/mode/lr/cycles 错位导致
    日志数值荒谬）；`--headless` 下打印前若干次 IRQ 触发/恢复点，便于追踪
    多次中断。
- **怎么验证**：B9c 用例改为 FFXII 分发器风格（push lr / 破坏 r1 / pop pc），
  断言 handler 破坏的 r1 在返回后恢复、CPSR 恢复、被中断指令重执行。全量
  **633 项检查 0 失败**。
- **真 ROM 现象**：第一次 VBlank 的完整处理链（含处理函数里嵌套的大块内存
  填充）执行完成；但第二次 IRQ 前 ARM9 落入未映射 0x0027xxxx 漂移。根因怀疑：
  模拟器所有模式仍共用 SP，IRQ handler 用 System 栈压栈/嵌套，覆盖了用户现场
  （真机 IRQ 有独立 r13_irq）。排 B9g：实现按模式独立的 r13/r14。

## 2026-09-05 · 21-B9g — 模式私有 r13/r14（banked registers）

- **做了什么**：`arm_cpu_t` 增加 User/System 主 r13/r14 槽与 FIQ/IRQ/SVC/ABT/UND
  私有 r13/r14 槽；新增 `exec_apply_cpsr`：替换 CPSR 时先把当前可见 r13/r14
  存入所属模式，再加载新模式的私有值。所有模式切换入口统一改走它——MSR 写
  CPSR、异常入口、SUBS pc/LDM ^ 异常返回、IRQ HLE 现场恢复。
- **启动序列验证**：FFXII 0x0200080C 起分别切 SVC/IRQ/System 设置栈
  （0x027E3FC0 / 0x027E3F7C / 0x027E3AF8），现在三者真正隔离；B9c 用例改为
  先配置 IRQ 专用 SP 再触发中断。新增 `[case 21-B9g]`：SVC/IRQ/System 往返
  各栈值保留不串（10 项）。全量 **643 项检查 0 失败**。
- **真 ROM 效果**：ARM9 不再落入未映射区跑飞；第一次 IRQ 后主程序继续执行，
  停在 0x02007CE0 的链表遍历（与 ARM7 0x037FC8A0 轮询配合）。日志新露出
  ARM9 读 0x04000280-2BF 区域未知 IO，排 B9h 查这批寄存器。

## 2026-09-05 · 21-B9h — LDM/STM ^ 的 User 槽语义 + 中断位按硬件修正

- **做了什么**：
  - `exec.c` 块搬移补上 ARM `^` 规则：S=1 且寄存器列表不含 PC 时，特权模式读写
    **User/System 槽的 r13/r14**。此前 SVC 下执行 `ldm r0,{r0-r14}^` 会把任务现场
    装进 SVC 私有 SP/LR，FFXII 协作式调度“切到 A 却还在跑旧现场”，最终就绪队列
    A/B 的 next 互相指成环，删除任务函数在环上死转（0x02007CCC）。
  - headless 进度附带两核 cpsr/r0-r3/IME/IE/IF；“IF&IE pending 但 CPSR.I 屏蔽”
    只提示一次，便于区分“中断没来”和“来了被关”。
  - VBlank 从早期简化的 bit3 改回 NDS 硬件 bit0（bit3 实为 Timer0）；
    Timer0-Timer3 溢出按 TMxCNT_H bit6（IRQ 使能）置 IF bit3-bit6。
- **怎么验证**：新增 `[case 21-B9h]`：SVC 下 `LDMIA r0,{r0-r14}^`，断言 SVC 私有
  SP/LR 保持、User 槽 SP/LR 被装入；6.x 中断用例与位常量从 bit3 改 bit0。
  全量 **649 项检查 0 失败**。
- **真 ROM 效果**：就绪队列不再成环；第二次、第三次 VBlank IRQ 均能进入 handler
  并完整返回（`--headless 4000000` 可复现）。Timer0（0x04000100/02=0x00C1）溢出
  已能置 IF。当前停在 0x0200EA84 等待对象忙位（0x02078F0A bit1）清 0，
  疑与 Timer0/GX/DMA 完成事件相关，排 B9i。

## 2026-09-05 · 21-B9i — FIFO 送达 ARM7 + ARM7 IRQ 槽跳板

- **做了什么**：
  - `fifo.c`：修复 CNT 合并写——FFXII 用“高字节 0xC4”一次写完成错误应答（bit14）、
    使能（bit15）与收非空 IRQ（bit10）；旧实现把含错误位的写入当纯应答，跳过了
    使能位更新，导致 `cnt9/cnt7` 恒 0、ARM9 的 FIFO 命令全部被静默丢弃。
  - `cpu.c`：ARM7 首中断现场槽从猜的 0x03FFFFFC 改为实测的 **0x0380FFFC**
    （ARM7 WRAM 顶，FFXII 装的是 0x037FB8F4），并补 ARM7 IRQ 槽跳板 + HLE
    现场保存/恢复（与 ARM9 DTCM 槽对称）。
- **怎么验证**：新增 `[case 21-B9i]` 两项 10 检查：CNT 合并写后 send 能入队并置
  ARM7 IF18；ARM7 触发 IRQ 能跳到 0x0380FFFC 指定的 handler 并恢复现场。
  全量 **659 项检查 0 失败**。
- **真 ROM 效果**：ARM9 的 FIFO 命令（0x04000188）真正进入队列并触发 ARM7 IRQ；
  ARM7 handler 能完整执行并返回（不再从低向量 0x18 跑飞）。ARM9 仍停在
  0x0200EA84 等忙位，下一步 B9j 查 ARM7 回执路径/忙位清除。

## 2026-09-06 · 21-B9j（后半之二）— ARM7 IRQ 入口帧预填

- ARM7 调度器 0x37FBA10 保存旧任务时会用 `ldmib sp!,{...}` 从 IRQ 栈上方
  （0x380FF80..94）取 r0-r3/r12/lr；该区域本应由真机 IRQ 入口帧预填，但模拟器
  ARM7 槽跳板此前直接跳到用户 dispatcher，导致读到 0、把调度器上下文 PC 存坏。
- 修复：ARM7 IRQ 槽跳板在跳用户 handler 前把当前 r0-r3/r12 与
  `lr=被打断PC+4` 写到 IRQ SP 上方对应 6 个字，再进入 dispatcher。
- 验证：全量 **665 项检查 0 失败**；headless 3200 万步 ARM7 不再跌进
  0x0142xxxx/0xFFFFFFxx 跑飞，稳定停留在有效代码区。

## 2026-09-06 · 21-B9m — ARM9 CP15 WFI（MCR p15,0,r0,c7,c0,4）

- NDS9 没有 HALTCNT 寄存器，FFXII 的 OS 空闲任务直接执行 CP15 WFI 指令；
  旧实现把它当普通 CP15 写忽略，空闲任务变成满速空转。
- `cpu_step` 对 ARM9 的 `0xEE070F90` 特殊处理：中断未挂起 → PC 不动等待；
  挂起到（IME 门控）→ 清 CPSR.I 后走正常 IRQ 入口。对应 GBATEK “NDS9 的
  CP15 Halt 只受 IME 门控，IME=0 才锁死”的口径。
- 验证：全量 704 项后保持 0 失败；headless 空闲任务 CPU 周期不再随 VBlank
  无界增长，ARM9 每次帧中断被正常唤醒。

## 2026-09-06 · 21-B9s — ARM BLX 立即数 + ARM7 Halt 唤醒语义

- **ARM BLX 立即数**：旧实现只补了 `BLX Rm`（B9e），FFXII 0x02012500 的
  `BLX 0x020007A8` 被误译成普通 B：既没写 LR、也没切 Thumb，执行落在安全区
  “字节序占位区”，之后跳进错误路径，ARM9 永远不会走到 service13 FIFO 发送。
  按 ARMv5 编码补：cond=1111 的分支 = BLX 立即数，`r14=PC+4`、无条件置 T、
  目标按 H 位补 2、清最低位。FFXII 用它进安全区里的 Thumb SWI 桩
  （`svc #0x0B; bx lr`）。
- **ARM7 Halt 唤醒**：IRQ HLE 原本对所有等待都“回到被打断指令重执行”。
  对 IntrWait 这是对的，但 ARM7 `SWI 0x06 Halt`（Thumb `DF06`）被中断唤醒后
  应继续 SWI 之后（`bx lr` 回调用方），否则 FIFO 处理完再次睡死，后续
  回执/队列逻辑永远不执行。修复：恢复现场时若被打断指令是 Thumb SWI6，
  PC 前进 2 字节。
- **ARM9 CP15 WFI 唤醒**：同族修复——WFI 被中断唤醒时应先“完成”再进 IRQ，
  PC 前进 4；否则 IRQ 返回后仍停在 WFI 上重执行，空闲任务永远走不到
  WFI 之后的下一条指令。
- 验证：新增 `[case 21-B9s]` BLX 立即数单测 5 项；全量 **723 项检查 0 失败**。
  真 ROM 的 BA84/BA94（0xF3141/1）首次对上参考值，ARM7 会回发
  C0204006 类 FIFO 回执，ARM9 离开 0x0200EA88 忙等，双核推进到系统空闲
  （0x0200957C WFI / SWI6 Halt）。下一卡点：ARM9 空闲后尚未开始显示初始化，
  等待下一个启动事件。

## 2026-09-06 · 21-B9t（分析）— ARM7 Halt 唤醒后缺少周期任务路径

- 参考核心 C0204006 之后会从 Halt 醒来进入周期任务（r10=0x0380A9E8 等），
  由它调用 0x4588 入队 service-0x10；本地同样回到 Halt，但 BIOS HLE 把
  SWI 0x06 当纯等待，唤醒后没有“继续周期任务/调度”的语义。
- q82D8/q8248/q82C4 在两次回执发送瞬间均为 0，排除队列标志差异。
- 下一步：给 ARM7 Halt HLE 补唤醒后调度路径，或改走参考 ARM7 BIOS
  0x000011xx/0x00001Fxx 的异常返回与 SWI 分发语义。

## 2026-09-06 · 21-B9x — ARM9 IRQ 入口补 BIOS 六字帧

- 根因：FFXII 的 ITCM 中断分发器在 0x01FF8164 附近会从 IRQ 栈弹
  r0-r3/r12/lr 六个字保存“被打断任务”的现场。真机这些字由 ARM9 BIOS
  高向量入口（FreeBIOS `interrupt_handler` 同款 `stmdb sp!,{r0-r3,r12,lr}`）
  在跳用户 handler 前压栈；本地 HLE 直接跳槽指针，导致分发器弹到栈底 0，
  把空闲任务上下文写坏后 PC 弹到 0。
- 修复：跳 ARM9 handler 前先压 6 字帧（r0-r3/r12 + 被打断 PC+4），并下移 IRQ SP。
- 验证：`[case 21-B9c]` 补 3 项帧断言；全量 **738 项检查 0 失败**。
  真 ROM 不再跳低地址空扫，稳定停在 0x0200957C 空闲等下一帧事件。

## 2026-09-06 · 21-B9y — IRQ 返回时恢复 ARM9 IRQ SP

- B9x 压帧后 IRQ SP 每次入口下移 0x18，但本地 HLE 不走 BIOS 的
  `ldmia sp!,{r0-r3,r12,lr}` 返回路径，SP 一直不回弹；约 180 帧后溢出到
  DTCM 0x027E0000 的 IRQ handler 表，把 FIFO 表项 0x027E0048 覆盖成
  0x04000188，下一次 FIFO 中断直接跳到 IO 地址空扫。
- 修复：`irq_hle_ctx` 记录压帧前的 IRQ SP，`irq_hle_restore` 返回前恢复；
  `[case 21-B9c]` 补 SP 恢复断言。全量 **739 项检查 0 失败**。

## 2026-09-06 · 21-B9zg — ARM7 STM 基址在列表内的保存语义

- melonDS `A_STM`：ARM7 的 STM 在 rn 也在列表且存在更低编号寄存器时，rn 槽保存
  当前写地址（FFXII OS 上下文保存依赖）；本模拟器此前保存原 rn 值。
- `exec_block_transfer` 按此口径实现，仅影响 ARM7 STM 且 rn 槽不是最低列表寄存器。

## 2026-09-06 · 21-B9zn — A_STM 同一口径补到 ARM9

- 对照 melonDS `ARMInterpreter_LoadStore.cpp` 的 `A_STM`：该“基址在列表内保存
  当前写地址”是 ARM9/ARM7 共用的解释器语义，不是 ARM7 专属行为。
  B9zg 只给 `is_arm7` 生效，ARM9 的 OS/IRQ 上下文保存仍按旧口径写回原 rn 值。
- 修复：`exec_block_transfer` 去掉 `is_arm7` 条件，同一规则作用于两颗 CPU；
  新增 `[case 21-B9zn]` 4 项 ARM9 断言。全量 **751 项检查 0 失败**。
- 真 ROM 复测：该修复本身尚不足以进入第二阶段，但用发送指令级探针重新确认了
  现状——ARM9 已能发出 service11（2B/81E3E82B/AB），ARM7 回 6B 两次，随后仍回
  0x0200957C 空闲；与 B9zh..zm 记录的“service11 从未出现”不同，需以本次为准。

## 2026-09-06 · 21-B9vv — 周期成本模型骨架

- 按大改造路线第 1 步，给 `arm_cpu_t` 增加 `step_cycles`：普通指令=1，
  WFI 无挂起中断=0（等待不消耗调度时间）。
- `cpu->cycles` 仍保留“已执行指令数”语义，测试计数不受影响；后续事件
  调度器用 `step_cycles` 累计真实时间戳。
- 新增 `[case 21-B9vv]` 4 项（WFI step_cycles=0/不增指令计数、普通指令
  step_cycles=1/指令计数+1）。全量 **755 项检查 0 失败**。

## 2026-09-06 · 21-B9wa — ARM9 IRQ 返回桩改为 FreeBIOS 尾部

- 逐指令对照（方式 1）：参考 ARM9 解释器在 0x01FF8120 后打印连续 [tr]，
  本地临时加同窗口 [ltr]。第二次 6B 里 ITCM 0x01FF8120→01FF8198 两侧完全
  一致，分歧只在 0x01FF81A0 的 `ldmia sp!,{pc}`：
  - 参考弹到 **0xFFFF06F0**（FreeBIOS IRQ 入口 `mov r14,r15` 留在栈里的
    返回桩），随后 `ldmia sp!,{r0-r3,r12,r14}; subs pc,r14,#4` 把新任务
    接到 0x02007F74/0778C，service11 续发 1AB；
  - 本地弹到 **0x02009580**（被打断旧空闲任务 PC），`irq_hle_restore`
    恢复旧 sleep 上下文，随后 077D4 把 F18 写回 76F24。
- 根因：本地 ARM9 IRQ HLE 把 lr 设成“被打断 PC”；真 FreeBIOS 在入口
  `stmdb sp!,{r0-r3,r12,lr}` 压六字帧后把 lr 改成桩地址 0xFFFF06F0，
  ITCM 分发器弹栈拿到的是桩地址而不是旧任务 PC。
- 修复：ARM9 IRQ 槽跳转等价执行 FreeBIOS 0xFFFF06D8 入口（压六字帧 +
  lr=0xFFFF06F0）；新增 `bios_irq_tail9` 在 PC==0xFFFF06F0 且 IRQ 模式时
  模拟尾部两条指令，用 SPSR_irq 恢复 CPSR。IRQ HLE 私有恢复只保留给 ARM7。
- 验证：`[case 21-B9c]` 改为新语义（lr=桩、弹回桩、tail 恢复后重执行被打断
  指令、IRQ SP 回弹）；全量 **763 项检查 0 失败**。真 ROM 60M 步下 service11
  从 3 次续到 AB/1AB/22B/26B/00040005 等多轮，终点推进到 16.4M 周期空闲。

## 2026-09-06 · 21-B9wf — ARM7 FreeBIOS 低地址等待路径 HLE

- **做了什么**：
  - `arm_cpu_t` 增加 bios7 状态（active/delay/halted/pc/ret/cpsr）：SWI 3 进入
    0x115C 循环、SWI 6 进入 0x1158 Halt 暂停的参考级 HLE，不执行 BIOS 字节码。
  - `cpu_step` 把低地址整步放到 IRQ 检查之前：Halt 唤醒先执行
    0x1158→0x112C 尾段恢复调用方，IRQ 尾随其后；修复“IRQ 在 BIOS 内部抢占，
    调度器把 0x1158 当任务 PC 保存、LDM ^ 恢复出主存地址跑飞”的路径。
  - 事件 headless 驱动：单核等待时系统时间由运行核推进；等待核唤醒时成本跳到
    当前系统时间（ARM9×2），不再把暂停时间当积压指令补跑。
- **怎么验证**：`[case 21-B9wf]` 20 项断言（delay 三轮+尾部恢复；Halt 暂停
  不耗周期+置位唤醒+尾部恢复）；全量 **790 项检查 0 失败**。
- **结果**：真 ROM ARM7 首中断与后续暂停都落在 0x115C/0x1158（与参考低地址
  一致）；长跑不再主存跑飞。标题所需的周期时间模型/事件表继续在 B9wf 后续
  小步推进。

## 2026-09-06 · 21-B9wg — ARM7 IRQ 入口/尾部改 FreeBIOS 桩（对称 B9wa）

- **做了什么**：
  - ARM7 IRQ 触发不再用模拟器私有 `irq_hle_restore` 保存/恢复现场，改为等价
    执行 FreeBIOS 0x1FB0 入口：`stmdb sp!,{r0-r3,r12,lr}` 压六字帧
    （lr=被打断 PC+4）、SP 下移 0x18、lr 设 0x1FC0 返回桩后跳用户 handler。
  - 新增 `bios_irq_tail7`：ARM7 在 IRQ 模式弹回 0x1FC0 时模拟
    `ldmia sp!,{r0-r3,r12,r14}` + `subs pc,r14,#4`，用 SPSR_irq 恢复 CPSR。
  - `[case 21-B9i]` 更新为 FreeBIOS 语义：handler LDMFD 先回 0x1FC0 桩，
    再弹帧回被打断点，随后重执行被打断指令。
- **怎么验证**：`[case 21-B9i]` 增加 tail/ret 两步断言；全量
  **792 项检查 0 失败**。
- **结果**：真 ROM 短跑 ARM7 继续正常进出 0x1158/0x115C；事件驱动 20M 步内
  ARM9 也能多次离开空闲并跑显示/服务代码。超长跑到 Timer0 回绕后的标题段仍
  有双核跑飞，下一小步继续对照该段任务恢复路径。

## 2026-09-06 · 21-B9wh — CLZ Rd 掩码修正（标题解码越过原崩溃点）

- **证据**：逐指令对照 0x01FF847C 时两侧进入状态一致
  （r3=617730B0、r6=01FF9990、lr=01FFAED4），但 0x01FF8480 之后分歧：
  参考 r10=1、CPSR 不变，本地 r10 保持 0x00009858、CPSR 被清成 0000001F。
  反汇编按 opcode=CMN 显示，实际编码是 ARMv5 **CLZ r10,r3（E16FAF13）**：
  CLZ 与 CMN 共用 S=0 的 opcode 空间（表行 0x16），靠 bit19-16=1111 与
  bit11-4=11110001 区分。
- **根因**：B9d 的 CLZ 识别写成 `(insn & 0x0FFFFFF0) == 0x016F0F10`，
  掩码没有让 Rd（bit15-12）保持可变——只有 Rd=0 的 CLZ 能命中，Rd=10 时
  落到通用数据运算分支被当 CMN 执行，r3 被错误移位成 0，标题位流解码在第
  二次进 0x847C 时从表尾外取到垃圾并跳 EA000988。
- **修复**：掩码改为 `0x0FFF0FF0`（固定 bit27-20、bit19-16、bit11-4，
  留出 Rd/Rm），识别条件不变。
- **怎么验证**：`[case 21-B9d]` 补“CLZ r10,r3 且 Rd≠0”回归：r10=1、
  CPSR 不被触碰、PC+4；全量 **795 项检查 0 失败**。
- **真 ROM 效果**：修复后同一调用点 r10=1，标题位流解码继续走到
  0x01FF8524/8528 与 0x01FF86A4 复制循环（向 0x021B1573 写 0xA0A0A0A0
  等填充字节），不再跳 EA000988；长跑 ARM9 持续在 0x01FF84xx/0x020119xx
  服务与标题段活动。ARM7 后期 PC 仍沿未映射主存跑飞，作为下一对照点。

## 2026-09-06 · 21-B9wk — ARMv5 DSP 乘法（标题位流解码逐指令对齐）

- **证据**：给参考 melonDS ARM9 解释器与本地 `cpu_step` 同时加
  0x01FFD7F0-0x01FFDD00 全寄存器 trace，按“参考只记录条件成立指令”的规则
  对齐。首个寄存器分歧是 0x01FFD904 的 `E1690584`：本地 r9=9、参考
  r9=0xFFFFE740，CPSR 也随后的比较不同。该编码是 **ARMv5 SMULBB
  r9,r4,r5**（bits27-20=0x16、bit7=1、bit4=0），本地按普通数据运算
  （row 0x16 的 S=0 空间）处理，未写 r9。
- **怎么做**：`exec.c` 新增 `exec_dsp_mul`，实现 SMLAxy、SMLAWy、
  SMLALxy、SMULxy、SMULWy 五种 DSP 乘法（bit5/bit6 选择高低半字、
  SMLAxy/SMLAWy 的 Q 标志 bit27、SMLALxy 的 64 位累加）；识别掩码
  `(insn & 0x0F900090) == 0x01000080`（row ∈ {0x10,0x12,0x14,0x16}、
  bit20=0、bit7=1、bit4=0）。ARM7（ARMv4T）无这些指令，仍走未定义异常。
  掩码踩坑：若允许 bit20=1 会把 `CMP/TST` 行误判成 DSP 乘法，因此专门
  用 bit20 过滤。
- **怎么验证**：新增 `[case 21-B9wk]` 8 项乘法断言 + 2 项
  “CMP 不是 DSP 乘法”回归；全量 **818 项检查 0 失败**。修复后同一段
  trace 连续 **42,899 条指令**寄存器与参考完全一致，ARM7 长跑稳定在
  0x1158，不再因子虚乌有的 DSP 指令掉未定义向量。

## 2026-09-12 · 21-B9ws — 异常入口 CPSR 口径 + ARM7 低地址路径接进 cpu_step

- `arm_exception` 的低 8 位改为参考核口径（SWI 0x93 / 未定义 0x9B / 中止 0x97 /
  IRQ 0xD2 / FIQ 0xD1）：**入口一律清 T**、SWI 与未定义同样置 I。此前
  Thumb SWI 进向量时 T 还在，游戏把「PC 停在 0x08 向量」的现场存进任务上下文后，
  恢复出来的 CPU 状态与参考不同，会走到分发器读错编号的岔路（实测导致 SWI 0
  SoftReset 被误触发、旧栈帧弹到野地址）。
- `cpu_step` 的顺序改为：① ARM7 HALTCNT 暂停检查（`(IF&IE)` 唤醒，step_cycles=0
  表示仍在暂停）→ ② 定时器/卡带推进 → ③ IRQ 检查（含 ARM9 WFI 特例）→
  ④ ARM7 低地址 BIOS 路径 `bios7_low_step()` → ⑤ 常规取指执行。
  这样 IRQ 会在与真机相同的边界插入：被打断的是 BIOS 里的 0x1158（b swi_complete），
  而不是调用方代码——参考核实测 543/596 次 IRQ 正是打断在这个地址。
- 删除模拟器私有桩：`bios7_active/delay/halted/pc/ret/cpsr` 字段、
  `bios_irq_tail7()`、`irq_hle_restore()`、ARM7 IRQ 槽跳转块；`irq_hle_ctx_t`
  随之删除，诊断计数改名为 `arm_cpu_t.irq_count`（runner 摘要沿用）。
- 新增 `cpu_direct_boot()`：按 melonDS `SetupDirectBoot` 设置两核
  r12/r13/r14 与 sp_irq/sp_svc（ARM7 0x0380FD80/FF80/FFC0、
  ARM9 0x03002F7C/3F80/3FC0）。

## 2026-09-12 · 21-B9yh — 按取指区域计费（非顺序取指加价）

**问题**：本地每条指令都只花 1 个系统时钟单位，而参考核会按取指区域/顺序性
计费。实测（本地 vs 参考核 melonDS+FreeBIOS）：

| 口径 | 第 26 帧 ARM9 指令 | 第 1900 帧 ARM9 指令 |
|---|---|---|
| 参考核 | 8,328,123（14.56M 单位 ⇒ 1.75 单位/条） | 720,588,787 |
| 本地：全部 1 单位/条 | 12,065,443（**+45%**） | 784,957,288（+8.9%） |
| 本地：主存一律 2 单位/条 | 7,112,902（−15%） | 618,299,382（−14.2%） |
| 本地：**仅非顺序取指加价到 3** | **8,138,527（−2.3%）** | **668,450,486（−7.2%）** |

差额集中在**启动期**（ARM9 代码在主存 0x02000800 起）而不是长程，说明参考核
的口径是「顺序取指便宜、非顺序取指贵」（melonDS ARM9 主存 S16=1 / N16=8，
ARM 态顺序 S32=2 / 非顺序 N32=9），而不是「主存一律贵」。

**实现**（`cpu.c` / `cpu.h`）：

- 新增 `arm_cpu_t.next_fetch_pc`：每步记下「顺序取指应在的下一条地址」
  （按执行前的 T 位算指令长度；`cpu_reset` 初始化成入口地址）；
- `cpu_fetch_cost()`：ARM7 恒 1；ARM9 的 ITCM（0x01FF8000+32KB）恒 1
  （melonDS `CodeRead32` 的 `addr<ITCMSize` 分支）、其它区域 1、
  **主存的非顺序取指 3**（按 melonDS 的 N32/S32 比例缩小）；
- `cpu_step` 在指令执行后取 `max(step_cycles, fetch_cost)`（`step_cycles=0`
  的等待态不计费），于是 runner 的系统时钟推进与指令实际代价挂钩。

**验证**：`[case 21-B9vv]` 扩到 6 条断言（主存顺序 1 / 主存分支 3 / ITCM 1 /
ARM7 1）；全量 **926 项检查 0 失败**；里程碑不回归——900 帧
`ARM7=00001158`、`disp=00121F10`、`0x027FFC30=0xFFFFFFFF`、
`0x03808240=1`、`0x0380BA7C=1`。

### 21-B9yi 续：主存非顺序取指代价按**时间线**定标（默认 1）

21-B9yh 的 3 单位是按**指令数**校准的（26 帧 −2.3%、1900 帧 −7.2%）；
改用「游戏进度 vs 帧号」这条时间线重新定标后结论相反——本地在读卡带阶段
比参考核慢，而且越拖越多：

```
同一读取序号对应的帧号差（负 = 本地超前）
 读取序号    代价=3   代价=2   代价=1
   200000     +11      -1      -13
   350000    +110     +49       -8
   500000    +160     +74       -9
```

⇒ 默认改为 **1**（`NDS_ARM9_NOSEQ=1..8` 或 `cpu_set_nonseq_cost()` 可调），
本地在整个读卡带阶段稳定在 −8~−13 帧。教训：**指令数一致 ≠ 时间线一致**，
本项目后续的时序定标一律以「同一逻辑进度对应的帧号」为准。

### 21-B9yi 续：定时器/卡带时钟按「本步实际周期」推进

`cpu_step` 传入 `io_advance_timers` / `io_advance_cart` 的周期数此前固定为 1
（每条指令 1），而 ARM9 平均每条指令约 1.2 个系统单位 ⇒ 定时器与卡带取数
节奏系统性偏慢。现在改为传**上一条指令实际消耗的周期数**（`prev_cost`，
等待态为 0 时按 1 计）。实测：

- 读卡带进度曲线不变（该阶段瓶颈在 CPU，不在卡带节奏）⇒ 说明这不是卡死主因；
- 口径与 melonDS「定时器/卡带挂系统时钟」一致，保留。

### 21-B9yi（续12）— IRQ/模式日志帧区间化 + 「卡带中断挂错核」的清除

- `NDS_IRQLOG` / `NDS_MODELOG` 支持 `LO-HI` 帧区间（此前只能从第 0 帧开印，
  跑到 1900 帧时 20 万条上限早满，等于看不到目标区间）。`locirq` 行同时打印
  `if/ie`，可直接与参考核 `refirq` 的 `if/ie` 逐条对照。
- **ARM7 的卡带中断嫌疑排除**：卡带完成中断此前两核都挂（`irq_set_card(&io->irq[0])`
  + `[1]`），而 melonDS 是 `SetIRQ(Num, IRQ_CartXferDone)`、`Num` = 卡带槽所属核
  （NDS 槽 0 = ARM9）。本地 ARM7 因此自 1880 帧起 IF 一直挂着 bit19
  （`if7=00080000`，参考核 `if7=00000000`），会污染 `HaltInterrupted()`
  的 `(IF & IE) != 0` 判据。改成只挂 ARM9 后：

  ```
  f=1889-1898  本地 if7=00000000 ie7=0104009D   ←→ 参考核 if7=00000000 ie7=0104009D
  ARM7 稳定停在 FreeBIOS Halt 0x1158/0x115C，与参考核同址
  ```

  ⇒ 结论：**ARM7 FreeBIOS 低地址 Halt/调度路径本轮无嫌疑**（IF/IE 逐位一致、
  低地址取指直方图此前已 93/93 一致），剩余工作集中在 ARM9 侧的卡带任务/DMA/IRQ
  插入相位。

### 21-B9yi（续19）— IRQ 日志扩展到两核 + 两核 IRQ 画像对比

`NDS_IRQLOG=LO-HI` 现在**两个核都记**（ARM7 行前缀 `locirq7`），参考核 `REF_IRQLOG`
同样输出 `refirq7`。f=2000-2060 逐位对比结果：

```
ARM7  本地 12.5 次/帧  IF bit0=61 bit2=244 bit3=8 bit4=452
      参考 12.5 次/帧  IF bit0=61 bit2=244 bit3=8 bit4=452      ← 完全一致
      （= VBlank 1/帧 + VCount 匹配 4/帧 + Timer1 7.4/帧）

ARM9  本地 207 次/帧  bit1=12325 bit18=244 bit11=58
      参考 178 次/帧  bit1=10525 bit18=244 bit11=0
      ← HBlank 多 17%；DMA3 完成中断多 ~1 次/帧
```

⇒ 驱动游戏调度节拍的 ARM7 侧**节拍正确**（此前观察到的「任务现场保存次数差 20%」
不是 ARM7 中断率造成的）；ARM9 剩两处待查：HBlank 触发条件（本地 263 行全挂 vs
参考更少）与 DMA3「IRQ on end」的收尾时机。

### 21-B9yi（续38）— 性能实验：每指令 IO 推进的「快速返回」守卫（**实测更慢，已回退**）

**动机**：`cpu_step()` 每条指令都要调 `io_advance_timers()`（4 个定时器）与
`io_advance_cart()`（GX 推进 + GX-FIFO DMA 续跑 + 卡带取数循环）。直觉上「先判断
有没有事、没事就跳过」应该更快。

**做法**：新增 `gx_idle()` 与 `io_arm9_clock_active()`，在 `cpu_step()` 里先做
「本核定时器是否使能」「GX/卡带/GX-FIFO DMA 是否全空闲」的判定再决定是否调用。

**A/B 实测**（同一台机器、交替各跑两次 4200 帧、相同输入脚本）：

```
基线（8aa872a，无守卫）：150.6s (27.9fps) / 149.7s (28.1fps)
加守卫              ：224.4s (18.7fps) / 223.7s (18.8fps)   ← 慢 50%
```

**结论**：守卫本身要读 GX 队列状态 + 4 个 DMA 通道 + 卡带相位，而本游戏运行中
这些几乎**始终是忙的**（GX 一直有命令、卡带一直在预取），于是每次都白付一遍
判定开销。⇒ **已回退**（`git checkout` 到 8aa872a 状态）。

**经验**：性能改动必须 A/B 实测（基线两次 150.6/149.7 秒非常一致，说明这套交替
测量的噪声很小）；「少调用 = 更快」在热点路径上不成立。

## 2026-09-13 · 21-B9yi（续49）：**指令级 trace 默认开着** —— 窗口模式一直跑在 <2 fps

**做了什么**：`exec.c` / `thumb.c` 的指令级跟踪开关默认值 `1 → 0`。

```c
static int g_trace = 1;   /* 旧：bring-up 期为了看前几十条指令 */
static int g_trace = 0;   /* 新：诊断设施必须 opt-in */
```

**为什么**：这个开关就是「每条 ARM/Thumb 指令 `printf` 一次」。无头路径会在
`runner_headless_frames()` 开头显式 `exec_set_trace(0)`，**窗口路径从来没关过**，
于是用户在窗口里玩的时候，相当于解释器每执行一条指令就格式化打印一行。

**怎么验证**（同机同 ROM 同输入脚本）：

```
窗口 600 帧（满键 0x3FF）  修复前：>330 s 未跑完（<2 fps），stdout 1.97 GB 且还在涨
                          修复后：7.13 s（84 fps），stdout 3.4 KB
窗口 3000 帧（满键）        修复后：104.4 s（28.7 fps）
无头 3000 帧（同输入）      修复后：100.6 s（29.8 fps）  ⇒ 窗口宿主开销可忽略
```

**结果**：✅ 保留。零回归：931 项单测 0 失败；同配置 3000 帧无头跑两遍，
**416 行 disp-change 时间线逐行相同**（trace 只影响打印，不影响语义）。
另记负结果：`NDS_NOIOADV=1`（跳过每指令 IO 推进）实测**慢 4 倍**——跳过推进会让
定时器/卡带不再产生事件，游戏退化成满速空转，属于「行为被改坏」而非优化，已回退。

## 2026-09-13 · 21-B9yi（续50）：阶段剖析 `NDS_PROF=1` —— 瓶颈在核心，不在 IO 推进

**做了什么**：新增诊断开关 `NDS_PROF=1`（`src/cpu/cpu.c`）：用 `rdtsc` 把每条指令
的时间拆成「IO 推进（`io_advance_timers` 4 个定时器 + `io_advance_cart`：GX 时钟/
卡带时钟/DRQ→DMA 检查）」与「取指/译码/执行/访存」，并统计指令条数。
关闭时只多一次全局 int 判断；结束时 `atexit` 打一行汇总。

**为什么要自己写**：本机没有可用的采样 profiler —— `gprof` 在这套 MinGW 上
连「hello + 忙循环」都采不到样本（flat profile 恒为空，已实测），LTO 又让符号消失。

**实测**（1500 帧、满键 0x3FF、49.2 s、6.48 亿条指令）：

```
prof: steps=648120557  io-advance=17.1%  rest(fetch/exec/mem)=82.9%
```

**结论**：**解释器核心占 83%，IO 推进只占 17%**。配合行为无关的
`NDS_NORAST=1` A/B（3000 帧 87.0 s vs 89.7 s ⇒ 光栅化 ~3%），
可以确定下一步该优化**核心访存与每步固定开销**，而不是继续折腾 IO 推进。

## 2026-09-13 · 21-B9yi（续52–54）：每步固定开销的清理（含一个负结果）

### 续52：三处结构性削减

1. **诊断开关改为一次性初始化**：`NDS_ARM9_NOSEQ` / `NDS_PCSAMPLE` / `NDS_PROF`
   原来是「静态变量 == -1 就解析一次」的写法，散在热路径里 ⇒ 每条指令都要判一次
   「初始化过没有」。改为在 `cpu_create()` 里调用 `cpu_diag_init()` 一次性解析。
2. **WFI 检测不再额外取指**：旧代码 `if (!cpu->is_arm7 && cpu_fetch(cpu) == 0xEE070F90u)`
   **每条 ARM9 指令都要多读一次内存**去比对 WFI。现在：只在 ARM 态判断
   （Thumb 的 WFI 是 0xBF30，旧写法还可能把两个相邻 Thumb 指令拼成的字误判成 WFI），
   且把读到的指令字**缓存给下面的 exec 复用** ⇒ ARM 态每条指令只读一次指令。
3. **`bios_irq_tail9` 前置条件提到调用点**：绝大多数指令（ARM9 不在 IRQ 模式）
   因此连函数调用都省了。

### 续53：微清理（单独测**没有收益**）

把 `cpu->nds->bus->…` / `cpu->nds->io->…` 三级链式取址提到局部变量、诊断块只判一次
`bus->diag`、每步只算一次 `irq_pending` 供 WFI/屏蔽提示/受理 IRQ 复用。

**A/B（同机相邻配对，1000 帧满键）**：

```
r53（微清理）   ：17.67 s (56.6 fps) / 17.37 s (57.6 fps)
r52（微清理前） ：17.66 s (56.6 fps) / 17.56 s (56.9 fps)   ⇒ 差别在噪声内 ≈ 0
```

### 续54：把剖析插桩改成**编译期**开关（这一步才把上面的收益放出来）

原因：`NDS_PROF` 的取指/译码/IO 分桶要在热路径上每步插 rdtsc，**默认构建里
这些分支本身也要花钱**，正好抵掉了续52/53 的清理。改为`-DNDS_PROF=ON` 才编进去
（见 CMakeLists 的 `option(NDS_PROF ...)`），默认构建里两个宏退化成空操作。

**A/B（同机相邻配对，1000 帧满键）**：

```
r54（清理 + 无插桩分支）：17.45 s (57.3 fps) / 17.13 s (58.4 fps)
r52（改动前）          ：17.72 s (56.4 fps) / 17.52 s (57.1 fps)   ⇒ 约 -1.9%
```

**零回归**：931 项单测 0 失败；1000 帧 **70 行 disp-change 时间线逐行相同**；
2000 帧双屏统计逐值相同、最终截图 **SHA-256 完全相同**。

**结论**：微优化只能拿到 ~2%。要再上一个台阶（目标 60 fps）需要**结构性**改动，
下一刀应砍在「每步的调用/检查骨架」或「IO 推进的事件化」上。

## 2026-09-13 · 21-B9yi（续62）：LTO 其实**从来没生效过** —— 修好后 **-13.2%**

**怎么发现的**：做性能收口时顺手核对编译命令行，发现主构建 `build.ninja` 里
`FLAGS = -std=c11 -O3`，**根本没有 `-flto`**：

```cmake
check_ipo_supported(RESULT NDS_IPO_OK OUTPUT NDS_IPO_MSG)   # 旧写法
```

本工程 `project(nds-emu C)` **只声明了 C 语言**，而 `check_ipo_supported()`
默认按 **CXX** 探测 ⇒ 一直返回「CMake doesn't support IPO for current CXX compiler」，
`INTERPROCEDURAL_OPTIMIZATION` 从未设置成功。

> 更正旧记录：续41 曾把「LTO」记为**零收益实验（配对差 1%）**——那次对比实际是
> **两个都没开 LTO** 的构建，当然没有差别。本轮是它第一次真正生效。

**修复**：`check_ipo_supported(... LANGUAGES C)`，重新配置后 `build.ninja` 出现
`-flto=auto -fno-fat-lto-objects`。

**A/B（同机相邻配对）**：

```
1000 帧（轻段为主）：LTO 10.90 / 10.60 / 10.67 s  vs 无LTO 12.59 / 12.33 / 12.13 s
                     ⇒ 平均 10.72 s vs 12.35 s（**-13.2%**，帧率 +15%）
10000 帧（含重场景）：LTO 109.4 s vs 无LTO 122.2 s（**-10.5%**）
窗口 10000 帧（同会话相邻对照）：无LTO 133.1 s(75.1 fps) → LTO 121.0 s(82.6 fps)（**-9.1%**）
```

**零回归**：931 项单测 0 失败（LTO 版本）；1000 帧 70 行 disp-change 时间线逐行相同；
2000 帧双屏统计逐值相同、截图 SHA-256 完全相同（`A72E11A2…CC513`）。

**结果**：✅ 保留。解释器形态正是「大量跨 TU 小函数」，LTO 能把 `bus_read*` /
`timer_advance` / `io_advance_*` / `snd_*` 这些内联进热点路径。

## 2026-09-13 · 21-B9yi（续63/64）：剖析器自校准 + 「除法改移位」实测 0 → 瓶颈指向访存

### 续63：把 `NDS_PROF` 改成**抽样 + 自校准**（否则占比不可信）

上一版剖析器每条指令插 8 次以上 rdtsc，而一条指令本身才 ~100 周期 ⇒ 测量本身把
「other」桶撑到 50%+（实测 `cpu_step 占墙钟` 算出来 231%，明显不可能）。改成：

1. **抽样**：每 64 条指令只测一条（`s_prof_meas`），单条误差仍在但**占比可信**；
2. **每桶独立测量**：定时器 / 卡带-GX / 取指 / 译码执行各用自己的一对 rdtsc，
   不再共用同一个 t0（旧写法会让后面的桶替前面的桶付测量开销）；
3. **自校准**：在报告里连测 1000 组空 rdtsc 得到固有开销（本机 **16 周期/组**），
   从每个桶里扣掉后再算「每条指令多少周期」。

**校正后的结果**（10000 帧、含重场景、LTO+O3 的剖析版）：

```
每条指令（校正后）：定时器 16.7  卡带/GX 13.2  取指 0.0  译码执行 12.9  其余 130.6  合计 173.3 周期
占比：定时器 9.6%  卡带/GX 7.6%  执行 7.4%  其余 75.3%
```

**怎么读**：「其余（检查/记账/调用开销）」占绝对多数。它包含 `cpu_step` 的
函数进出、诊断/中断检查、`cpu_fetch_cost`、以及**等待访存的停顿周期**。
注意仍有残差：校准扣的是「一对空 rdtsc」的 16 周期，而真实测量还要加计数器
更新与分支（约 +10 周期），所以真实的「其余」应低于 130 周期。

### 续64：`RUNNER_SYS9` 里的**运行时除法** ⇒ 改移位，实测 **≈0**

发现调度循环里 `RUNNER_SYS9(c) = (c) / s_arm9_div` 的除数是**运行时变量**，
编译器只能生成 64 位除法指令（~20-40 周期），而它**每条指令被调用 3 次**
（选 ARM9/ARM7 走谁 + 推进系统时间）。口径只有 1 或 2，于是改成移位：

```c
static unsigned s_arm9_shift;                       /* 0=÷1、1=÷2 */
#define RUNNER_SYS9(c) ((uint64_t)(c) >> s_arm9_shift)
```

**A/B**（4 轮交替、2000 帧/次、取最小值做稳健估计）：

```
移位：min 28.10 s / 平均 28.26 s
除法：min 27.98 s / 平均 28.10 s      ⇒ **差别在噪声内（约 0%）**
```

**零回归**：2000 帧双屏统计逐值相同、截图 SHA-256 完全相同（`A72E11A2…CC513`）。

**结论（重要）**：热路径上少掉 3 条 64 位除法**测不出收益** ⇒ 解释器的瓶颈
**不在 ALU，而在访存（缓存未命中延迟）**。这解释了此前几轮的现象：
①续51「访存快路径」能拿到 -22%（减少的是访存次数与判定链）；
②续57 的 -O3 与续62 的 LTO 能拿到 10%+（更好的代码布局/内联 ⇒ 更少访存）；
③而单纯削减算术指令（本轮）几乎无收益。
**下一步应当继续朝「减少访存次数 / 改善局部性」方向做**，而不是继续抠算术。

### 续65：顺着「访存延迟」做定向实验 —— **指令预取 -1%（4/4 轮一致）**

按续64 的结论，试最简单的局部性手段：在取指处预取**下一条指令所在的缓存行**
（只对最常见的两个代码区做：ARM9 ITCM / Main RAM）。

**A/B**（4 轮交替、2000 帧/次）：

```
预取  ：28.33 / 27.88 / 27.96 / 28.07 s   平均 28.06 s（最小 27.88）
无预取：28.63 / 28.14 / 28.27 / 28.31 s   平均 28.34 s（最小 28.14）
⇒ **-1.0%**，且 4 轮**全部**是预取版更快（方向一致，不是噪声）
```

**零回归**：2000 帧双屏统计逐值相同、截图 SHA-256 完全相同。

**结论**：✅ 保留（代价只有一条 `prefetcht0`）。收益小但方向明确 —— 继续做
「访存/局部性」类优化比抠算术更有前途。

### 续66：把 `cpu->nds->bus` / `cpu->nds->io` 缓存进 CPU 结构 —— **实测 ≈0，已回退**

**动机**：热路径里每取指/访存都要走 `cpu->nds->bus->…` 这条**两级依赖加载链**
（`cpu->nds->bus` 在 cpu/exec/thumb 三个文件里共 67 处）；按续64「瓶颈在访存延迟」
的结论，把 `bus` / `io` 指针直接缓存进 `arm_cpu_t` 应该能缩短链、省下若干周期。

**做法**：`arm_cpu_t` 新增 `bus_t *bus; io_t *io;`，在 `cpu_create()` 里赋值
（nds.c 中 bus/io 都先于 CPU 创建），然后把三处文件里的 72 个 `cpu->nds->bus` /
`cpu->nds->io` 机械替换为 `cpu->bus` / `cpu->io`。

**A/B**（4 轮交替、2000 帧/次）：

```
缓存指针：30.92 / 27.79 / 27.71 / 30.55 s   最小 27.71 s
链式    ：30.50 / 30.37 / 27.54 / 27.50 s   最小 27.50 s   ⇒ **≈0（噪声内，方向不一致）**
```

**结论**：**已回退**（`git checkout` 回上一提交，避免无收益的大面积改动 + 缓存指针
带来的潜在不一致风险）。这条负结果进一步收窄了「访存瓶颈」的含义：
瓶颈不是 L1 命中的依赖加载链（这些被乱序执行吸收掉了），而是**真正的缓存未命中**
（模拟内存工作集 4MB 主存 + 656KB VRAM + 代码，访问分散）。
