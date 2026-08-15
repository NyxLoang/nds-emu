#ifndef NDS_EMU_CPU_EXEC_H
#define NDS_EMU_CPU_EXEC_H

#include <stdint.h>
#include "cpu.h"

/* CPSR 标志位：N(31) Z(30) C(29) V(28)，与真机一致 */
#define CPSR_N (1u << 31)  /* 负结果（结果最高位为 1） */
#define CPSR_Z (1u << 30)  /* 零结果 */
#define CPSR_C (1u << 29)  /* 进位 / 无借位 */
#define CPSR_V (1u << 28)  /* 有符号溢出 */

/* CPSR 控制位（阶段 12 起用于异常/模式切换） */
#define CPSR_I   (1u << 7)    /* IRQ 禁止位（1=禁止 IRQ） */
#define CPSR_F   (1u << 6)    /* FIQ 禁止位（1=禁止 FIQ） */
#define CPSR_T   (1u << 5)    /* Thumb 状态（阶段 13 用，本阶段恒 0） */
#define CPSR_MODE_MASK 0x1Fu  /* 模式位 M[4:0] */

/* 处理器模式（M[4:0]） */
#define ARM_MODE_USER 0x10u  /* User：非特权 */
#define ARM_MODE_FIQ  0x11u  /* FIQ：快速中断 */
#define ARM_MODE_IRQ  0x12u  /* IRQ：普通中断 */
#define ARM_MODE_SVC  0x13u  /* Supervisor：复位 / SWI */
#define ARM_MODE_ABT  0x17u  /* Abort：取指/数据中止 */
#define ARM_MODE_UND  0x1Bu  /* Undefined：未定义指令 */
#define ARM_MODE_SYS  0x1Fu  /* System：特权但同 User 寄存器组 */

/* 异常向量偏移（相对 vector_base） */
#define EXC_RESET_OFF 0x00u
#define EXC_UNDEF_OFF 0x04u
#define EXC_SWI_OFF   0x08u
#define EXC_PABT_OFF  0x0Cu
#define EXC_DABT_OFF  0x10u
#define EXC_IRQ_OFF   0x18u
#define EXC_FIQ_OFF   0x1Cu

/* 执行一条指令的全部语义：条件判断 → 译码 → 各指令实现 → 推进 PC。
   由 cpu_step 调用（cpu_step 负责取指与指令计数）。
   返回 0 表示停机，1 表示继续。 */
int exec_step(arm_cpu_t *cpu, uint32_t insn);

/* 进入异常：保存当前 CPSR 到目标模式的 SPSR → 切模式 → 按类型置 I/F → 写 LR → 跳向量。
   vector_offset：异常向量偏移（EXC_*_OFF）；new_mode：进入的模式（ARM_MODE_*）；
   lr_adjust：LR = 当前 PC + lr_adjust（SWI/未定义/IRQ/FIQ 为 +4，数据中止为 +8）。 */
void arm_exception(arm_cpu_t *cpu, uint32_t vector_offset, unsigned new_mode,
                   uint32_t lr_adjust);

/* 特权模式 → spsr[5] 下标（FIQ/IRQ/SVC/ABT/UND = 0..4）；User/System 无 SPSR 返回 -1。 */
int exec_spsr_index(unsigned mode);

/* 指令级跟踪日志开关：默认开。批量跑 LDR/STR 循环时（如 4.5 写屏测试码）
   临时关闭，避免每条指令 printf 刷屏并拖慢模拟。 */
void exec_set_trace(int on);

#endif /* NDS_EMU_CPU_EXEC_H */
