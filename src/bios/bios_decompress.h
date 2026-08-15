#ifndef NDS_EMU_BIOS_DECOMPRESS_H
#define NDS_EMU_BIOS_DECOMPRESS_H

#include <stdint.h>

typedef struct arm_cpu arm_cpu_t;

/* SWI 0x10 BitUnPack：位流解包（阶段 11.5） */
int bios_bitunpack(arm_cpu_t *cpu);

/* SWI 0x11/0x12 LZ77UnComp：LZ77 解压（阶段 11.5） */
int bios_lz77(arm_cpu_t *cpu);

/* SWI 0x13 HuffUnComp：Huffman 解压（阶段 11.5） */
int bios_huff(arm_cpu_t *cpu);

/* SWI 0x14/0x15 RLUnComp：RLE 解压（阶段 11.5） */
int bios_rl(arm_cpu_t *cpu);

#endif /* NDS_EMU_BIOS_DECOMPRESS_H */
