#ifndef NDS_EMU_CPU_ARM9_H
#define NDS_EMU_CPU_ARM9_H

#include "cpu.h"

/* ARM9 功能文件：负责创建 ARM9 实例（is_arm7=0）。
   ARM9（ARM946E-S）是主处理器，跑游戏逻辑与渲染。
   指令执行语义复用 exec.c，此处只做 ARM9 特有的初始化/行为。 */
arm_cpu_t *arm9_create(nds_t *nds);

#endif /* NDS_EMU_CPU_ARM9_H */
