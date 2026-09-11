#include <stdint.h>
#include <stdio.h>
#include "bios.h"
#include "bios_wait.h"
#include "cpu/cpu.h"
#include "cpu/exec.h"
#include "io/io.h"

/* 取当前 CPU 视角的中断寄存器（ARM9 用 [0]、ARM7 用 [1]）。 */
static irq_t *cpu_irq(arm_cpu_t *cpu)
{
    return &cpu->nds->io->irq[cpu->is_arm7 ? 1 : 0];
}

/* 21-B9wf：进入 ARM7 FreeBIOS 低地址等待路径（HLE 状态机）。
   参考 0x1080 SWI 分发会把处理器切到 System 模式（T 清 0、I/F 保持原值），
   到 0x112C 尾部再恢复调用方 CPSR。这里只保留对任务可见的状态：System 模式、
   原始中断屏蔽位、原始标志，以及调用方返回 PC。 */
static void bios7_enter_low(arm_cpu_t *cpu, uint32_t low_pc)
{
    uint32_t next = cpu->r[15] + ((cpu->cpsr & CPSR_T) ? 2u : 4u);
    cpu->bios7_ret = next;
    cpu->bios7_cpsr = cpu->cpsr;
    cpu->bios7_active = 1;
    cpu->bios7_delay = 0;
    cpu->bios7_pc = low_pc;
    /* 保留 N/Z/C/V/I/F，模式置 System，清 Thumb：与 FreeBIOS 0x108C-0x1098 一致 */
    uint32_t low = (cpu->cpsr & (CPSR_N | CPSR_Z | CPSR_C | CPSR_V |
                                 CPSR_I | CPSR_F)) | ARM_MODE_SYS;
    exec_apply_cpsr(cpu, low);
    cpu->r[15] = low_pc;
}

/* SWI 0x06 Halt：等 (IE & IF) != 0。满足则继续，否则停在 SWI 上重试。
   （真机此时 CPU 进入低功耗，视频/定时器等继续运行；本模拟器用「不前进 PC」等价等待。） */
int bios_halt(arm_cpu_t *cpu)
{
    irq_t *irq = cpu_irq(cpu);
    if (cpu->is_arm7) {
        /* 21-B9wf：ARM7 按 melonDS HaltInterrupted 语义等待——
           (IF&IE)!=0 即唤醒，不要求 IME；唤醒后 BIOS 尾部恢复调用方，IRQ
           再由 cpu_step 的 IRQ 入口决定是否立即接管。 */
        if (irq->ie & irq->ifl) {
            if (cpu->bios7_active)
                return BIOS_RET_REDIR; /* 已走低地址尾部，保持状态 */
            bios7_enter_low(cpu, 0x00001158u);
            return BIOS_RET_REDIR;
        }
        if (!cpu->bios7_active)
            bios7_enter_low(cpu, 0x00001158u);
        cpu->bios7_halted = 1;
        cpu->step_cycles = 0;
        return BIOS_RET_REDIR;
    }
    if (irq->ie & irq->ifl)
        return BIOS_RET_HANDLED;
    return BIOS_RET_WAIT;
}

/* SWI 0x04 IntrWait：等 r1 指定的中断位，返回前清位。
   入：r0=0 立即返回已置位 / 1 丢弃旧标志、r1=等待的中断位掩码。
   本模拟器按「已置位→清位返回，未置位→等待」简化处理，并强制 IME=1。 */
int bios_intr_wait(arm_cpu_t *cpu)
{
    irq_t *irq = cpu_irq(cpu);
    uint32_t flags = cpu->r[1];
    irq->ime = 1; /* IntrWait 强制总开关打开 */
    if (irq->ifl & flags) {
        irq->ifl &= ~flags;
        return BIOS_RET_HANDLED;
    }
    return BIOS_RET_WAIT;
}

/* SWI 0x05 VBlankIntrWait：等 VBlank 位（IF bit3），返回前清位。 */
int bios_vblank_intr_wait(arm_cpu_t *cpu)
{
    irq_t *irq = cpu_irq(cpu);
    if (irq->ifl & IO_IF_VBLANK) {
        irq->ifl &= ~IO_IF_VBLANK;
        return BIOS_RET_HANDLED;
    }
    return BIOS_RET_WAIT;
}

/* SWI 0x03 WaitByLoop：忙等延时（r0≈循环次数）。本模拟器不建模精确时序，按空操作处理。 */
int bios_wait_by_loop(arm_cpu_t *cpu)
{
    /* 21-B9wf：ARM7 的 SWI 3 是真循环（subs r0,#1; bgt），不是空操作。
       参考 FFXII 在 0x037FE408 队列等待处以 r0=0xFA 进入 0x115C，
       每轮约 3 周期；本地旧 HLE 直接返回使 ARM7 任务节奏远快于参考。 */
    if (cpu->is_arm7) {
        if (cpu->bios7_active && cpu->bios7_delay)
            return BIOS_RET_REDIR; /* 中断返回后继续当前循环计数 */
        if (cpu->r[0] != 0) {
            bios7_enter_low(cpu, 0x0000115Cu);
            cpu->bios7_delay = 1;
            return BIOS_RET_REDIR;
        }
    }
    return BIOS_RET_HANDLED;
}
