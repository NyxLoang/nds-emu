#ifndef NDS_EMU_BIOS_H
#define NDS_EMU_BIOS_H

#include <stdint.h>

typedef struct arm_cpu arm_cpu_t; /* 前向声明：bios 只操作寄存器/总线指针 */

/* SWI 函数号（NDS，ARM9/ARM7 视角）。
   ARM 状态下指令为 `SWI <n<<16>`，注释字段低 24 位 = n<<16，
   故函数号 n = cpu->swi_num >> 16（见 docs/12-bios-hle.md）。 */
#define BIOS_SWI_SOFT_RESET         0x00u
#define BIOS_SWI_REGISTER_RAM_RESET 0x01u
#define BIOS_SWI_WAIT_BY_LOOP       0x03u
#define BIOS_SWI_INTR_WAIT          0x04u
#define BIOS_SWI_VBLANK_INTR_WAIT   0x05u
#define BIOS_SWI_HALT               0x06u
#define BIOS_SWI_SOUNDBIAS          0x08u   /* 仅 NDS7 */
#define BIOS_SWI_DIV                0x09u
#define BIOS_SWI_CPUSET             0x0Bu
#define BIOS_SWI_CPUFASTSET         0x0Cu
#define BIOS_SWI_SQRT               0x0Du
#define BIOS_SWI_GET_CRC16          0x0Eu
#define BIOS_SWI_BITUNPACK          0x10u
#define BIOS_SWI_LZ77_WRAM          0x11u
#define BIOS_SWI_LZ77_VRAM          0x12u
#define BIOS_SWI_HUFF               0x13u
#define BIOS_SWI_RL_WRAM            0x14u
#define BIOS_SWI_RL_VRAM            0x15u
#define BIOS_SWI_GET_SINE_TABLE     0x1Au  /* 仅 NDS7 */
#define BIOS_SWI_GET_PITCH_TABLE    0x1Bu  /* 仅 NDS7 */
#define BIOS_SWI_GET_VOLUME_TABLE   0x1Cu  /* 仅 NDS7 */
#define BIOS_SWI_GET_BOOT_PROCS     0x1Du  /* 仅 NDS7 */

/* bios_dispatch 返回值：决定 SWI 之后 PC 是否前进。 */
#define BIOS_RET_HANDLED 1    /* 已知函数已处理：PC += 4 */
#define BIOS_RET_UNKNOWN 0    /* 未知号：打印日志，PC += 4 */
#define BIOS_RET_WAIT    (-1) /* 等待未满足：PC 不动，重跑本 SWI */
#define BIOS_RET_REDIR   (-2) /* 21-B9wf：处理器已切到 HLE 低地址状态，PC 不再由 exec 推进 */

/* HLE 分发：按 SWI 函数号 n（= swi_num>>16）调用对应实现。 */
int bios_dispatch(uint32_t n, arm_cpu_t *cpu);

#endif /* NDS_EMU_BIOS_H */
