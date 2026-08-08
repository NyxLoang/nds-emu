#ifndef NDS_EMU_NDS_H
#define NDS_EMU_NDS_H

#include "bus/bus.h"

typedef struct arm_cpu arm_cpu_t; /* 前向声明，避免 nds.h 依赖 cpu.h */

/* 整机状态：一台 NDS 的所有硬件（bus / cpu / ppu / cart …）最终都挂在这里。
   阶段 3a：bus 与 cpu 已挂入；后续微步加 ppu 等。 */
typedef struct nds {
    bus_t *bus;          /* 内存总线：CPU 通过它访问 Main RAM / VRAM / IO */
    arm_cpu_t *cpu;      /* ARM9 处理器 */
} nds_t;

/* 创建一台机器并初始化各模块。失败返回 NULL。 */
nds_t *nds_create(void);

/* 逆序清理各模块并释放。 */
void nds_destroy(nds_t *nds);

#endif /* NDS_EMU_NDS_H */
