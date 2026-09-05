#ifndef NDS_EMU_BIOS_CRC16_H
#define NDS_EMU_BIOS_CRC16_H

#include <stdint.h>

typedef struct arm_cpu arm_cpu_t;

/* SWI 0x0E GetCRC16：对 r1 指向的 r2 个字节算 CRC-16/IBM，初值 r0（阶段 21-B9） */
int bios_crc16(arm_cpu_t *cpu);

#endif /* NDS_EMU_BIOS_CRC16_H */
