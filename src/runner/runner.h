#ifndef NDS_EMU_RUNNER_H
#define NDS_EMU_RUNNER_H

#include <stdint.h>

struct nds; /* 前向声明：避免 runner.h 依赖 nds.h */

/* 阶段 21 bring-up：headless 跑 N 步，只打印诊断事件与周期进度，最后打印两核状态。
   用于在不开窗口/音频的情况下定位 ROM 的第一个卡点。
   shot_path 非空时把跑完后的双屏帧缓冲写成 24 位 BMP（顶屏在上，256×384）。 */
void runner_headless(struct nds *nds, uint64_t steps, int trace,
                     const char *shot_path);
/* 事件目标 headless（周期成本模型实验驱动，固定 2:1 保留在上方函数） */
void runner_headless_cycles(struct nds *nds, uint64_t steps, int trace,
                            const char *shot_path,
                            uint64_t key_frame, uint32_t key_mask,
                            uint64_t key_period);

#endif /* NDS_EMU_RUNNER_H */
