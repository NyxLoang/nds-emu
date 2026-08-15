#include <stdint.h>
#include "bios.h"
#include "bios_wait.h"
#include "cpu/cpu.h"
#include "io/io.h"

/* 取当前 CPU 视角的中断寄存器（ARM9 用 [0]、ARM7 用 [1]）。 */
static irq_t *cpu_irq(arm_cpu_t *cpu)
{
    return &cpu->nds->io->irq[cpu->is_arm7 ? 1 : 0];
}

/* SWI 0x06 Halt：等 (IE & IF) != 0。满足则继续，否则停在 SWI 上重试。
   （真机此时 CPU 进入低功耗，视频/定时器等继续运行；本模拟器用「不前进 PC」等价等待。） */
int bios_halt(arm_cpu_t *cpu)
{
    irq_t *irq = cpu_irq(cpu);
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
    (void)cpu;
    return BIOS_RET_HANDLED;
}
