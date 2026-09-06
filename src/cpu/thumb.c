/* 阶段 13：Thumb 指令集（16 位）。
   真机游戏主体代码几乎全是 Thumb：更省 ROM，代价是寄存器/立即数受限。
   本文件实现 Thumb 译码与全部 Thumb 指令语义，由 cpu_step 在 CPSR.T=1 时调用。
   PC 约定：Thumb 读 PC = 当前指令地址 + 4（流水线）；PC 相对寻址基址需字对齐 (pc&~3)。 */

#include <stdint.h>
#include <stdio.h>

#include "thumb.h"
#include "cpu.h"
#include "exec.h"
#include "bus/bus.h"
#include "bios/bios.h"

/* 指令级跟踪开关（thumb_set_trace 控制） */
static int g_trace = 1;

void thumb_set_trace(int on)
{
    g_trace = on;
}

/* ---- 标志更新（N/Z/C）与移位（带进位） ---- */
static void set_nz(uint32_t r, arm_cpu_t *cpu)
{
    cpu->cpsr = (r & 0x80000000u) ? (cpu->cpsr | CPSR_N) : (cpu->cpsr & ~CPSR_N);
    cpu->cpsr = (r == 0) ? (cpu->cpsr | CPSR_Z) : (cpu->cpsr & ~CPSR_Z);
}
static void set_c(int v, arm_cpu_t *cpu)
{
    cpu->cpsr = v ? (cpu->cpsr | CPSR_C) : (cpu->cpsr & ~CPSR_C);
}
static void set_v_add(uint32_t a, uint32_t b, uint32_t r, arm_cpu_t *cpu)
{
    int v = (((a ^ r) & (b ^ r)) >> 31) & 1;
    cpu->cpsr = v ? (cpu->cpsr | CPSR_V) : (cpu->cpsr & ~CPSR_V);
}
static void set_v_sub(uint32_t a, uint32_t b, uint32_t r, arm_cpu_t *cpu)
{
    int v = (((a ^ b) & (a ^ r)) >> 31) & 1;
    cpu->cpsr = v ? (cpu->cpsr | CPSR_V) : (cpu->cpsr & ~CPSR_V);
}

/* 移位：*carry 返回移出的最后一位（n=0 时 LSL 不改 C，返回 -1 表示保持）。 */
static uint32_t shift_lsl(uint32_t v, unsigned n, int *carry)
{
    if (n == 0) { *carry = -1; return v; }
    if (n >= 32) { *carry = n == 32 ? (v & 1u) : 0; return 0; }
    *carry = (v >> (32 - n)) & 1u;
    return v << n;
}
static uint32_t shift_lsr(uint32_t v, unsigned n, int *carry)
{
    if (n == 0) n = 32;               /* LSR #0 = LSR #32 */
    if (n >= 32) { *carry = n == 32 ? ((v >> 31) & 1u) : 0; return 0; }
    *carry = (v >> (n - 1)) & 1u;
    return v >> n;
}
static uint32_t shift_asr(uint32_t v, unsigned n, int *carry)
{
    if (n == 0 || n >= 32) { *carry = (v >> 31) & 1u; return (v & 0x80000000u) ? 0xFFFFFFFFu : 0u; }
    *carry = (v >> (n - 1)) & 1u;
    return (uint32_t)((int32_t)v >> n);
}
static uint32_t shift_ror(uint32_t v, unsigned n, int *carry)
{
    if (n == 0) { *carry = -1; return v; } /* ROR #0 无操作 */
    n &= 31;
    if (n == 0) { *carry = (v >> 31) & 1u; return v; }
    *carry = (v >> (n - 1)) & 1u;
    return (v >> n) | (v << (32 - n));
}

