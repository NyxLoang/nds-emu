#include <stdio.h>
#include "exec.h"
#include "bus/bus.h"
#include "bios/bios.h"

/* 指令级跟踪开关（exec_set_trace 控制） */
static int g_trace = 1;

void exec_set_trace(int on)
{
    g_trace = on;
}

/* 分支大类：bit27-25 = 101 */
#define ARM_OP_BRANCH 0x5u
#define BRANCH_OFFSET_MINUS1 0xFFFFFEu  /* 24 位立即数全 1：跳 0 步（跳自己） */

/* 3b.1 条件码判断：按 CPSR 标志判断条件 cond(bit31-28) 是否成立。
   15 种条件全支持，供条件执行使用。 */
int cond_ok(const arm_cpu_t *cpu, unsigned cond)
{
    const uint32_t c = cpu->cpsr;
    const unsigned n = (c >> 31) & 1u, z = (c >> 30) & 1u,
                   cy = (c >> 29) & 1u, v = (c >> 28) & 1u;
    switch (cond) {
    case 0x0: return z == 1;                 /* EQ  等于（Z=1） */
    case 0x1: return z == 0;                 /* NE  不等（Z=0） */
    case 0x2: return cy == 1;                /* CS  进位（C=1） */
    case 0x3: return cy == 0;                /* CC  无进位（C=0） */
    case 0x4: return n == 1;                 /* MI  负（N=1） */
    case 0x5: return n == 0;                 /* PL  正/零（N=0） */
    case 0x6: return v == 1;                 /* VS  溢出（V=1） */
    case 0x7: return v == 0;                 /* VC  无溢出（V=0） */
    case 0x8: return cy == 1 && z == 0;      /* HI  无符号大于 */
    case 0x9: return cy == 0 || z == 1;      /* LS  无符号小于等于 */
    case 0xA: return n == v;                 /* GE  有符号大于等于 */
    case 0xB: return n != v;                 /* LT  有符号小于 */
    case 0xC: return z == 0 && n == v;       /* GT  有符号大于 */
    case 0xD: return z == 1 || n != v;       /* LE  有符号小于等于 */
    case 0xE: return 1;                      /* AL  总是执行 */
    default:  return 1;                      /* NV（本模拟器按总是执行处理） */
    }
}

/* 设置 N/Z 标志：结果最高位 → N，结果是否为 0 → Z */
static void set_nz(uint32_t result, arm_cpu_t *cpu)
{
    if (result & 0x80000000u) cpu->cpsr |= CPSR_N; else cpu->cpsr &= ~CPSR_N;
    if (result == 0)          cpu->cpsr |= CPSR_Z; else cpu->cpsr &= ~CPSR_Z;
}

static void set_c(uint32_t v, arm_cpu_t *cpu)
{
    if (v) cpu->cpsr |= CPSR_C; else cpu->cpsr &= ~CPSR_C;
}

/* 读通用寄存器：r15 在 ARM 架构上 = 当前指令地址 + 8（三级流水线），并按 4 字节对齐。
   所有「把 r15 当普通寄存器读」的地方（字面量装载/数据运算源/基址）都必须走这里，
   否则 PC 相对寻址会少算 8 字节，读错字面量池。 */
static uint32_t read_reg(const arm_cpu_t *cpu, unsigned r)
{
    return (r == 15) ? ((cpu->r[15] & ~3u) + 8u) : cpu->r[r];
}

/* 3b.2 立即数旋转：ARM 立即数 = imm8 循环右移 (rot4*2) 位。
   这是 ARM 立即数的经典编码，能把 0x00-0xFF 旋转成其它位型。 */
static uint32_t arm_rotate(uint32_t imm8, unsigned rot4)
{
    unsigned rot = (rot4 * 2) & 31;
    if (rot == 0)
        return imm8;
    /* ROR：右移 rot 位，溢出的低位补到高位 */
    return (imm8 >> rot) | (imm8 << (32 - rot));
}

/* ---- 10.2 移位运算：type 00=LSL 01=LSR 10=ASR 11=ROR ----
   处理 amount > 0 的通用情形（amount==0 的 LSR#32/ASR#32/RRX 由调用方特判）。
   carry_in 是旧 C 标志（ROR by 32 时保持进位）。*carry_out 输出 shifter 进位。 */
static uint32_t shift_apply(uint32_t val, unsigned type, uint32_t amount,
                            uint32_t carry_in, uint32_t *carry_out)
{
    switch (type) {
    case 0: /* LSL */
        if (amount >= 32) { *carry_out = (amount == 32) ? (val & 1u) : 0u; return 0; }
        *carry_out = (val >> (32 - amount)) & 1u;
        return val << amount;
    case 1: /* LSR */
        if (amount >= 32) { *carry_out = (amount == 32) ? ((val >> 31) & 1u) : 0u; return 0; }
        *carry_out = (val >> (amount - 1)) & 1u;
        return val >> amount;
    case 2: /* ASR（算术右移，高位填符号位） */
        if (amount >= 32) {
            *carry_out = (val >> 31) & 1u;
            return (val & 0x80000000u) ? 0xFFFFFFFFu : 0u;
        }
        *carry_out = (val >> (amount - 1)) & 1u;
        return (uint32_t)((int32_t)val >> amount);
    case 3: /* ROR */
    default: {
        uint32_t a = amount & 31u;
        if (a == 0) { *carry_out = carry_in; return val; } /* ROR by 32 = 无移位 */
        *carry_out = (val >> (a - 1)) & 1u;
        return (val >> a) | (val << (32 - a));
    }
    }
}

