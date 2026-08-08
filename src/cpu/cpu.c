#include <stdio.h>
#include <stdlib.h>
#include "cpu.h"
#include "bus/bus.h"

/* ARM 指令高位位模式（阶段 3b 会拆更多）：
   bit31-28 = 条件码，bit27-25 = 大类。
   无条件 AL = 1110；大类 101 = 分支 B/BL。
   死循环「B 自己」= 分支立即数为 -1（跳转量 0 步）。 */
#define ARM_COND_AL    0xEu
#define ARM_OP_BRANCH  0x5u
#define BRANCH_OFFSET_MINUS1 0xFFFFFEu  /* 24 位立即数全 1（除符号位外） */

arm_cpu_t *cpu_create(nds_t *nds, uint32_t reset_pc)
{
    arm_cpu_t *cpu = calloc(1, sizeof(arm_cpu_t));
    if (cpu == NULL)
        return NULL;
    cpu->nds = nds;
    /* r15 = PC，初始指向镜像入口；cpsr 清零（阶段 3b 再细化） */
    cpu->r[15] = reset_pc;
    return cpu;
}

void cpu_destroy(arm_cpu_t *cpu)
{
    free(cpu);
}

/* 复位：装载镜像完成后把 PC 指到 ARM9 入口，重新开始计数。 */
void cpu_reset(arm_cpu_t *cpu, uint32_t reset_pc)
{
    cpu->r[15] = reset_pc;
    cpu->cycles = 0;
    cpu->deadloop_reported = 0;
}

/* 3a.3 取指：按 PC 从总线读 32 位指令字（小端拼拆已在 bus_read32 内完成）。 */
uint32_t cpu_fetch(const arm_cpu_t *cpu)
{
    return bus_read32(cpu->nds->bus, cpu->r[15]);
}

/* 3a.4/3a.5/3a.6 单步执行：
   - 取指；
   - 若是「无条件 B 跳自己」→ 原地打转（PC 不前进），返回 1 表示继续空转；
   - 若是其它指令 → 打印机器码并计数，模拟器继续（阶段 3b 逐条实现）。
   返回 0 表示停机。 */
int cpu_step(arm_cpu_t *cpu)
{
    uint32_t insn = cpu_fetch(cpu);
    cpu->cycles++;

    /* 译码：只看需要的高位。条件码取 bit31-28，大类取 bit27-25。
       位计算：insn >> 28 拿高 4 位；insn >> 25 & 7 拿 3 位大类。 */
    unsigned cond = (insn >> 28) & 0xF;
    unsigned op   = (insn >> 25) & 0x7;
    uint32_t imm24 = insn & 0x00FFFFFFu;  /* 分支的 24 位立即数 */

    if (cond == ARM_COND_AL && op == ARM_OP_BRANCH) {
        /* 分支指令：立即数 = 符号扩展的 24 位 << 2。
           为验证死循环，这里先特判「跳自己」；完整 B 实现在阶段 3b.7。 */
        int32_t offset = (int32_t)(imm24 << 8) >> 8;   /* 24 位符号扩展 */
        int32_t disp = offset << 2;                     /* 以字节为单位的位移 */
        uint32_t target = cpu->r[15] + 8u + (uint32_t)disp; /* ARM 有 PC+8 偏移 */
        if (imm24 == BRANCH_OFFSET_MINUS1) {
            /* 目标 = PC+8-8 = PC：跳到自己，死循环。PC 保持不动。
               只在第一次命中时打印，避免主循环每帧刷屏。 */
            if (!cpu->deadloop_reported) {
                printf("cpu: PC=%08X insn=%08X dead-loop branch to self (cycles=%llu)\n",
                       cpu->r[15], insn, (unsigned long long)cpu->cycles);
                cpu->deadloop_reported = 1;
            }
            return 1; /* 继续空转，验证多步后 PC 仍在同一地址 */
        }
        printf("cpu: PC=%08X insn=%08X branch to %08X (cycles=%llu)\n",
               cpu->r[15], insn, target, (unsigned long long)cpu->cycles);
        cpu->r[15] = target;
        return 1;
    }

    /* 3a.6 未实现指令：打印机器码并继续（阶段 3b 逐类实现）。 */
    printf("cpu: PC=%08X insn=%08X unimplemented (cycles=%llu)\n",
           cpu->r[15], insn, (unsigned long long)cpu->cycles);
    cpu->r[15] += 4; /* 非分支指令默认推进 4 字节 */
    return 1;
}
