#include <stdlib.h>
#include "cpu.h"
#include "exec.h"
#include "bus/bus.h"

arm_cpu_t *cpu_create(nds_t *nds, uint32_t reset_pc)
{
    arm_cpu_t *cpu = calloc(1, sizeof(arm_cpu_t));
    if (cpu == NULL)
        return NULL;
    cpu->nds = nds;
    /* r15 = PC，初始指向镜像入口；cpsr 清零（真机复位后是 SVC 模式，此处简化） */
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

/* 单步执行一条指令：
   框架只负责「取指 + 指令计数」，指令语义全部委托给 exec_step（见 exec.c）。
   返回 0 表示停机（本阶段总是返回 1，停机由死循环达成）。 */
int cpu_step(arm_cpu_t *cpu)
{
    uint32_t insn = cpu_fetch(cpu);
    cpu->cycles++;
    return exec_step(cpu, insn);
}