/* operand2 解码：I=1 立即数（imm8 ROR rot4*2）；I=0 寄存器 + 可选移位。
   返回操作数值，*carry_out 输出 shifter 进位（供 S=1 时更新 C）。 */
static uint32_t decode_op2(const arm_cpu_t *cpu, uint32_t insn, uint32_t *carry_out)
{
    uint32_t c = (cpu->cpsr & CPSR_C) ? 1u : 0u;
    if (insn & (1u << 25)) { /* 立即数 */
        uint32_t imm8 = insn & 0xFFu;
        unsigned rot4 = (insn >> 8) & 0xFu;
        if (rot4 == 0) { *carry_out = c; return imm8; }
        unsigned rot = rot4 * 2;
        *carry_out = (imm8 >> (rot - 1)) & 1u;
        return arm_rotate(imm8, rot4);
    }
    uint32_t rm = read_reg(cpu, insn & 0xFu);
    unsigned type = (insn >> 5) & 3u;
    if (insn & (1u << 4)) { /* bit4=1：寄存器移位（Rs 低字节为移位量） */
        unsigned rs = (insn >> 8) & 0xFu;
        uint32_t amount = read_reg(cpu, rs) & 0xFFu;
        if (amount == 0) { *carry_out = c; return rm; }
        return shift_apply(rm, type, amount, c, carry_out);
    }
    /* bit4=0：立即数移位（移位量在 bit11-7） */
    unsigned amount = (insn >> 7) & 0x1Fu;
    if (amount == 0) {
        switch (type) {
        case 0: *carry_out = c; return rm;                             /* LSL #0 */
        case 1: *carry_out = (rm >> 31) & 1u; return 0;                /* LSR #0=#32 */
        case 2: *carry_out = (rm >> 31) & 1u;
                return (rm & 0x80000000u) ? 0xFFFFFFFFu : 0u;          /* ASR #0=#32 */
        case 3: *carry_out = rm & 1u; return (c << 31) | (rm >> 1);    /* ROR #0=RRX */
        }
    }
    return shift_apply(rm, type, amount, c, carry_out);
}

/* ---- 10.3 数据运算指令（16 种 opcode 全实现） ----
   opcode 取自 bit24-21；S 位(bit20)决定是否更新标志。
   逻辑运算（AND/EOR/ORR/BIC/MOV/MVN/TST/TEQ）S=1 时 C 来自 shifter 进位；
   算术运算（ADD/ADC/SUB/SBC/RSB/RSC/CMP/CMN）C/V 来自加减进位/溢出。 */
