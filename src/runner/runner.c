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
        if ((i & 0xFFFFFu) == 0xFFFFFu)
            printf("headless: step=%llu ARM9 PC=%08X cyc=%llu | ARM7 PC=%08X cyc=%llu\n",
                   (unsigned long long)(i + 1),
                   nds->cpu->r[15], (unsigned long long)nds->cpu->cycles,
                   nds->cpu7->r[15], (unsigned long long)nds->cpu7->cycles);
    }

    printf("headless: done. ARM9 PC=%08X cyc=%llu | ARM7 PC=%08X cyc=%llu\n",
           nds->cpu->r[15], (unsigned long long)nds->cpu->cycles,
           nds->cpu7->r[15], (unsigned long long)nds->cpu7->cycles);
    fflush(stdout);
}
