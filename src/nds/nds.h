#ifndef NDS_EMU_NDS_H
#define NDS_EMU_NDS_H

#include "bus/bus.h"

/* 整机状态：一台 NDS 的所有硬件（bus / cpu / ppu / cart …）最终都挂在这里。
   阶段 2：bus 已挂入；后续微步加 cpu / ppu 等。 */
typedef struct nds {
    bus_t *bus; /* 内存总线：CPU 通过它访问 Main RAM / VRAM / IO */
} nds_t;
/* 创建一台机器并初始化各模块。失败返回 NULL。 */
nds_t *nds_create(void);

/* 逆序清理各模块并释放。 */
void nds_destroy(nds_t *nds);

#endif /* NDS_EMU_NDS_H */
