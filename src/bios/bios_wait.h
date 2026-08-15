#ifndef NDS_EMU_BIOS_WAIT_H
#define NDS_EMU_BIOS_WAIT_H

#include <stdint.h>

typedef struct arm_cpu arm_cpu_t;

/* SWI 0x06 Halt：等 (IE & IF) != 0（阶段 11.6） */
int bios_halt(arm_cpu_t *cpu);

/* SWI 0x04 IntrWait：等 r1 指定中断位，返回前清位（阶段 11.6） */
int bios_intr_wait(arm_cpu_t *cpu);

/* SWI 0x05 VBlankIntrWait：等 VBlank 位，返回前清位（阶段 11.6） */
int bios_vblank_intr_wait(arm_cpu_t *cpu);

/* SWI 0x03 WaitByLoop：忙等延时（阶段 11.6，本模拟器按空操作处理） */
int bios_wait_by_loop(arm_cpu_t *cpu);

#endif /* NDS_EMU_BIOS_WAIT_H */
