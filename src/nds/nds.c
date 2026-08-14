#include <stdlib.h>
#include "nds.h"
#include "cpu/cpu.h"
#include "cpu/arm9.h"
#include "cpu/arm7.h"
#include "io/io.h"

nds_t *nds_create(void)
{
    nds_t *nds = calloc(1, sizeof(nds_t));
    if (nds == NULL)
        return NULL;

    /* 总线是其它模块的依赖，先建；失败则整体失败 */
    nds->bus = bus_create();
    if (nds->bus == NULL) {
        free(nds);
        return NULL;
    }
    /* IO 寄存器区（中断/定时器/按键/DMA/FIFO）。bus 需要把 IO 地址转发给它，故建立双向联系。 */
    nds->io = io_create();
    if (nds->io == NULL) {
        bus_destroy(nds->bus);
        free(nds);
        return NULL;
    }
    nds->bus->io = nds->io;
    nds->io->bus = nds->bus; /* io 需要 bus 反指（DMA 搬运经 bus 访存），与 bus->io 对称 */
    /* ARM9 CPU：PC 先用 0 占位，装载镜像后由 main 调 cpu_reset 指到入口 */
    nds->cpu = arm9_create(nds);
    if (nds->cpu == NULL) {
        io_destroy(nds->io);
        bus_destroy(nds->bus);
        free(nds);
        return NULL;
    }
    /* ARM7 CPU（阶段 8 双核）：同样 0 占位，装载 ARM7 镜像后 reset 到入口 */
    nds->cpu7 = arm7_create(nds);
    if (nds->cpu7 == NULL) {
        cpu_destroy(nds->cpu);
        io_destroy(nds->io);
        bus_destroy(nds->bus);
        free(nds);
        return NULL;
    }
    return nds;
}

void nds_destroy(nds_t *nds)
{
    if (nds == NULL)
        return;
    /* 逆序清理：后建的先释放（cpu7 → cpu → io → bus） */
    cpu_destroy(nds->cpu7);
    cpu_destroy(nds->cpu);
    io_destroy(nds->io);
    bus_destroy(nds->bus);
    free(nds);
}