static void exec_dataop(arm_cpu_t *cpu, uint32_t insn, uint32_t op2, uint32_t carry)
{
    unsigned opcode = (insn >> 21) & 0xFu;
    unsigned s = (insn >> 20) & 1u;
    unsigned rn = (insn >> 16) & 0xFu;
    unsigned rd = (insn >> 12) & 0xFu;
    uint32_t a = read_reg(cpu, rn);
    uint32_t result;
    uint32_t c_in = (cpu->cpsr & CPSR_C) ? 1u : 0u;

    switch (opcode) {
    case 0x0: /* AND */ result = a & op2; if (s) { set_nz(result, cpu); set_c(carry, cpu); } cpu->r[rd] = result; break;
    case 0x1: /* EOR */ result = a ^ op2; if (s) { set_nz(result, cpu); set_c(carry, cpu); } cpu->r[rd] = result; break;
    case 0x2: /* SUB */ result = a - op2;
        if (s) { set_nz(result, cpu); set_c(a >= op2, cpu);
                 if (((a ^ op2) & (a ^ result) & 0x80000000u)) cpu->cpsr |= CPSR_V; else cpu->cpsr &= ~CPSR_V; }
        cpu->r[rd] = result; break;
    case 0x3: /* RSB（op2 - a 反向减） */ result = op2 - a;
        if (s) { set_nz(result, cpu); set_c(op2 >= a, cpu);
                 if (((op2 ^ a) & (op2 ^ result) & 0x80000000u)) cpu->cpsr |= CPSR_V; else cpu->cpsr &= ~CPSR_V; }
        cpu->r[rd] = result; break;
    case 0x4: /* ADD */ result = a + op2;
        if (s) { set_nz(result, cpu); set_c(result < a, cpu);
                 if (((a ^ result) & (op2 ^ result) & 0x80000000u)) cpu->cpsr |= CPSR_V; else cpu->cpsr &= ~CPSR_V; }
        cpu->r[rd] = result; break;
    case 0x5: { /* ADC（a + op2 + 进位） */
        uint64_t r64 = (uint64_t)a + op2 + c_in; result = (uint32_t)r64;
        if (s) { set_nz(result, cpu); set_c((r64 >> 32) & 1u, cpu);
                 if ((~(a ^ op2) & (a ^ result) & 0x80000000u)) cpu->cpsr |= CPSR_V; else cpu->cpsr &= ~CPSR_V; }
        cpu->r[rd] = result; break; }
    case 0x6: /* SBC（a - op2 - !进位） */ result = a - op2 - (c_in ? 0u : 1u);
        if (s) { set_nz(result, cpu); set_c(c_in ? a >= op2 : a > op2, cpu);
                 if (((a ^ op2) & (a ^ result) & 0x80000000u)) cpu->cpsr |= CPSR_V; else cpu->cpsr &= ~CPSR_V; }
        cpu->r[rd] = result; break;
    case 0x7: /* RSC（op2 - a - !进位） */ result = op2 - a - (c_in ? 0u : 1u);
        if (s) { set_nz(result, cpu); set_c(c_in ? op2 >= a : op2 > a, cpu);
                 if (((op2 ^ a) & (op2 ^ result) & 0x80000000u)) cpu->cpsr |= CPSR_V; else cpu->cpsr &= ~CPSR_V; }
        cpu->r[rd] = result; break;
    case 0x8: /* TST（AND，只置标志） */ result = a & op2; set_nz(result, cpu); set_c(carry, cpu); break;
    case 0x9: /* TEQ（EOR，只置标志） */ result = a ^ op2; set_nz(result, cpu); set_c(carry, cpu); break;
    case 0xA: /* CMP（SUB，只置标志） */ result = a - op2; set_nz(result, cpu); set_c(a >= op2, cpu);
        if (((a ^ op2) & (a ^ result) & 0x80000000u)) cpu->cpsr |= CPSR_V; else cpu->cpsr &= ~CPSR_V; break;
    case 0xB: /* CMN（ADD，只置标志） */ result = a + op2; set_nz(result, cpu); set_c(result < a, cpu);
        if (((a ^ result) & (op2 ^ result) & 0x80000000u)) cpu->cpsr |= CPSR_V; else cpu->cpsr &= ~CPSR_V; break;
    case 0xC: /* ORR */ result = a | op2; if (s) { set_nz(result, cpu); set_c(carry, cpu); } cpu->r[rd] = result; break;
    case 0xD: /* MOV */ if (s) { set_nz(op2, cpu); set_c(carry, cpu); } cpu->r[rd] = op2; break;
    case 0xE: /* BIC（a & ~op2） */ result = a & ~op2; if (s) { set_nz(result, cpu); set_c(carry, cpu); } cpu->r[rd] = result; break;
    case 0xF: /* MVN（~op2） */ result = ~op2; if (s) { set_nz(result, cpu); set_c(carry, cpu); } cpu->r[rd] = result; break;
    }
    /* S=1 且 Rd=PC（SUBS pc, lr, #4 / MOVS pc, lr / ADDS pc,...）：异常返回。
       真机此时不更新标志，而是把当前模式的 SPSR 拷回 CPSR（模式/中断位恢复）。 */
    if (s && rd == 15) {
        int idx = exec_spsr_index(cpu->cpsr & CPSR_MODE_MASK);
        if (idx >= 0) cpu->cpsr = cpu->spsr[idx];
    }
}

/* ---- 10.4 MRS/MSR：读/写 CPSR/SPSR ----
   只写被 field_mask 选中的字节（bit0=c 控制/bit1=x 扩展/bit2=s 状态/bit3=f 标志）。
   阶段 12.3 起允许写模式位（bit4-0），使 `MSR CPSR_c` 能切换特权模式。 */
static void msr_write(arm_cpu_t *cpu, uint32_t value, unsigned field_mask)
{
    uint32_t mask = 0;
    if (field_mask & 0x1) mask |= 0x000000FFu;
    if (field_mask & 0x2) mask |= 0x0000FF00u;
    if (field_mask & 0x4) mask |= 0x00FF0000u;
    if (field_mask & 0x8) mask |= 0xFF000000u;
    cpu->cpsr = (cpu->cpsr & ~mask) | (value & mask);
}

/* ---- 12.3 特权模式 → spsr[5] 下标 ----
   只有 FIQ/IRQ/SVC/ABT/UND 有 SPSR（各一份）；User/System 无 SPSR。 */
int exec_spsr_index(unsigned mode)
{
    switch (mode) {
    case ARM_MODE_FIQ: return 0;
    case ARM_MODE_IRQ: return 1;
    case ARM_MODE_SVC: return 2;
    case ARM_MODE_ABT: return 3;
    case ARM_MODE_UND: return 4;
    default:           return -1; /* User(0x10)/System(0x1F) 无 SPSR */
    }
}

