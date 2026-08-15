#ifndef NDS_EMU_BIOS_MEM_H
#define NDS_EMU_BIOS_MEM_H

#include <stdint.h>

typedef struct arm_cpu arm_cpu_t;

/* SWI 0x0B CpuSet：按 16/32 位搬移或填充（阶段 11.4） */
int bios_cpuset(arm_cpu_t *cpu);

/* SWI 0x0C CpuFastSet：按 32 位搬移或填充（阶段 11.4） */
int bios_cpufastset(arm_cpu_t *cpu);

#endif /* NDS_EMU_BIOS_MEM_H */
