#ifndef NDS_EMU_CPU_EXEC_H
#define NDS_EMU_CPU_EXEC_H

#include <stdint.h>
#include "cpu.h"

/* CPSR 标志位：N(31) Z(30) C(29) V(28)，与真机一致 */
#define CPSR_N (1u << 31)  /* 负结果（结果最高位为 1） */
#define CPSR_Z (1u << 30)  /* 零结果 */
#define CPSR_C (1u << 29)  /* 进位 / 无借位 */
#define CPSR_V (1u << 28)  /* 有符号溢出 */

/* 执行一条指令的全部语义：条件判断 → 译码 → 各指令实现 → 推进 PC。
   由 cpu_step 调用（cpu_step 负责取指与指令计数）。
   返回 0 表示停机，1 表示继续。 */
int exec_step(arm_cpu_t *cpu, uint32_t insn);

#endif /* NDS_EMU_CPU_EXEC_H */
