#ifndef NDS_EMU_CPU_ARM7_H
#define NDS_EMU_CPU_ARM7_H

#include "cpu.h"

/* ARM7 功能文件：负责创建 ARM7 实例（is_arm7=1）。
   ARM7（ARM7TDMI）是协处理器，管音频/触摸/联网，有专属 WRAM（0x03800000）。
   指令执行语义复用 exec.c，此处只做 ARM7 特有的初始化/行为。 */
arm_cpu_t *arm7_create(nds_t *nds);

#endif /* NDS_EMU_CPU_ARM7_H */