/* ---- 12.2 异常入口 ----
   硬件动作：SPSR_<新模式> = 当前 CPSR → 写模式位 → IRQ/FIQ 置 I（FIQ 再置 F）→
   LR = PC + lr_adjust → PC = vector_base + offset。 */
void arm_exception(arm_cpu_t *cpu, uint32_t vector_offset, unsigned new_mode,
                   uint32_t lr_adjust)
{
    int idx = exec_spsr_index(new_mode);
    if (idx >= 0)
        cpu->spsr[idx] = cpu->cpsr;
    cpu->cpsr = (cpu->cpsr & ~(uint32_t)CPSR_MODE_MASK) | new_mode;
    if (new_mode == ARM_MODE_IRQ)      cpu->cpsr |= CPSR_I;      /* IRQ 入口关 IRQ */
    else if (new_mode == ARM_MODE_FIQ) cpu->cpsr |= CPSR_I | CPSR_F; /* FIQ 关 IRQ+FIQ */
    cpu->r[14] = cpu->r[15] + lr_adjust;
    cpu->r[15] = cpu->vector_base + vector_offset;
    if (g_trace)
        printf("cpu: PC=%08X exception vec=%08X mode=%02X lr=%08X cycles=%llu\n",
               cpu->r[15], cpu->vector_base + vector_offset, new_mode, cpu->r[14],
               (unsigned long long)cpu->cycles);
}

/* ---- 10.5 LDM/STM（含 PUSH/POP）：块搬移，IA/IB/DA/DB 四模式 ----
   P/U 决定寻址方向，W 写回基址；寄存器按升序访问（r0 在最低地址）。 */
static void exec_block_transfer(arm_cpu_t *cpu, uint32_t insn)
{
    unsigned p = (insn >> 24) & 1u, u = (insn >> 23) & 1u,
             s = (insn >> 22) & 1u,
             w = (insn >> 21) & 1u, l = (insn >> 20) & 1u;
    unsigned rn = (insn >> 16) & 0xFu;
    uint32_t list = insn & 0xFFFFu;
    uint32_t rn_val = read_reg(cpu, rn);

    int n = 0;
    for (int i = 0; i < 16; i++) if (list & (1u << i)) n++;

    uint32_t addr;
    if (u)      addr = p ? rn_val + 4 : rn_val;            /* IB / IA */
    else        addr = p ? rn_val - 4u * n : rn_val - 4u * (n - 1); /* DB / DA */

    for (int i = 0; i < 16; i++) {
        if (!(list & (1u << i))) continue;
        if (l) cpu->r[i] = bus_read32(cpu->nds->bus, addr);
        else   bus_write32(cpu->nds->bus, addr, cpu->r[i]);
        addr += 4;
    }
    /* 写回：无论增/减方向，最终基址 = 起始 + n*4 或 - n*4 */
    if (w) cpu->r[rn] = u ? rn_val + 4u * n : rn_val - 4u * n;
    /* LDM ... ^（S=1 且列表含 PC）：加载 PC 后，再用当前模式的 SPSR 恢复 CPSR（12.3 异常返回） */
    if (l && s && (list & (1u << 15))) {
        int idx = exec_spsr_index(cpu->cpsr & CPSR_MODE_MASK);
        if (idx >= 0) cpu->cpsr = cpu->spsr[idx];
    }
    if (g_trace) {
        /* 21-B6：LDM/STM trace 附上基址（rn=13 即 SP 变化前）与弹出 PC，
           便于 bring-up 阶段追“返回地址被污染”类问题。 */
        printf("cpu: PC=%08X insn=%08X %s r%u%s, list=%04X n=%d",
               cpu->r[15], insn, l ? "LDM" : "STM", rn, w ? "!" : "", list, n);
        if (rn == 13) printf(" base=%08X", rn_val);
        if (l && (list & (1u << 15))) printf(" pc_new=%08X", cpu->r[15]);
        printf(" cycles=%llu\n", (unsigned long long)cpu->cycles);
    }
}

/* ---- 10.6 乘法 MUL/MLA + 长乘 UMULL/UMLAL/SMULL/SMLAL ---- */
static void exec_mul(arm_cpu_t *cpu, uint32_t insn)
{
    unsigned s = (insn >> 20) & 1u, a_bit = (insn >> 21) & 1u;
    unsigned rd = (insn >> 16) & 0xFu, rn = (insn >> 12) & 0xFu,
             rs = (insn >> 8) & 0xFu, rm = insn & 0xFu;
    uint32_t result = cpu->r[rm] * cpu->r[rs] + (a_bit ? cpu->r[rn] : 0u);
    cpu->r[rd] = result;
    if (s) set_nz(result, cpu); /* C/V 乘法下无定义，不更新 */
    if (g_trace)
        printf("cpu: PC=%08X insn=%08X %s r%u, r%u, r%u%s = %08X\n",
               cpu->r[15], insn, a_bit ? "MLA" : "MUL", rd, rm, rs,
               a_bit ? "" : "", result);
}

