#include <stdio.h>
#include "bios.h"
#include "cpu/cpu.h"
#include "bios_arith.h"
#include "bios_mem.h"
#include "bios_decompress.h"
#include "bios_wait.h"

/* HLE 分发：把 SWI 函数号路由到对应功能文件。
   返回 BIOS_RET_* 之一，exec_step 据返回值决定 SWI 之后 PC 是否前进。 */
int bios_dispatch(uint32_t n, arm_cpu_t *cpu)
{
    switch (n) {
    case BIOS_SWI_DIV:        return bios_div(cpu);
    case BIOS_SWI_SQRT:       return bios_sqrt(cpu);
    case BIOS_SWI_CPUSET:     return bios_cpuset(cpu);
    case BIOS_SWI_CPUFASTSET: return bios_cpufastset(cpu);
    case BIOS_SWI_BITUNPACK:  return bios_bitunpack(cpu);
    case BIOS_SWI_LZ77_WRAM:  return bios_lz77(cpu);
    case BIOS_SWI_LZ77_VRAM:  return bios_lz77(cpu);
    case BIOS_SWI_HUFF:       return bios_huff(cpu);
    case BIOS_SWI_RL_WRAM:    return bios_rl(cpu);
    case BIOS_SWI_RL_VRAM:    return bios_rl(cpu);
    case BIOS_SWI_HALT:             return bios_halt(cpu);
    case BIOS_SWI_INTR_WAIT:        return bios_intr_wait(cpu);
    case BIOS_SWI_VBLANK_INTR_WAIT: return bios_vblank_intr_wait(cpu);
    case BIOS_SWI_WAIT_BY_LOOP:     return bios_wait_by_loop(cpu);
    /* SoftReset / RegisterRamReset / GetCRC16 等本阶段未实现，仅记录。
       用位图保证每个未知号只打印一次，避免游戏循环调用时刷屏。 */
    default:
    {
        static uint32_t unknown_swi_seen[8]; /* 256 个函数号 → 8×32 位位图 */
        unsigned idx = (unsigned)n >> 5, bit = (unsigned)n & 31u;
        if (!(unknown_swi_seen[idx] & (1u << bit))) {
            unknown_swi_seen[idx] |= (1u << bit);
            printf("bios: unknown SWI 0x%02X at PC=%08X\n", (unsigned)n, cpu->r[15]);
        }
        return BIOS_RET_UNKNOWN;
    }
    }
}
