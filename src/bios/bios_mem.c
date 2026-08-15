#include <stdint.h>
#include "bios_mem.h"
#include "cpu/cpu.h"
#include "bus/bus.h"

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

    for (uint32_t i = 0; i < count; i++) {
        uint32_t v = fill ? bus_read32(bus, src)
                          : bus_read32(bus, src + 4 * i);
        bus_write32(bus, dst + 4 * i, v);
    }
    return 1;
}
