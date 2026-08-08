#ifndef NDS_EMU_CPU_H
#define NDS_EMU_CPU_H

#include <stdint.h>
#include "nds/nds.h"

/* ARM9 处理器（ARM946E-S）的可见状态。阶段 3a 只实现「取指 + 推进」，
   寄存器大多留空，供阶段 3b 逐条指令填充。 */
typedef struct arm_cpu {
    nds_t *nds;      /* 所属机器：取指/访存都通过它的 bus */
    uint32_t r[16];  /* 通用寄存器 r0-r15；r15 即 PC（程序计数器） */
    uint32_t cpsr;   /* 当前程序状态寄存器（阶段 3b 条件码再用） */
    uint64_t cycles; /* 已执行指令数（供主循环计数/验证） */
    int deadloop_reported; /* 死循环识别已打印过（避免每步刷屏） */
} arm_cpu_t;

/* 创建 / 销毁 ARM9 CPU。reset_pc：复位后开始执行的地址（镜像入口）。 */
arm_cpu_t *cpu_create(nds_t *nds, uint32_t reset_pc);
void cpu_destroy(arm_cpu_t *cpu);

/* 复位：把 PC 设为入口地址，清零 cycles（装载镜像后调用）。 */
void cpu_reset(arm_cpu_t *cpu, uint32_t reset_pc);

/* 从 bus 按 PC 取 32 位指令字（3a.3）。 */
uint32_t cpu_fetch(const arm_cpu_t *cpu);

/* 执行一条指令并推进 PC（3a.4 起）。返回 0 表示执行到停机点。 */
int cpu_step(arm_cpu_t *cpu);

#endif /* NDS_EMU_CPU_H */
