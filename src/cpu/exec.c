#include <stdio.h>
#include "exec.h"
#include "bus/bus.h"

/* 分支大类：bit27-25 = 101 */
#define ARM_OP_BRANCH 0x5u
#define BRANCH_OFFSET_MINUS1 0xFFFFFEu  /* 24 位立即数全 1：跳 0 步（跳自己） */

/* 3b.1 条件码判断：按 CPSR 标志判断条件 cond(bit31-28) 是否成立。
   15 种条件全支持，供条件执行使用。 */
static int cond_ok(const arm_cpu_t *cpu, unsigned cond)
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

/* 加法进位/溢出：
   C = 无符号最高位进位，等价于 result < a（a+b 溢出回绕）。
   V = 有符号溢出：a、b 同号而 result 异号（同号相加越界）。 */
static void set_carry_add(uint32_t a, uint32_t b, uint32_t result, arm_cpu_t *cpu)
{
    if (result < a) cpu->cpsr |= CPSR_C; else cpu->cpsr &= ~CPSR_C;
    if (((a ^ result) & (b ^ result) & 0x80000000u) != 0)
        cpu->cpsr |= CPSR_V; else cpu->cpsr &= ~CPSR_V;
}

/* 减法进位/溢出：
   C = 无借位，即 a >= b。
   V = 有符号溢出：a、b 异号 且 a、result 异号（小减大越过符号界）。 */