/* 执行一条 Thumb 指令。默认推进 PC+=2；分支/BX/BL 显式写 PC。 */
int thumb_step(arm_cpu_t *cpu, uint16_t insn)
{
    uint32_t pc = cpu->r[15] + 4;      /* Thumb 读 PC 的流水线值 */
    uint32_t pc_word = pc & ~3u;       /* PC 相对寻址的字对齐基址 */
    unsigned rd, rs, rn, rm, imm, op;

    /* 逐条 trace（21-B5）：bring-up 阶段普通 Thumb 指令也要能看见，
       否则无法定位真 ROM 在 Thumb 段内跳到哪一步出错。 */
    if (g_trace)
        printf("thumb: PC=%08X insn=%04X\n", cpu->r[15], insn);

    /* ---- 格式 1/2：bits15-13 = 000：移位 / 加减 ---- */
    if ((insn >> 13) == 0) {
        op = (insn >> 11) & 3u;
        if (op == 3) {
            /* 格式 2：ADD/SUB Rd, Rs, Rn 或 #imm3 */
            unsigned i = (insn >> 10) & 1u;
            unsigned sub = (insn >> 9) & 1u;
            rs = (insn >> 3) & 7u; rd = insn & 7u;
            uint32_t a = cpu->r[rs];
            uint32_t b = i ? ((insn >> 6) & 7u) : cpu->r[(insn >> 6) & 7u];
            uint32_t r = sub ? a - b : a + b;
            cpu->r[rd] = r;
            set_nz(r, cpu);
            if (sub) { set_c(a >= b, cpu); set_v_sub(a, b, r, cpu); }
            else     { set_c(r < a, cpu); set_v_add(a, b, r, cpu); }
        } else {
            /* 格式 1：LSL/LSR/ASR Rd, Rs, #imm5 */
            imm = (insn >> 6) & 0x1Fu;
            rs = (insn >> 3) & 7u; rd = insn & 7u;
            uint32_t v = cpu->r[rs];
            int c;
            uint32_t r = op == 0 ? shift_lsl(v, imm, &c)
                       : op == 1 ? shift_lsr(v, imm, &c)
                       :           shift_asr(v, imm, &c);
            cpu->r[rd] = r;
            set_nz(r, cpu);
            if (c >= 0) set_c(c, cpu);
        }
        cpu->r[15] += 2;
        return 1;
    }

    /* ---- 格式 3：bits15-13 = 001：MOV/CMP/ADD/SUB Rd, #imm8 ---- */
    if ((insn >> 13) == 1) {
        op = (insn >> 11) & 3u;
        rd = (insn >> 8) & 7u;
        imm = insn & 0xFFu;
        uint32_t a = cpu->r[rd];
        if (op == 0) { cpu->r[rd] = imm; set_nz(imm, cpu); }                    /* MOV */
        else if (op == 1) { set_nz(a - imm, cpu); set_c(a >= imm, cpu); set_v_sub(a, imm, a - imm, cpu); } /* CMP */
        else if (op == 2) { uint32_t r = a + imm; cpu->r[rd] = r; set_nz(r, cpu); set_c(r < a, cpu); set_v_add(a, imm, r, cpu); } /* ADD */
        else             { uint32_t r = a - imm; cpu->r[rd] = r; set_nz(r, cpu); set_c(a >= imm, cpu); set_v_sub(a, imm, r, cpu); } /* SUB */
        cpu->r[15] += 2;
        return 1;
    }

    /* ---- 格式 4：bits15-10 = 010000：ALU（全部更新标志） ---- */
    if ((insn >> 10) == 0x10u) {
        op = (insn >> 6) & 0xFu;
        rs = (insn >> 3) & 7u; rd = insn & 7u;
        uint32_t a = cpu->r[rd], b = cpu->r[rs];
        uint32_t c_in = (cpu->cpsr & CPSR_C) ? 1u : 0u;
        int c;
        switch (op) {
        case 0x0: cpu->r[rd] = a & b; set_nz(a & b, cpu); break;                    /* AND */
        case 0x1: cpu->r[rd] = a ^ b; set_nz(a ^ b, cpu); break;                    /* EOR */
        case 0x2: { uint32_t r = shift_lsl(a, b & 0xFFu, &c); cpu->r[rd] = r; set_nz(r, cpu); if (c >= 0) set_c(c, cpu); break; } /* LSL */
        case 0x3: { uint32_t r = shift_lsr(a, b & 0xFFu, &c); cpu->r[rd] = r; set_nz(r, cpu); if (c >= 0) set_c(c, cpu); break; } /* LSR */
        case 0x4: { uint32_t r = shift_asr(a, b & 0xFFu, &c); cpu->r[rd] = r; set_nz(r, cpu); if (c >= 0) set_c(c, cpu); break; } /* ASR */
        case 0x5: { uint64_t r64 = (uint64_t)a + b + c_in; uint32_t r = (uint32_t)r64;
                    cpu->r[rd] = r; set_nz(r, cpu); set_c((r64 >> 32) & 1u, cpu); set_v_add(a, b, r, cpu); break; } /* ADC */
        case 0x6: { uint32_t r = a - b - (c_in ? 0u : 1u);
                    cpu->r[rd] = r; set_nz(r, cpu); set_c(c_in ? a >= b : a > b, cpu); set_v_sub(a, b, r, cpu); break; } /* SBC */
        case 0x7: { uint32_t r = shift_ror(a, b & 0xFFu, &c); cpu->r[rd] = r; set_nz(r, cpu); if (c >= 0) set_c(c, cpu); break; } /* ROR */
        case 0x8: set_nz(a & b, cpu); break;                                       /* TST */
        case 0x9: { uint32_t r = 0 - b; cpu->r[rd] = r; set_nz(r, cpu); set_c(b != 0, cpu); set_v_sub(0, b, r, cpu); break; } /* NEG */
        case 0xA: set_nz(a - b, cpu); set_c(a >= b, cpu); set_v_sub(a, b, a - b, cpu); break; /* CMP */
        case 0xB: { uint32_t r = a + b; set_nz(r, cpu); set_c(r < a, cpu); set_v_add(a, b, r, cpu); break; } /* CMN */
        case 0xC: cpu->r[rd] = a | b; set_nz(a | b, cpu); break;                    /* ORR */
        case 0xD: cpu->r[rd] = a * b; set_nz(a * b, cpu); break;                    /* MUL */
        case 0xE: cpu->r[rd] = a & ~b; set_nz(a & ~b, cpu); break;                  /* BIC */
        case 0xF: cpu->r[rd] = ~b; set_nz(~b, cpu); break;                          /* MVN */
        }
        cpu->r[15] += 2;
        return 1;
    }

    /* ---- 格式 5：bits15-10 = 010001：高寄存器 / BX / BLX ---- */
    if ((insn >> 10) == 0x11u) {
        op = (insn >> 8) & 3u;
        if (op == 3) {
            /* BX/BLX Rm：寄存器号在 bit6:3（如 BX lr=0x4770 → Rm=14），
               低 3 位固定为 0；bit7=0 BX / 1 BLX。T=Rm[0]，PC=Rm&~1；
               BLX 先 lr=当前 pc|1。 */
            unsigned bx_rm = (insn >> 3) & 0xFu;
            if (insn & (1u << 7)) /* BLX */
                cpu->r[14] = (pc | 1u);
            cpu->cpsr = (cpu->r[bx_rm] & 1u) ? (cpu->cpsr | CPSR_T) : (cpu->cpsr & ~CPSR_T);
            cpu->r[15] = cpu->r[bx_rm] & ~1u;
            return 1;
        }
        /* ADD/CMP/MOV Rd, Rm（可含高寄存器）：Rd=bit7:bits2-0，Rm=bit6:bits5-3 */
        rm = ((insn >> 3) & 7u) | ((insn >> 6) & 1u ? 8u : 0u);
        rd = (insn & 7u) | ((insn >> 7) & 1u ? 8u : 0u);
        if (op == 0) { cpu->r[rd] += cpu->r[rm]; if (rd == 15) cpu->r[15] &= ~1u; } /* ADD */
        else if (op == 1) { set_nz(cpu->r[rd] - cpu->r[rm], cpu); set_c(cpu->r[rd] >= cpu->r[rm], cpu); set_v_sub(cpu->r[rd], cpu->r[rm], cpu->r[rd] - cpu->r[rm], cpu); } /* CMP */
        else { cpu->r[rd] = cpu->r[rm]; if (rd == 15) cpu->r[15] &= ~1u; }          /* MOV */
        if (op != 1 && rd == 15) return 1; /* ADD/MOV 写 PC：已跳转，不再 +2 */
        cpu->r[15] += 2;
        return 1;
    }

    /* ---- 格式 6：bits15-11 = 01001：LDR Rd, [PC, #imm8*4] ---- */
    if ((insn >> 11) == 0x09u) {
        rd = (insn >> 8) & 7u;
        cpu->r[rd] = bus_read32(cpu->nds->bus, pc_word + ((insn & 0xFFu) << 2));
        cpu->r[15] += 2;
        return 1;
    }

    /* ---- 格式 7：bits15-12 = 0101：寄存器偏移访存 ---- */
    if ((insn >> 12) == 0x5u) {
        op = (insn >> 9) & 7u;
        rm = (insn >> 6) & 7u; rn = (insn >> 3) & 7u; rd = insn & 7u;
        uint32_t addr = cpu->r[rn] + cpu->r[rm];
        switch (op) {
        case 0: bus_write32(cpu->nds->bus, addr, cpu->r[rd]); break;                    /* STR */
        case 1: bus_write16(cpu->nds->bus, addr, (uint16_t)cpu->r[rd]); break;          /* STRH */
        case 2: bus_write8(cpu->nds->bus, addr, (uint8_t)cpu->r[rd]); break;            /* STRB */
        case 3: cpu->r[rd] = (uint32_t)(int32_t)(int8_t)bus_read8(cpu->nds->bus, addr); break; /* LDRSB */
        case 4: cpu->r[rd] = bus_read32(cpu->nds->bus, addr); break;                    /* LDR */
        case 5: cpu->r[rd] = bus_read16(cpu->nds->bus, addr); break;                    /* LDRH */
        case 6: cpu->r[rd] = bus_read8(cpu->nds->bus, addr); break;                     /* LDRB */
        case 7: cpu->r[rd] = (uint32_t)(int32_t)(int16_t)bus_read16(cpu->nds->bus, addr); break; /* LDRSH */
        }
        cpu->r[15] += 2;
        return 1;
    }

    /* ---- 格式 8：bits15-13 = 011：立即偏移访存（字/字节） ---- */
    if ((insn >> 13) == 0x3u) {
        unsigned b = (insn >> 12) & 1u, l = (insn >> 11) & 1u;
        imm = (insn >> 6) & 0x1Fu; rn = (insn >> 3) & 7u; rd = insn & 7u;
        uint32_t addr = cpu->r[rn] + (b ? imm : imm << 2);
        if (l) cpu->r[rd] = b ? bus_read8(cpu->nds->bus, addr) : bus_read32(cpu->nds->bus, addr);
        else if (b) bus_write8(cpu->nds->bus, addr, (uint8_t)cpu->r[rd]);
        else        bus_write32(cpu->nds->bus, addr, cpu->r[rd]);
        cpu->r[15] += 2;
        return 1;
    }

    /* ---- 格式 9：bits15-12 = 1000：半字访存 STRH/LDRH, #imm5*2 ---- */
    if ((insn >> 12) == 0x8u) {
        unsigned l = (insn >> 11) & 1u;
        imm = (insn >> 6) & 0x1Fu; rn = (insn >> 3) & 7u; rd = insn & 7u;
        uint32_t addr = cpu->r[rn] + (imm << 1);
        if (l) cpu->r[rd] = bus_read16(cpu->nds->bus, addr);
        else   bus_write16(cpu->nds->bus, addr, (uint16_t)cpu->r[rd]);
        cpu->r[15] += 2;
        return 1;
    }

    /* ---- 格式 10：bits15-12 = 1001：SP 相对访存 STR/LDR, #imm8*4 ---- */
    if ((insn >> 12) == 0x9u) {
        unsigned l = (insn >> 11) & 1u;
        rd = (insn >> 8) & 7u;
        uint32_t addr = cpu->r[13] + ((insn & 0xFFu) << 2);
        if (l) cpu->r[rd] = bus_read32(cpu->nds->bus, addr);
        else   bus_write32(cpu->nds->bus, addr, cpu->r[rd]);
        cpu->r[15] += 2;
        return 1;
    }

    /* ---- 格式 11：bits15-12 = 1010：取地址 ADD Rd, PC/SP, #imm8*4 ---- */
    if ((insn >> 12) == 0xAu) {
        unsigned sp = (insn >> 11) & 1u;
        rd = (insn >> 8) & 7u;
        uint32_t base = sp ? cpu->r[13] : pc_word;
        cpu->r[rd] = base + ((insn & 0xFFu) << 2);
        cpu->r[15] += 2;
        return 1;
    }

    /* ---- 格式 12：bits15-8 = 10110000：ADD/SUB SP, #imm7*4 ---- */
    if ((insn >> 8) == 0xB0u) {
        unsigned sub = (insn >> 7) & 1u;
        uint32_t off = (insn & 0x7Fu) << 2;
        cpu->r[13] = sub ? cpu->r[13] - off : cpu->r[13] + off;
        cpu->r[15] += 2;
        return 1;
    }

    /* ---- 格式 13：bits15-12 = 1011：PUSH/POP ---- */
    if ((insn >> 12) == 0xBu) {
        unsigned l = (insn >> 11) & 1u;
        uint32_t list = insn & 0xFFu;
        unsigned rbit = (insn >> 8) & 1u;   /* PUSH 的 LR / POP 的 PC */
        unsigned n = 0; for (unsigned i = 0; i < 8; i++) if (list & (1u << i)) n++;
        if (rbit) n++;
        uint32_t addr;
        if (l) { /* POP（LDMIA sp!） */
            addr = cpu->r[13];
            for (unsigned i = 0; i < 8; i++) if (list & (1u << i)) { cpu->r[i] = bus_read32(cpu->nds->bus, addr); addr += 4; }
            if (rbit) { cpu->r[15] = bus_read32(cpu->nds->bus, addr) & ~1u; addr += 4; }
            cpu->r[13] += n * 4;
            if (rbit) return 1;             /* POP 含 PC：已跳转 */
        } else { /* PUSH（STMDB sp!） */
            addr = cpu->r[13] - n * 4;
            uint32_t a = addr;
            for (unsigned i = 0; i < 8; i++) if (list & (1u << i)) { bus_write32(cpu->nds->bus, a, cpu->r[i]); a += 4; }
            if (rbit) { bus_write32(cpu->nds->bus, a, cpu->r[14]); a += 4; }
            cpu->r[13] = addr;
        }
        cpu->r[15] += 2;
        return 1;
    }

    /* ---- 格式 14：bits15-12 = 1100：STMIA/LDMIA ---- */
    if ((insn >> 12) == 0xCu) {
        unsigned l = (insn >> 11) & 1u;
        unsigned rb = (insn >> 8) & 7u;
        uint32_t list = insn & 0xFFu;
        unsigned n = 0; for (unsigned i = 0; i < 8; i++) if (list & (1u << i)) n++;
        uint32_t addr = cpu->r[rb];
        for (unsigned i = 0; i < 8; i++) {
            if (!(list & (1u << i))) continue;
            if (l) cpu->r[i] = bus_read32(cpu->nds->bus, addr);
            else   bus_write32(cpu->nds->bus, addr, cpu->r[i]);
            addr += 4;
        }
        cpu->r[rb] += n * 4;    /* 写回 */
        cpu->r[15] += 2;
        return 1;
    }

    /* ---- 格式 15：bits15-12 = 1101：条件分支（11011111 = SWI） ---- */
    if ((insn >> 12) == 0xDu) {
        if ((insn & 0xFF00u) == 0xDF00u) {
            /* SWI imm8：经 BIOS HLE 分发（函数号 = imm8） */
            unsigned fn = insn & 0xFFu;
            cpu->swi_num = fn;
            int ret = bios_dispatch(fn, cpu);
            if (ret == BIOS_RET_WAIT) return 1;
            if (ret == BIOS_RET_REDIR) return 1;
            if (ret == BIOS_RET_UNKNOWN) { arm_exception(cpu, EXC_SWI_OFF, ARM_MODE_SVC, 4); return 1; }
            cpu->r[15] += 2;
            return 1;
        }
        unsigned cond = (insn >> 8) & 0xFu;
        int32_t disp = ((int32_t)(int8_t)(insn & 0xFFu)) << 1;
        if (cond_ok(cpu, cond)) cpu->r[15] = pc + (uint32_t)disp;
        else cpu->r[15] += 2;
        return 1;
    }

    /* ---- 格式 17：bits15-11 = 11100：无条件分支 B ---- */
    if ((insn >> 11) == 0x1Cu) {
        int32_t disp = ((int32_t)(insn & 0x7FFu) << 21) >> 20; /* 符号扩展 11 位再 <<1 */
        cpu->r[15] = pc + (uint32_t)disp;
        return 1;
    }

    /* ---- 格式 18：bits15-11 = 1111x：BL/BLX 长跳转 ---- */
    {
        unsigned top = insn >> 11;   /* 11110 = 第一半字 / 11111 = BL 第二 / 11101 = BLX 第二 */
        if (top == 0x1Eu) {
            /* 第一半字：LR = pc + (符号扩展 offset11 << 12) */
            int32_t off = ((int32_t)(insn & 0x7FFu) << 21) >> 9;
            cpu->r[14] = pc + (uint32_t)off;
            cpu->r[15] += 2;
            return 1;
        } else if (top == 0x1Fu) {
            /* BL 第二半字：PC = LR + (offset11 << 1)；LR = 第二半字 PC | 1；保持 Thumb */
            uint32_t target = cpu->r[14] + ((insn & 0x7FFu) << 1);
            cpu->r[14] = (pc | 1u);
            cpu->r[15] = target;
            return 1;
        } else if (top == 0x1Du) {
            /* BLX 第二半字：PC = LR + (offset11 << 1)；LR = 第二半字 PC | 1；切 ARM(T=0) */
            uint32_t target = (cpu->r[14] + ((insn & 0x7FFu) << 1)) & ~1u;
            cpu->r[14] = (pc | 1u);
            cpu->cpsr &= ~CPSR_T;
            cpu->r[15] = target;
            return 1;
        }
    }

    /* 未实现 Thumb 指令：触发未定义异常。 */
    if (g_trace || cpu->nds->bus->diag)
        printf("cpu: PC=%08X thumb insn=%04X undefined (cycles=%llu)\n",
               cpu->r[15], insn, (unsigned long long)cpu->cycles);
    arm_exception(cpu, EXC_UNDEF_OFF, ARM_MODE_UND, 4);
    return 1;
}