static void exec_mul_long(arm_cpu_t *cpu, uint32_t insn)
{
    unsigned s = (insn >> 20) & 1u, u_bit = (insn >> 22) & 1u, a_bit = (insn >> 21) & 1u;
    unsigned rdhi = (insn >> 16) & 0xFu, rdlo = (insn >> 12) & 0xFu,
             rs = (insn >> 8) & 0xFu, rm = insn & 0xFu;
    uint64_t result;
    if (u_bit) result = (uint64_t)(int64_t)(int32_t)cpu->r[rm] * (int64_t)(int32_t)cpu->r[rs]; /* SMULL/SMLAL：有符号 */
    else       result = (uint64_t)cpu->r[rm] * cpu->r[rs];                                      /* UMULL/UMLAL：无符号 */
    if (a_bit) result += ((uint64_t)cpu->r[rdhi] << 32) | cpu->r[rdlo];
    cpu->r[rdlo] = (uint32_t)result;
    cpu->r[rdhi] = (uint32_t)(result >> 32);
    if (s) {
        if (cpu->r[rdhi] & 0x80000000u) cpu->cpsr |= CPSR_N; else cpu->cpsr &= ~CPSR_N;
        if (cpu->r[rdhi] == 0 && cpu->r[rdlo] == 0) cpu->cpsr |= CPSR_Z; else cpu->cpsr &= ~CPSR_Z;
    }
}

/* ---- 10.7 单数据传输 LDR/STR/LDRB/STRB（bit27-26=01） ----
   支持立即偏移/寄存器偏移（可移位）、前/后变址、W 回写、字节/字宽。 */
static void exec_single_transfer(arm_cpu_t *cpu, uint32_t insn)
{
    unsigned p = (insn >> 24) & 1u, u = (insn >> 23) & 1u,
             b = (insn >> 22) & 1u, w = (insn >> 21) & 1u, l = (insn >> 20) & 1u;
    unsigned rn = (insn >> 16) & 0xFu, rd = (insn >> 12) & 0xFu;
    uint32_t rn_val = read_reg(cpu, rn);
    uint32_t offset;

    if (insn & (1u << 25)) { /* 寄存器偏移（可移位） */
        uint32_t rm = read_reg(cpu, insn & 0xFu);
        unsigned type = (insn >> 5) & 3u;
        uint32_t amount = (insn >> 7) & 0x1Fu;
        uint32_t co;
        offset = shift_apply(rm, type, amount, 0, &co);
    } else {
        offset = insn & 0xFFFu;
    }

    uint32_t addr = u ? rn_val + offset : rn_val - offset;
    if (p == 0) addr = rn_val;      /* 后变址：先按 Rn 访存，再更新 */
    if (w || p == 0) cpu->r[rn] = u ? rn_val + offset : rn_val - offset; /* 回写 */
    if (l) {
        cpu->r[rd] = b ? bus_read8(cpu->nds->bus, addr) : bus_read32(cpu->nds->bus, addr);
        if (g_trace)
            printf("cpu: PC=%08X insn=%08X LDR%s r%u, [r%u] = %08X\n",
                   cpu->r[15], insn, b ? "B" : "", rd, rn, cpu->r[rd]);
    } else {
        if (b) bus_write8(cpu->nds->bus, addr, (uint8_t)cpu->r[rd]);
        else   bus_write32(cpu->nds->bus, addr, cpu->r[rd]);
        if (g_trace)
            printf("cpu: PC=%08X insn=%08X STR%s r%u, [r%u]\n",
                   cpu->r[15], insn, b ? "B" : "", rd, rn);
    }
}

/* ---- 10.7 额外传输 LDRH/STRH/LDRSB/LDRSH（bit27-25=000, bit7=1, bit4=1） ----
   S/H 选符号/半字，支持立即/寄存器偏移、前/后变址、W 回写。 */
static void exec_extra_transfer(arm_cpu_t *cpu, uint32_t insn)
{
    unsigned p = (insn >> 24) & 1u, u = (insn >> 23) & 1u,
             i_bit = (insn >> 22) & 1u, w = (insn >> 21) & 1u, l = (insn >> 20) & 1u;
    unsigned rn = (insn >> 16) & 0xFu, rd = (insn >> 12) & 0xFu;
    unsigned s_bit = (insn >> 6) & 1u, h_bit = (insn >> 5) & 1u;
    uint32_t rn_val = read_reg(cpu, rn);
    uint32_t offset;

    if (i_bit) offset = ((insn >> 4) & 0xF0u) | (insn & 0xFu); /* 8 位立即数拼装 */
    else       offset = read_reg(cpu, insn & 0xFu);

    uint32_t addr = u ? rn_val + offset : rn_val - offset;
    if (p == 0) addr = rn_val;
    if (w || p == 0) cpu->r[rn] = u ? rn_val + offset : rn_val - offset;

    if (l) {
        uint32_t v;
        if (h_bit)      v = bus_read16(cpu->nds->bus, addr);
        else            v = bus_read8(cpu->nds->bus, addr);
        if (s_bit) { /* 符号扩展 */
            if (h_bit) v = (uint32_t)(int32_t)(int16_t)(uint16_t)v;
            else       v = (uint32_t)(int32_t)(int8_t)(uint8_t)v;
        }
        cpu->r[rd] = v;
    } else {
        if (h_bit) bus_write16(cpu->nds->bus, addr, (uint16_t)cpu->r[rd]);
        else       bus_write8(cpu->nds->bus, addr, (uint8_t)cpu->r[rd]);
    }
    if (g_trace)
        printf("cpu: PC=%08X insn=%08X %s r%u, [r%u] = %08X\n",
               cpu->r[15], insn, l ? "LDR(x)" : "STR(x)", rd, rn, cpu->r[rd]);
}

