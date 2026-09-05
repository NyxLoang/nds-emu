#include <stdio.h>
#include "nds/nds.h"      /* nds_t / bus_t */
#include "bus/bus.h"      /* bus_set_diag */
#include "cpu/cpu.h"      /* cpu_step / arm_cpu_t（r / cycles） */
#include "cpu/exec.h"     /* exec_set_trace */
#include "cpu/thumb.h"    /* thumb_set_trace */
#include "io/io.h"        /* io_set_vblank */
#include "runner.h"

void runner_headless(nds_t *nds, uint64_t steps, int trace)
{
    bus_set_diag(nds->bus, 1);      /* 只打异常事件：未知 SWI/未实现指令/未知 IO */
    exec_set_trace(trace);          /* trace=1 时逐条打印指令，用于追前几十步 */
    thumb_set_trace(trace);

    printf("headless: running %llu steps (ARM9:ARM7 = 2:1)\n",
           (unsigned long long)steps);
    fflush(stdout);

    for (uint64_t i = 0; i < steps; i++) {
        if (i % 3 == 2)
            cpu_step(nds->cpu7);
        else
            cpu_step(nds->cpu);

        /* 近似一帧（约 100 万步）触发一次 VBlank，模拟显示硬件，让等 VBlank 的游戏能继续 */
        if ((i & 0xFFFFFu) == 0xFFFFFu)
            io_set_vblank(nds->io);
        /* 每 100 万步打一次进度 */
        if ((i & 0xFFFFFu) == 0xFFFFFu) {
            /* 21-B9h：进度附带两核中断寄存器，便于判断“等 IRQ 但没来”是
               使能被关、挂起没置位还是 handler 没清位。 */
            printf("headless: step=%llu ARM9 PC=%08X cyc=%llu cpsr=%08X r0=%08X r1=%08X r2=%08X r3=%08X ime=%08X ie=%08X if=%08X\n",
                   (unsigned long long)(i + 1),
                   nds->cpu->r[15], (unsigned long long)nds->cpu->cycles,
                   nds->cpu->cpsr, nds->cpu->r[0], nds->cpu->r[1],
                   nds->cpu->r[2], nds->cpu->r[3],
                   nds->io->irq[0].ime,
                   nds->io->irq[0].ie, nds->io->irq[0].ifl);
            printf("headless: step=%llu ARM7 PC=%08X cyc=%llu ime=%08X ie=%08X if=%08X\n",
                   (unsigned long long)(i + 1),
                   nds->cpu7->r[15], (unsigned long long)nds->cpu7->cycles,
                   nds->io->irq[1].ime, nds->io->irq[1].ie, nds->io->irq[1].ifl);
        }
    }

    printf("headless: done. ARM9 PC=%08X cyc=%llu cpsr=%08X ime=%08X ie=%08X if=%08X"
           " | ARM7 PC=%08X cyc=%llu ime=%08X ie=%08X if=%08X\n",
           nds->cpu->r[15], (unsigned long long)nds->cpu->cycles,
           nds->cpu->cpsr,
           nds->io->irq[0].ime, nds->io->irq[0].ie, nds->io->irq[0].ifl,
           nds->cpu7->r[15], (unsigned long long)nds->cpu7->cycles,
           nds->io->irq[1].ime, nds->io->irq[1].ie, nds->io->irq[1].ifl);
    fflush(stdout);
}
