#ifndef NDS_EMU_NDS_H
#define NDS_EMU_NDS_H

/* 整机状态：一台 NDS 的所有硬件（bus / cpu / ppu / cart …）最终都挂在这里。
   现阶段为空结构体，先把「一台机器」这个概念立起来。 */
typedef struct nds {
    /* TODO(阶段 2+)：bus、cpu、ppu、cart … */
} nds_t;

/* 创建 / 销毁一台机器。目前无内部状态，后续在 create 里初始化各模块。 */
nds_t *nds_create(void);
void nds_destroy(nds_t *nds);

#endif /* NDS_EMU_NDS_H */
