#include <stdio.h>
#include <stdlib.h>
#include "cpu.h"
#include "exec.h"
#include "thumb.h"
#include "bus/bus.h"
#include "io/io.h"

arm_cpu_t *cpu_create(nds_t *nds, uint32_t reset_pc, int is_arm7)
{
    arm_cpu_t *cpu = calloc(1, sizeof(arm_cpu_t));
    if (cpu == NULL)
        return NULL;
    cpu->nds = nds;
    cpu->is_arm7 = is_arm7;
    /* r15 = PC，初始指向镜像入口；cpsr 清零（真机复位后是 SVC 模式，此处简化） */
    cpu->r[15] = reset_pc;
    /* 异常向量基址：ARM9 用高向量 0xFFFF0000，ARM7 用低向量 0x00000000（阶段 12） */
    cpu->vector_base = is_arm7 ? 0x00000000u : 0xFFFF0000u;
    return cpu;
}

void cpu_destroy(arm_cpu_t *cpu)
{
    free(cpu);
}

/* 复位：装载镜像完成后把 PC 指到 ARM9 入口，重新开始计数。 */
void cpu_reset(arm_cpu_t *cpu, uint32_t reset_pc)
{
    cpu->r[15] = reset_pc;
    cpu->cycles = 0;
    cpu->deadloop_reported = 0;
}

/* 3a.3 取指：按 PC 从总线读 32 位指令字（小端拼拆已在 bus_read32 内完成）。 */
uint32_t cpu_fetch(const arm_cpu_t *cpu)
{
    return bus_read32(cpu->nds->bus, cpu->r[15]);
}

/* 13.2 Thumb 取指：按 PC 从总线读 16 位半字指令。 */
uint16_t cpu_fetch16(const arm_cpu_t *cpu)
{
    return bus_read16(cpu->nds->bus, cpu->r[15]);
}

/* 单步执行一条指令：
   框架只负责「取指 + 指令计数」，指令语义全部委托给 exec_step（见 exec.c）。
   返回 0 表示停机（本阶段总是返回 1，停机由死循环达成）。 */
int cpu_step(arm_cpu_t *cpu)
{
    /* 8.x：设置当前访问者身份，供 bus 对中断/FIFO 等按 CPU 分流 */
    cpu->nds->bus->active_is_arm7 = cpu->is_arm7;
    /* 6.5：一条指令 ≈ 一个周期，推进所有使能定时器（分频在 timer.c 内处理） */
    io_advance_timers(cpu->nds->io);
    /* 12.5：取指前检查 IRQ。条件 = 该核 IF&IE&IME 挂起，且 CPSR 的 I 位未禁止。
       满足则进 IRQ 异常向量（0x18），PC 跳到 handler；被打断指令地址留作返回点。 */
    irq_t *irq = &cpu->nds->io->irq[cpu->is_arm7 ? 1 : 0];
    if (irq_pending(irq) && !(cpu->cpsr & CPSR_I)) {
        cpu->cycles++;
        /* 21-B9b：诊断首中断现场——FFXII 的 ARM9 第一次 IRQ 会跳到高向量 0xFFFF0018，
           真 BIOS 在那里从“可读写 RAM 的约定槽”取用户 handler。先看游戏把 handler
           装到哪：ARM9 槽在 DTCM 末 8 字节（0x3FF8=等待标志/0x3FFC=handler 指针），
           ARM7 槽在 WRAM 高地址 0x03FFFFF8/FC。只打一次，避免轮询刷屏。 */
        if (cpu->nds->bus->diag && !cpu->irq_dump_done) {
            cpu->irq_dump_done = 1;
            uint32_t slot_f8 = 0, slot_fc = 0;
            if (cpu->is_arm7) {
                slot_f8 = bus_read32(cpu->nds->bus, 0x03FFFFF8u);
                slot_fc = bus_read32(cpu->nds->bus, 0x03FFFFFCu);
            } else {
                /* 用 bus 保存的 DTCM 基址计算槽位，游戏改配 DTCM 时也跟得上 */
                uint32_t base = cpu->nds->bus->arm9_dtcm_on
                                    ? cpu->nds->bus->arm9_dtcm_base
                                    : 0x027E0000u; /* 未使能时回退 FFXII 的已知配置 */
                slot_f8 = bus_read32(cpu->nds->bus, base + 0x3FF8u);
                slot_fc = bus_read32(cpu->nds->bus, base + 0x3FFCu);
            }
            printf("irq: first %s IRQ pc=%08X cpsr=%08X ime=%08X ie=%08X ifl=%08X"
                   " slot+3FF8=%08X slot+3FFC=%08X\n",
                   cpu->is_arm7 ? "arm7" : "arm9", cpu->r[15], cpu->cpsr,
                   irq->ime, irq->ie, irq->ifl, slot_f8, slot_fc);
        }
        arm_exception(cpu, EXC_IRQ_OFF, ARM_MODE_IRQ, 4);
        return 1;
    }
    /* 13.2：按 CPSR.T 位分发——Thumb 取 16 位半字，ARM 取 32 位字。 */
    if (cpu->cpsr & CPSR_T) {
        uint16_t insn16 = cpu_fetch16(cpu);
        cpu->cycles++;
        return thumb_step(cpu, insn16);
    }
    uint32_t insn = cpu_fetch(cpu);
    cpu->cycles++;
    return exec_step(cpu, insn);
}
