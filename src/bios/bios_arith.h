#ifndef NDS_EMU_BIOS_ARITH_H
#define NDS_EMU_BIOS_ARITH_H

#include <stdint.h>

typedef struct arm_cpu arm_cpu_t;

/* SWI 0x09 Div：有符号除法 r0/r1（阶段 11.3） */
int bios_div(arm_cpu_t *cpu);

/* SWI 0x0D Sqrt：无符号整数开方（阶段 11.3） */
int bios_sqrt(arm_cpu_t *cpu);

#endif /* NDS_EMU_BIOS_ARITH_H */
