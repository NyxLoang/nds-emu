# Thumb 指令集（阶段 13）

## 为什么必须有 Thumb

- NDS 游戏主体代码绝大多数是 **Thumb（16 位）** 编译的：更省 ROM 空间，代价是寄存器/立即数范围受限。
- 真机 ARM9/ARM7 都在 ARM 与 Thumb 两种状态间切换：复位进 ARM，`BX`/`BLX` 切 Thumb；中断返回时恢复原状态。
- 我们阶段 3b–10 只实现了 ARM 状态（32 位指令）。没有 Thumb，商业游戏几乎一步都跑不了。

## 状态位 T

- CPSR 的 **bit5（T 位）** 指示当前指令集：`T=0` → ARM（32 位取指），`T=1` → Thumb（16 位取指）。
- `T` 位只能通过 `BX` / `BLX` / 异常进入/返回 改变，不能用 `MSR` 直接写（真机忽略该写）。
- 切换规则：`BX Rm` → `T = Rm 的 bit0`，`PC = Rm & ~1`（bit0 只决定状态，不进入地址）。

## 取指差异

| | ARM | Thumb |
|--|-----|-------|
| 指令宽 | 32 位 | 16 位 |
| 取指 | `bus_read32(PC)` | `bus_read16(PC)` |
| 推进 | `PC += 4` | `PC += 2`（分支/写 PC 例外） |
| 对齐 | 4 字节 | 2 字节 |

## Thumb 的「PC」约定

- Thumb 里读取 PC（`r15`）得到的值 = **当前指令地址 + 4**（流水线），用于 `ADD Rd, PC, #imm`、`LDR Rd, [PC, #imm]`、`B`/`BL` 目标计算。
- PC 相对寻址时基址需 **字对齐**：`(PC + 4) & ~3`。
- 我们模拟器的 `cpu->r[15]` 存的是「当前指令地址」，所以 Thumb 译码里令 `pc = r[15] + 4`。

## 指令分组（16 位，按高 5 位）

| 高 5 位 | 组 | 说明 |
|---------|-----|------|
| `000xx` | 移位 Rd,Rs,#imm5 | LSL/LSR/ASR |
| `00011` | 加/减 | ADD/SUB Rd,Rs,Rn 或 #imm3 |
| `001xx` | 立即数运算 | MOV/CMP/ADD/SUB Rd,#imm8 |
| `010000` | ALU | AND/EOR/LSL/LSR/ASR/ADC/SBC/ROR/TST/NEG/CMP/CMN/ORR/MUL/BIC/MVN |
| `010001` | 高寄存器 | ADD/CMP/MOV（可带 r8-r15）、BX/BLX Rm |
| `01001` | PC 相对加载 | LDR Rd,[PC,#imm8*4] |
| `0101` | 寄存器偏移访存 | STR/LDR/STRB/LDRB/LDRH/STRH/LDSB/LDSH |
| `011` | 立即偏移访存 | STR/LDR/STRB/LDRB（#imm5*1/4）、STRH/LDRH（#imm5*2） |
| `1000` | 半字访存 | STRH/LDRH Rd,[Rn,#imm5*2] |
| `1001` | SP 相对 | STR/LDR Rd,[SP,#imm8*4] |
| `1010` | 取地址 | ADD Rd,PC/SP,#imm8*4 |
| `10110000` | 调 SP | ADD/SUB SP,#imm7*4 |
| `1011` | PUSH/POP | PUSH/POP {reglist}(+LR/PC) |
| `1100` | 多寄存器 | STMIA/LDMIA Rn!,{reglist} |
| `1101` | 条件分支 | Bcond label（imm8） |
| `11011111` | 软中断 | SWI imm8（进 BIOS HLE） |
| `11100` | 无条件分支 | B label（imm11） |
| `1111` | 长跳转 | BL/BLX（两半字拼 22 位偏移） |

## BX / BLX 与 T 位联动

- ARM 状态的 `BX Rm` / `BLX Rm`：`T = Rm[0]`，`PC = Rm & ~1`（BLX 先把 `lr = PC+4`）。
- Thumb 状态的 `BX Rm`：同样 `T = Rm[0]`。`BLX Rm`：`lr = PC|1`，切 ARM（T=0）。
- Thumb 的 `BLX label`（长跳转第二种半字 H=1 时）：`lr = 当前 PC | 1`，切 ARM。
- 本阶段先实现 T 位切换与 BX/BLX，异常返回恢复 T 位在后续阶段随 SPSR 一起处理（SPSR 含 T 位）。
