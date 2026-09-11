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

/* 21-B9wq：持久化帧驱动调度器（窗口模式与 headless-frames 共用同一套
   事件/周期成本模型）。create 后按帧调用 runner_run_frame。 */
typedef struct runner runner_t;

runner_t *runner_create(struct nds *nds);
void runner_destroy(runner_t *r);
void runner_set_keys(runner_t *r, uint64_t frame, uint32_t mask,
                     uint64_t period);
uint64_t runner_frame_index(const runner_t *r);
uint64_t runner_now(const runner_t *r);
/* 推进到下一帧；返回 1=已跨过一帧，0=无后续硬件事件/安全上限。 */
int runner_run_frame(runner_t *r);
/* 连续推进到 frame_index >= target_frame。 */
int runner_run_to_frame(runner_t *r, uint64_t target_frame);
/* headless-frames 入口：按帧运行并输出摘要/截图。 */
void runner_headless_frames(struct nds *nds, uint64_t frames,
                            const char *shot_path,
                            uint64_t key_frame, uint32_t key_mask,
                            uint64_t key_period);

#endif /* NDS_EMU_RUNNER_H */
