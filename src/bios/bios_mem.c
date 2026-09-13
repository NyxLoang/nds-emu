#include <stdint.h>
#include "bios_mem.h"
#include "cpu/cpu.h"
#include "bus/bus.h"
#include <stdio.h>   /* 21-B9yi(续94)：bios_mem_report 打印统计 */

/* 21-B9yi(续94)：统计 CpuSet/CpuFastSet 的调用次数与拷贝总量。
   动机：续93b 看到重场景里有 25~27 ms 的突发帧（模拟占 20~24 ms），
   怀疑是「大块内存拷贝」这类 BIOS HLE 路径逐字走总线调度太慢。 */
static unsigned long long s_cpuset_calls, s_cpuset_words;
static unsigned long long s_cpufastset_calls, s_cpufastset_words;

void bios_mem_report(void)
{
    printf("biosmem: CpuSet %llu 次 / %llu 字，CpuFastSet %llu 次 / %llu 字\n",
           s_cpuset_calls, s_cpuset_words,
           s_cpufastset_calls, s_cpufastset_words);
    fflush(stdout);
}

/* SWI 0x0B CpuSet：按 16/32 位搬移或填充。
   入：r0=源、r1=目的、r2=控制字
     bit0-20 计数（32 位模式为字数，16 位模式为半字数）
     bit24   固定源地址（0=递增拷贝，1=固定填充 {HALF}WORD[r0]）
     bit26   数据宽度（0=16 位，1=32 位） */
int bios_cpuset(arm_cpu_t *cpu)
{
    bus_t *bus = cpu->nds->bus;
    uint32_t src = cpu->r[0];
    uint32_t dst = cpu->r[1];
    uint32_t ctl = cpu->r[2];
    uint32_t count = ctl & 0x1FFFFFu;
    int fill = (ctl >> 24) & 1;
    int bits32 = (ctl >> 26) & 1;
    s_cpuset_calls++;
    s_cpuset_words += count;

    if (bits32) {
        for (uint32_t i = 0; i < count; i++) {
            uint32_t v = fill ? bus_read32(bus, src)
                              : bus_read32(bus, src + 4 * i);
            bus_write32(bus, dst + 4 * i, v);
        }
    } else {
        for (uint32_t i = 0; i < count; i++) {
            uint16_t v = fill ? bus_read16(bus, src)
                              : bus_read16(bus, src + 2 * i);
            bus_write16(bus, dst + 2 * i, v);
        }
    }
    return 1;
}

/* SWI 0x0C CpuFastSet：按 32 位搬移或填充。
   入：r0=源、r1=目的、r2=控制字
     bit0-20 字数、bit24 固定源（0=拷贝，1=填充 WORD[r0]） */
int bios_cpufastset(arm_cpu_t *cpu)
{
    bus_t *bus = cpu->nds->bus;
    uint32_t src = cpu->r[0];
    uint32_t dst = cpu->r[1];
    uint32_t ctl = cpu->r[2];
    uint32_t count = ctl & 0x1FFFFFu;
    int fill = (ctl >> 24) & 1;
    s_cpufastset_calls++;
    s_cpufastset_words += count;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t v = fill ? bus_read32(bus, src)
                          : bus_read32(bus, src + 4 * i);
        bus_write32(bus, dst + 4 * i, v);
    }
    return 1;
}
