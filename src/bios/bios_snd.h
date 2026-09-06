#ifndef NDS_EMU_BIOS_SND_H
#define NDS_EMU_BIOS_SND_H

typedef struct arm_cpu arm_cpu_t;

/* SWI 0x08 SoundBias（仅 NDS7）：把 SOUNDBIAS 电平平滑调到目标值。
   入：r0=目标电平（0=000h，其它=200h），r1=延迟计数。
   本模拟器瞬时完成调整，忽略 r1。 */
int bios_soundbias(arm_cpu_t *cpu);

/* SWI 0x1A/0x1B/0x1C（ARM7 音频查表）与 0x1D 启动处理器句柄。 */
int bios_get_sine_table(arm_cpu_t *cpu);
int bios_get_pitch_table(arm_cpu_t *cpu);
int bios_get_volume_table(arm_cpu_t *cpu);
int bios_get_boot_procs(arm_cpu_t *cpu);

#endif /* NDS_EMU_BIOS_SND_H */
