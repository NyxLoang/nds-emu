#ifndef NDS_EMU_RUNNER_H
#define NDS_EMU_RUNNER_H

#include <stdint.h>

struct nds; /* 前向声明：避免 runner.h 依赖 nds.h */

/* 阶段 21 bring-up：headless 跑 N 步，只打印诊断事件与周期进度，最后打印两核状态。
   用于在不开窗口/音频的情况下定位 ROM 的第一个卡点。 */
void runner_headless(struct nds *nds, uint64_t steps, int trace);

#endif /* NDS_EMU_RUNNER_H */
