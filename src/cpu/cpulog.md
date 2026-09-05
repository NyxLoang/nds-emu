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


