#include <stdint.h>
#include "bios.h"
#include "bios_snd.h"
#include "cpu/cpu.h"
#include "bus/bus.h"
#include "io/io.h"

/* SWI 0x08 SoundBias（NDS7 BIOS）：
   真机会按 r1 指定的延迟逐级增减 SOUNDBIAS，直至电平到达 r0 指定的目标；
   寄存器高 6 位保持原值。本模拟器无音频硬件时序，直接写成目标值。 */
int bios_soundbias(arm_cpu_t *cpu)
{
    uint16_t old = bus_read16(cpu->nds->bus, SND_SOUNDBIAS);
    uint16_t level = (cpu->r[0] == 0) ? 0x0000u : 0x0200u;
    uint16_t next = (uint16_t)((old & 0xFC00u) | level);

    bus_write16(cpu->nds->bus, SND_SOUNDBIAS, next);
    return BIOS_RET_HANDLED;
}
