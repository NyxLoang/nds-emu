#ifndef NDS_EMU_BIOS_SND_H
#define NDS_EMU_BIOS_SND_H

typedef struct arm_cpu arm_cpu_t;

/* SWI 0x08 SoundBias（仅 NDS7）：把 SOUNDBIAS 电平平滑调到目标值。
   入：r0=目标电平（0=000h，其它=200h），r1=延迟计数。
   本模拟器瞬时完成调整，忽略 r1。 */
int bios_soundbias(arm_cpu_t *cpu);

#endif /* NDS_EMU_BIOS_SND_H */
