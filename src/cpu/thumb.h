#ifndef NDS_EMU_CPU_THUMB_H
#define NDS_EMU_CPU_THUMB_H

#include <stdint.h>

struct arm_cpu;

/* 执行一条 16 位 Thumb 指令的全部语义（条件分支/译码/各指令实现/推进 PC）。
   由 cpu_step 在 CPSR.T=1 时调用（cpu_step 负责取 16 位指令与计数）。
   返回 0 表示停机，1 表示继续。 */
int thumb_step(struct arm_cpu *cpu, uint16_t insn);

/* 指令级跟踪日志开关：默认开。批量跑 Thumb 循环时临时关闭避免刷屏。 */
void thumb_set_trace(int on);

#endif /* NDS_EMU_CPU_THUMB_H */