static void set_carry_sub(uint32_t a, uint32_t b, uint32_t result, arm_cpu_t *cpu)
{
    if (a >= b) cpu->cpsr |= CPSR_C; else cpu->cpsr &= ~CPSR_C;
    if (((a ^ b) & (a ^ result) & 0x80000000u) != 0)
        cpu->cpsr |= CPSR_V; else cpu->cpsr &= ~CPSR_V;
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

/* 数据运算 operand2 解码：
   I=1（bit25）→ 立即数（imm8 旋转）；I=0 → 寄存器 Rm（本阶段忽略寄存器移位）。 */
static uint32_t decode_op2(const arm_cpu_t *cpu, uint32_t insn)
{
    if (insn & (1u << 25)) {
        uint32_t imm8 = insn & 0xFFu;
        unsigned rot4 = (insn >> 8) & 0xFu;
        return arm_rotate(imm8, rot4);
    }
    return cpu->r[insn & 0xFu];
}

/* 3b.2/3b.3/3b.4 数据运算指令（MOV/ADD/SUB/CMP）。
   opcode 取自 bit24-21；S 位(bit20)决定是否更新标志。
   CMP 与 SUB 算法相同但只写标志不写寄存器。 */
static void exec_dataop(arm_cpu_t *cpu, uint32_t insn, uint32_t op2)
{
    unsigned opcode = (insn >> 21) & 0xFu;
    unsigned s = (insn >> 20) & 1u;
    unsigned rn = (insn >> 16) & 0xFu;
    unsigned rd = (insn >> 12) & 0xFu;
    uint32_t rn_val = cpu->r[rn];

    switch (opcode) {
    case 0x0: /* AND：按位与 */
    {
        uint32_t result = rn_val & op2;
        if (s) set_nz(result, cpu);
        cpu->r[rd] = result;
        break;
    }
    case 0x1: /* EOR：按位异或 */
    {
        uint32_t result = rn_val ^ op2;
        if (s) set_nz(result, cpu);
        cpu->r[rd] = result;
        break;
    }
    case 0x2: /* SUB */
    {
        uint32_t result = rn_val - op2;
        if (s) { set_nz(result, cpu); set_carry_sub(rn_val, op2, result, cpu); }
        cpu->r[rd] = result;
        break;
    }
    case 0x4: /* ADD */
    {
        uint32_t result = rn_val + op2;
        if (s) { set_nz(result, cpu); set_carry_add(rn_val, op2, result, cpu); }
        cpu->r[rd] = result;
        break;
    }
    case 0xC: /* ORR：按位或 */
    {
        uint32_t result = rn_val | op2;
        if (s) set_nz(result, cpu);
        cpu->r[rd] = result;
        break;
    }
    case 0xD: /* MOV：直接写入操作数 */
        if (s) set_nz(op2, cpu);
        cpu->r[rd] = op2;
        break;
    case 0xA: /* CMP：Rn - op2，只更新标志 */
    {
        uint32_t result = rn_val - op2;
        set_nz(result, cpu);
        set_carry_sub(rn_val, op2, result, cpu);
        break;
    }
    default:
        printf("cpu: PC=%08X insn=%08X dataop 0x%X unimplemented\n",
               cpu->r[15], insn, opcode);
        break;
    }
}

/* 3b.7 分支 B/BL 目标计算：PC+8 + 符号扩展(offset24<<2)。
   这是 ARM 流水线的 PC+8 语义，单独成函数便于分支与 BL 复用。 */
static uint32_t branch_target(uint32_t pc, uint32_t insn)
{
    uint32_t imm24 = insn & 0x00FFFFFFu;
    /* 24 位立即数符号扩展成字节位移：
       imm24<<8 先左移（uint32），转 int32 后算术右移 8 位得符号扩展值，
       再左移 2 位（立即数单位是「字」，1 字 = 4 字节）。
       注意 cast 必须在右移之前，否则无符号右移会丢掉符号位。 */
    int32_t disp = ((int32_t)(imm24 << 8) >> 8) << 2;
    return pc + 8u + (uint32_t)disp;
}

/* 执行一条指令的全部语义。cpu_step 只负责取指与计数，此处处理：
   条件判断 → 分支/BL/BX → 数据运算 → LDR/STR → 未实现打印。
   所有分支路径都会正确推进或改写 PC。 */
int exec_step(arm_cpu_t *cpu, uint32_t insn)
{
    unsigned cond = (insn >> 28) & 0xFu;

    /* 3b.1 条件执行：条件不成立则跳过本指令（不产生任何副作用，仅 PC+4） */
    if (!cond_ok(cpu, cond)) {
        cpu->r[15] += 4;
        return 1;
    }

    /* 分支 B/BL：bit27-25 = 101。BL 额外把返回地址 PC+4 存进 lr(r14)。 */
    if (((insn >> 25) & 0x7u) == ARM_OP_BRANCH) {
        uint32_t imm24 = insn & 0x00FFFFFFu;
        uint32_t target = branch_target(cpu->r[15], insn);
        if (insn & (1u << 24)) { /* BL */
            cpu->r[14] = cpu->r[15] + 4; /* lr = 下一条指令地址 */
            printf("cpu: PC=%08X insn=%08X BL %08X (lr=%08X) cycles=%llu\n",
                   cpu->r[15], insn, target, cpu->r[14],
                   (unsigned long long)cpu->cycles);
        } else if (imm24 == BRANCH_OFFSET_MINUS1) {
            /* B 自己（死循环）：目标 = PC+8-8 = PC，原地打转。
               只在第一次命中时打印，避免主循环每帧刷屏。 */
            if (!cpu->deadloop_reported) {
                printf("cpu: PC=%08X insn=%08X dead-loop branch to self (cycles=%llu)\n",
                       cpu->r[15], insn, (unsigned long long)cpu->cycles);
                cpu->deadloop_reported = 1;
            }
        } else {
            printf("cpu: PC=%08X insn=%08X B %08X cycles=%llu\n",
                   cpu->r[15], insn, target, (unsigned long long)cpu->cycles);
        }
        cpu->r[15] = target;
        return 1;
    }

    /* BX Rm：模式 0x012FFF1x。跳转到 Rm 的值（本阶段只用于 BX lr 返回）。
       必须放在数据运算判定之前：BX 的 bit27-26 也是 00。 */
    if ((insn & 0x0FFFFFF0u) == 0x012FFF10u) {
        unsigned rm = insn & 0xFu;
        printf("cpu: PC=%08X insn=%08X BX r%u -> %08X cycles=%llu\n",
               cpu->r[15], insn, rm, cpu->r[rm],
               (unsigned long long)cpu->cycles);
        cpu->r[15] = cpu->r[rm];
        return 1;
    }

    /* 数据运算（MOV/ADD/SUB/CMP 等）：bit27-26 = 00 */
    if (((insn >> 26) & 0x3u) == 0) {
        uint32_t op2 = decode_op2(cpu, insn);
        exec_dataop(cpu, insn, op2);
        cpu->r[15] += 4;
        return 1;
    }

    /* 单数据传输 LDR/STR：bit27-26 = 01。
       P=1 前变址（先算地址再访存），U 决定 +/- 偏移，L=1 读、L=0 写。
       本阶段支持字宽（B=0）与立即偏移（I=0）；字节/寄存器偏移留待后续。 */
    if (((insn >> 26) & 0x3u) == 1) {
        unsigned p = (insn >> 24) & 1u;
        unsigned u = (insn >> 23) & 1u;
        unsigned b = (insn >> 22) & 1u;
        unsigned w = (insn >> 21) & 1u;
        unsigned l = (insn >> 20) & 1u;
        unsigned rn = (insn >> 16) & 0xFu;
        unsigned rd = (insn >> 12) & 0xFu;
        if (insn & (1u << 25)) { /* 寄存器偏移：本阶段未实现 */
            printf("cpu: PC=%08X insn=%08X ldr/str reg-offset unimplemented\n",
                   cpu->r[15], insn);
            cpu->r[15] += 4;
            return 1;
        }
        uint32_t offset = insn & 0xFFFu;
        /* 前变址地址 = Rn +/- 立即偏移 */
        uint32_t addr = u ? cpu->r[rn] + offset : cpu->r[rn] - offset;
        if (p == 0) addr = cpu->r[rn]; /* 后变址：地址 = Rn（先访存后更新，W 语义忽略） */
        if (w) cpu->r[rn] = addr;      /* W 回写：地址写回 Rn */
        if (l) {
            if (b) cpu->r[rd] = bus_read8(cpu->nds->bus, addr);
            else   cpu->r[rd] = bus_read32(cpu->nds->bus, addr);
            printf("cpu: PC=%08X insn=%08X LDR r%u, [r%u%s%X] = %08X cycles=%llu\n",
                   cpu->r[15], insn, rd, rn, u ? "+" : "-", offset, cpu->r[rd],
                   (unsigned long long)cpu->cycles);
        } else {
            if (b) bus_write8(cpu->nds->bus, addr, (uint8_t)cpu->r[rd]);
            else   bus_write32(cpu->nds->bus, addr, cpu->r[rd]);
            printf("cpu: PC=%08X insn=%08X STR r%u, [r%u%s%X] cycles=%llu\n",
                   cpu->r[15], insn, rd, rn, u ? "+" : "-", offset,
                   (unsigned long long)cpu->cycles);
        }
        cpu->r[15] += 4;
        return 1;
    }

    /* 其余未实现指令：打印机器码并继续（后续阶段逐类补充）。 */
    printf("cpu: PC=%08X insn=%08X unimplemented (cycles=%llu)\n",
           cpu->r[15], insn, (unsigned long long)cpu->cycles);
    cpu->r[15] += 4;
    return 1;
}
