#ifndef NDS_EMU_DEMO_H
#define NDS_EMU_DEMO_H

struct nds; /* 前向声明：避免 demo.h 依赖 nds.h */

/* 无 ROM 时的演示脚手架：顶屏 2D tile 棋盘格 + OBJ，底屏竖条纹，加一路音频提示音。
   仅用于验收显示/音频链路；有 ROM 时由 ROM 自己驱动画面，不调用本函数。 */
void demo_setup(struct nds *nds);

#endif /* NDS_EMU_DEMO_H */
