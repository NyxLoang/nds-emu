#include <stdint.h>
#include "bios.h"
#include "bios_snd.h"
#include "bios_audio_tables.h"
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

/* SWI 0x1A GetSineTable (ARM7): return bios_audio_sine[r0] */
int bios_get_sine_table(arm_cpu_t *cpu)
{
    uint32_t i = cpu->r[0];
    cpu->r[0] = (i < 64u) ? bios_audio_sine[i] : 0u;
    return BIOS_RET_HANDLED;
}

/* SWI 0x1B GetPitchTable (ARM7): return bios_audio_pitch[r0] */
int bios_get_pitch_table(arm_cpu_t *cpu)
{
    uint32_t i = cpu->r[0];
    cpu->r[0] = (i < 768u) ? bios_audio_pitch[i] : 0u;
    return BIOS_RET_HANDLED;
}

/* SWI 0x1C GetVolumeTable (ARM7): return bios_audio_volume[r0] */
int bios_get_volume_table(arm_cpu_t *cpu)
{
    uint32_t i = cpu->r[0];
    cpu->r[0] = (i < 724u) ? bios_audio_volume[i] : 0u;
    return BIOS_RET_HANDLED;
}

/* SWI 0x1D GetBootProcs (ARM7): return the three boot-processor handles */
int bios_get_boot_procs(arm_cpu_t *cpu)
{
    cpu->r[0] = 0x00000A2Eu;
    cpu->r[1] = 0x00002C3Cu;
    cpu->r[2] = 0x000005FFu;
    return BIOS_RET_HANDLED;
}
