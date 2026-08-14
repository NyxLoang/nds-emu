#include "arm7.h"

arm_cpu_t *arm7_create(nds_t *nds)
{
    /* ARM7 复位后 PC 由镜像装载流程经 cpu_reset 设置，此处先用 0 占位 */
    return cpu_create(nds, 0, 1);
}
