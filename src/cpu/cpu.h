#ifndef NDS_EMU_CPU_H
#define NDS_EMU_CPU_H

#include <stdint.h>
#include "nds/nds.h"

/* 21-B9f：IRQ HLE 桩保存的“被中断现场”。真 BIOS 在跳用户 IRQ handler 前会保存
   r0-r3/r12/SPSR，handler 返回后恢复；本模拟器没有可执行 BIOS 桩代码，改用模拟器
   私有槽保存，handler 弹出返回地址后由 cpu_step 取指前统一恢复。 */
typedef struct irq_hle_ctx {
    int active;          /* 有未恢复的 IRQ 现场 */
    int log_count;       /* 诊断：已打印过的中断/恢复次数（限前若干次） */
    uint32_t r[4];       /* 被中断代码的 r0-r3（用户 handler 可能改坏） */
    uint32_t ip;         /* r12 同 r0-r3 */
    uint32_t ret_pc;     /* 返回点 = 被打断指令地址 */
    uint32_t saved_cpsr; /* 被打断时的 CPSR（含模式/I 位） */
} irq_hle_ctx_t;

/* ARM 处理器（ARM946E-S 或 ARM7TDMI）的可见状态。两核共用同一套结构，
   用 is_arm7 区分实例；阶段 3a 只实现「取指 + 推进」，指令语义在 exec.c。 */
typedef struct arm_cpu {
    nds_t *nds;      /* 所属机器：取指/访存都通过它的 bus */
    int is_arm7;     /* 0=ARM9, 1=ARM7（中断/FIFO 等按访问者身份分流） */
    uint32_t r[16];  /* 通用寄存器 r0-r15；r15 即 PC（程序计数器） */
    uint32_t cpsr;   /* 当前程序状态寄存器（条件码 + 中断/模式位） */
    uint32_t spsr[5];/* 备份程序状态寄存器（阶段 12.3 起按特权模式各一份：
                        [0]=FIQ [1]=IRQ [2]=SVC [3]=ABT [4]=UND；User/System 无 SPSR） */
    uint32_t r13_sys;   /* User/System 模式的 r13（主 SP）；当前可见值在 r[13] */
    uint32_t r14_sys;   /* User/System 模式的 r14（主 LR）；当前可见值在 r[14] */
    uint32_t r13_bank[5]; /* FIQ/IRQ/SVC/ABT/UND 私有 r13（下标同 spsr；21-B9g） */
    uint32_t r14_bank[5]; /* FIQ/IRQ/SVC/ABT/UND 私有 r14（下标同 spsr；21-B9g） */
    uint32_t swi_num;/* 最近一次 SWI 的 24 位编号（阶段 10.8 记录，阶段 11 BIOS HLE 用） */
    uint32_t cp15[16]; /* CP15 协处理器寄存器（阶段 10.10 MRC/MCR，按 CRn 索引；阶段 12.4 c1 控制向量基址） */
    uint32_t cp15_dtcm; /* CP15 c9,c1,0：ARM9 DTCM 配置（阶段 21-B8） */
    uint32_t cp15_itcm; /* CP15 c9,c1,1：ARM9 ITCM 配置（先存储，暂不建映射） */
    uint32_t vector_base; /* 异常向量基址：ARM9=0xFFFF0000（高）、ARM7=0x00000000（低） */
    uint64_t cycles; /* 已执行指令数（供主循环计数/验证） */
    int deadloop_reported; /* 死循环识别已打印过（避免每步刷屏） */
    int irq_dump_done;     /* 首中断现场快照已打印过（21-B9b，避免每次 IRQ 刷屏） */
    int irq_mask_logged;   /* “IF&IE 已挂起但 CPSR.I 屏蔽”只提示一次（21-B9h） */
    irq_hle_ctx_t irq_hle; /* IRQ HLE 桩的现场保存区（21-B9f） */
} arm_cpu_t;

/* 创建 / 销毁 CPU 核。reset_pc：复位后开始执行的地址；is_arm7：实例身份。 */
arm_cpu_t *cpu_create(nds_t *nds, uint32_t reset_pc, int is_arm7);
void cpu_destroy(arm_cpu_t *cpu);

/* 复位：把 PC 设为入口地址，清零 cycles（装载镜像后调用）。 */
void cpu_reset(arm_cpu_t *cpu, uint32_t reset_pc);

/* 从 bus 按 PC 取 32 位指令字（3a.3）。 */
uint32_t cpu_fetch(const arm_cpu_t *cpu);

/* 从 bus 按 PC 取 16 位 Thumb 指令半字（13.2）。 */
uint16_t cpu_fetch16(const arm_cpu_t *cpu);

/* 执行一条指令并推进 PC（3a.4 起）。返回 0 表示执行到停机点。 */
int cpu_step(arm_cpu_t *cpu);

#endif /* NDS_EMU_CPU_H */