/* ---- 10.9 SWP/SWPB：寄存器与内存交换 ---- */
static void exec_swp(arm_cpu_t *cpu, uint32_t insn)
{
    unsigned b = (insn >> 22) & 1u;
    unsigned rn = (insn >> 16) & 0xFu, rd = (insn >> 12) & 0xFu, rm = insn & 0xFu;
    uint32_t addr = read_reg(cpu, rn);
    uint32_t old = b ? bus_read8(cpu->nds->bus, addr) : bus_read32(cpu->nds->bus, addr);
    if (b) bus_write8(cpu->nds->bus, addr, (uint8_t)cpu->r[rm]);
    else   bus_write32(cpu->nds->bus, addr, cpu->r[rm]);
    cpu->r[rd] = old;
    if (g_trace)
        printf("cpu: PC=%08X insn=%08X SWP%s r%u, r%u, [r%u] = %08X\n",
               cpu->r[15], insn, b ? "B" : "", rd, rm, rn, old);
}

/* ---- 10.10/12.4/21-B8 MRC/MCR 协处理器访问（CP15） ----
   阶段 12.4：c1（系统控制寄存器）的 bit13(V) 控制异常向量基址（0=低 0x00000000，
   1=高 0xFFFF0000）。
   阶段 21-B8：FFXII 复位例程用 MCR p15,0,rX,c9,c1,0/1 配置 DTCM/ITCM，并在 c1
   打开 bit16(DTCM enable)。按子寄存器保存 c9,c1,0/1，写后同步 bus 的 ARM9
   DTCM 映射（0x027E0000 起 16KB，与 Main RAM 镜像分离）。 */
static void exec_arm9_dtcm_update(arm_cpu_t *cpu)
{
    if (cpu->is_arm7)
        return;
    if ((cpu->cp15[1] >> 16) & 1u) {
        uint32_t size = 0x200u << ((cpu->cp15_dtcm >> 1) & 0x1Fu);
        uint32_t mask;
        if (size < 0x1000u)
            size = 0x1000u;
        if (size > BUS_ARM9_DTCM_SIZE)
            size = BUS_ARM9_DTCM_SIZE;
        mask = ~(size - 1u);
        bus_set_arm9_dtcm(cpu->nds->bus, 1, cpu->cp15_dtcm & mask, size);
    } else {
        bus_set_arm9_dtcm(cpu->nds->bus, 0, 0, 0);
    }
}

static void exec_coprocessor(arm_cpu_t *cpu, uint32_t insn)
{
    unsigned l = (insn >> 20) & 1u;
    unsigned crn = (insn >> 16) & 0xFu;
    unsigned rd = (insn >> 12) & 0xFu;
    unsigned crm = insn & 0xFu;
    unsigned op1 = (insn >> 21) & 0x7u;
    unsigned op2 = (insn >> 5) & 0x7u;
    if (crn < 16) {
        if (crn == 9 && op1 == 0 && crm == 1 && (op2 == 0 || op2 == 1)) {
            /* c9,c1,0 = DTCM 配置，c9,c1,1 = ITCM 配置 */
            if (l)
                cpu->r[rd] = (op2 == 0) ? cpu->cp15_dtcm : cpu->cp15_itcm;
            else if (op2 == 0)
                cpu->cp15_dtcm = cpu->r[rd];
            else
                cpu->cp15_itcm = cpu->r[rd];
            if (!l && op2 == 0)
                exec_arm9_dtcm_update(cpu);
        } else if (l) {
            cpu->r[rd] = cpu->cp15[crn];
        } else {
            cpu->cp15[crn] = cpu->r[rd];
            if (crn == 1) {
                cpu->vector_base = (cpu->cp15[1] & (1u << 13))
                                       ? 0xFFFF0000u : 0x00000000u;
                exec_arm9_dtcm_update(cpu);
            }
        }
    }
    if (g_trace)
        printf("cpu: PC=%08X insn=%08X %s p15,0, r%u, c%u, c%u, %u\n",
               cpu->r[15], insn, l ? "MRC" : "MCR", rd, crn, crm, op2);
}

/* 分支 B/BL 目标计算：PC+8 + 符号扩展(offset24<<2)。 */
static uint32_t branch_target(uint32_t pc, uint32_t insn)
{
    uint32_t imm24 = insn & 0x00FFFFFFu;
    int32_t disp = ((int32_t)(imm24 << 8) >> 8) << 2;
    return pc + 8u + (uint32_t)disp;
}

/* 执行一条指令的全部语义。cpu_step 只负责取指与计数。 */
int exec_step(arm_cpu_t *cpu, uint32_t insn)
{
    unsigned cond = (insn >> 28) & 0xFu;

    if (!cond_ok(cpu, cond)) {
        cpu->r[15] += 4;
        return 1;
    }

    /* SWI：bit27-24=1111。24 位立即数 = 注释字段（ARM 状态函数号 = 其 >>16）。
       阶段 11 起经 BIOS HLE 拦截分发（已知号）；未知号落入 SWI 异常向量 0x08（阶段 12.2），
       给 SWI 向量留真机路径。已处理 → PC += 4；等待未满足 → PC 不动重跑（等价忙等）。 */
    if ((insn & 0x0F000000u) == 0x0F000000u) {
        cpu->swi_num = insn & 0x00FFFFFFu;
        if (g_trace)
            printf("cpu: PC=%08X insn=%08X SWI %u cycles=%llu\n",
                   cpu->r[15], insn, cpu->swi_num,
                   (unsigned long long)cpu->cycles);
        int ret = bios_dispatch(cpu->swi_num >> 16, cpu);
        if (ret == BIOS_RET_WAIT) {
            /* PC 不动，重跑本 SWI */
        } else if (ret == BIOS_RET_UNKNOWN) {
            arm_exception(cpu, EXC_SWI_OFF, ARM_MODE_SVC, 4); /* 未知号 → SWI 向量 */
        } else {
            cpu->r[15] += 4;
        }
        return 1;
    }

    /* 分支 B/BL：bit27-25 = 101 */
    if (((insn >> 25) & 0x7u) == ARM_OP_BRANCH) {
        uint32_t imm24 = insn & 0x00FFFFFFu;
        uint32_t target = branch_target(cpu->r[15], insn);
        if (insn & (1u << 24)) { /* BL */
            cpu->r[14] = cpu->r[15] + 4;
            if (g_trace)
                printf("cpu: PC=%08X insn=%08X BL %08X (lr=%08X) cycles=%llu\n",
                       cpu->r[15], insn, target, cpu->r[14],
                       (unsigned long long)cpu->cycles);
        } else if (imm24 == BRANCH_OFFSET_MINUS1) {
            if (!cpu->deadloop_reported) {
                printf("cpu: PC=%08X insn=%08X dead-loop branch to self (cycles=%llu)\n",
                       cpu->r[15], insn, (unsigned long long)cpu->cycles);
                cpu->deadloop_reported = 1;
            }
        } else if (g_trace) {
            printf("cpu: PC=%08X insn=%08X B %08X cycles=%llu\n",
                   cpu->r[15], insn, target, (unsigned long long)cpu->cycles);
        }
        cpu->r[15] = target;
        return 1;
    }

    /* LDM/STM：bit27-25 = 100 */
    if (((insn >> 25) & 0x7u) == 0x4u) {
        exec_block_transfer(cpu, insn);
        /* LDM 且列表含 PC：PC 已被内存值覆盖（如 LDM ..., {pc}^ 异常返回），不再 +4 */
        if (!((insn & (1u << 20)) && (insn & (1u << 15))))
            cpu->r[15] += 4;
        return 1;
    }

    /* BX Rm：模式 0x012FFF1x */
    if ((insn & 0x0FFFFFF0u) == 0x012FFF10u) {
        unsigned rm = insn & 0xFu;
        if (g_trace)
            printf("cpu: PC=%08X insn=%08X BX r%u -> %08X cycles=%llu\n",
                   cpu->r[15], insn, rm, cpu->r[rm],
                   (unsigned long long)cpu->cycles);
        /* ARM BX：目标 LSB=1 → 切到 Thumb（置 T），LSB=0 → 回 ARM（清 T）；
           PC 写目标并清最低位（Thumb 半字对齐、ARM 字对齐都落在同一条基线上）。 */
        cpu->cpsr = (cpu->r[rm] & 1u) ? (cpu->cpsr | CPSR_T) : (cpu->cpsr & ~CPSR_T);
        cpu->r[15] = cpu->r[rm] & ~1u;
        return 1;
    }

    /* MRS（读 CPSR/SPSR）：bit27-24=0001, bit23-21=00x, bit19-16=1111, bit11-0=0，
       Rd 在 bit15-12（不可固定，需掩掉）。 */
    if ((insn & 0x0FFF0FFFu) == 0x010F0000u) {
        unsigned rd = (insn >> 12) & 0xFu;
        cpu->r[rd] = cpu->cpsr;
        cpu->r[15] += 4;
        return 1;
    }
    if ((insn & 0x0FFF0FFFu) == 0x014F0000u) {
        unsigned rd = (insn >> 12) & 0xFu;
        /* 读当前模式的 SPSR；User/System 无 SPSR，读 0（架构未定义） */
        int idx = exec_spsr_index(cpu->cpsr & CPSR_MODE_MASK);
        cpu->r[rd] = (idx >= 0) ? cpu->spsr[idx] : 0u;
        cpu->r[15] += 4;
        return 1;
    }

    /* MSR（写 CPSR/SPSR，寄存器形式）：bit22=R（0=CPSR,1=SPSR）需保留，勿固定为 0。 */
    if ((insn & 0x0FB0FFF0u) == 0x0120F000u) {
        unsigned rm = insn & 0xFu;
        unsigned field = (insn >> 16) & 0xFu;
        if (insn & (1u << 22)) {
            int idx = exec_spsr_index(cpu->cpsr & CPSR_MODE_MASK);
            if (idx >= 0) cpu->spsr[idx] = cpu->r[rm];
        } else {
            msr_write(cpu, cpu->r[rm], field);
        }
        cpu->r[15] += 4;
        return 1;
    }
    /* MSR（写 CPSR/SPSR，立即数形式）：bit22=R（0=CPSR,1=SPSR）需保留，勿固定为 0。 */
    if ((insn & 0x0FB0F000u) == 0x0320F000u) {
        uint32_t imm8 = insn & 0xFFu;
        unsigned rot4 = (insn >> 8) & 0xFu;
        uint32_t value = arm_rotate(imm8, rot4);
        unsigned field = (insn >> 16) & 0xFu;
        if (insn & (1u << 22)) {
            int idx = exec_spsr_index(cpu->cpsr & CPSR_MODE_MASK);
            if (idx >= 0) cpu->spsr[idx] = value;
        } else {
            msr_write(cpu, value, field);
        }
        cpu->r[15] += 4;
        return 1;
    }

    /* 乘法 MUL/MLA */
    if ((insn & 0x0FC000F0u) == 0x00000090u) {
        exec_mul(cpu, insn);
        cpu->r[15] += 4;
        return 1;
    }
    /* 长乘 UMULL/UMLAL/SMULL/SMLAL */
    if ((insn & 0x0F8000F0u) == 0x00800090u) {
        exec_mul_long(cpu, insn);
        cpu->r[15] += 4;
        return 1;
    }

    /* SWP/SWPB */
    if ((insn & 0x0FB00FF0u) == 0x01000090u) {
        exec_swp(cpu, insn);
        cpu->r[15] += 4;
        return 1;
    }

    /* 额外传输 LDRH/STRH/LDRSB/LDRSH */
    if ((insn & 0x0E000090u) == 0x00000090u) {
        exec_extra_transfer(cpu, insn);
        /* LDR 且 Rd=PC：PC 已被内存值覆盖，不再 +4 */
        if (!((insn & (1u << 20)) && (((insn >> 12) & 0xFu) == 15)))
            cpu->r[15] += 4;
        return 1;
    }

    /* 单数据传输 LDR/STR/LDRB/STRB：bit27-26 = 01 */
    if (((insn >> 26) & 0x3u) == 1) {
        exec_single_transfer(cpu, insn);
        /* LDR 且 Rd=PC（如 LDR pc, [sp], #4 返回惯用法）：PC 已被覆盖，不再 +4 */
        if (!((insn & (1u << 20)) && (((insn >> 12) & 0xFu) == 15)))
            cpu->r[15] += 4;
        return 1;
    }

    /* 协处理器 MRC/MCR：bit27-24 = 1110, bit4 = 1 */
    if ((insn & 0x0F000010u) == 0x0E000010u) {
        exec_coprocessor(cpu, insn);
        cpu->r[15] += 4;
        return 1;
    }

    /* 数据运算：bit27-26 = 00（MRS/MSR/乘法/额外传输/SWP 已在上方拦截） */
    if (((insn >> 26) & 0x3u) == 0) {
        uint32_t carry = 0;
        uint32_t op2 = decode_op2(cpu, insn, &carry);
        exec_dataop(cpu, insn, op2, carry);
        /* Rd=15：结果是写 PC（如 SUBS pc, lr, #4 / MOVS pc, lr 异常返回），不再 +4 */
        if (((insn >> 12) & 0xFu) != 15)
            cpu->r[15] += 4;
        return 1;
    }

    /* 其余未实现指令：触发未定义指令异常（12.2），跳到向量 0x04（不再打印后继续）。 */
    if (g_trace || cpu->nds->bus->diag)
        printf("cpu: PC=%08X insn=%08X undefined (cycles=%llu)\n",
               cpu->r[15], insn, (unsigned long long)cpu->cycles);
    arm_exception(cpu, EXC_UNDEF_OFF, ARM_MODE_UND, 4);
    return 1;
}
