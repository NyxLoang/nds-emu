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
/* 21-B9yi(续71)：随机输入浸泡（seed=0 时用 1） */
void runner_set_keys_random(runner_t *r, uint64_t frame, uint32_t seed,
                            uint64_t period);
void runner_set_touch_random(runner_t *r, uint64_t frame, uint32_t seed,
                             uint64_t period);
void runner_set_keys_random_series(uint64_t frame, uint32_t seed, uint64_t period);
void runner_set_touch_random_series(uint64_t frame, uint32_t seed, uint64_t period);
/* 21-B9yi(续75)：每 N 帧给双屏画面算指纹并打印（统计「跑过多少张不同画面」）。 */
void runner_set_hash_series(uint64_t every);
/* 21-B9yi(续46)：触摸注入脚本（屏幕像素坐标；period=0 只点一次，否则每 period
   帧点一次、每次按住 12 帧）。 */
void runner_set_touch(runner_t *r, uint64_t frame, int x, int y, uint64_t period);
/* 21-B9yi(续67)：一次拖拽手势（底屏像素坐标；steps=分几帧移动完）。 */
void runner_set_touch_drag(runner_t *r, uint64_t frame, int x1, int y1,
                           int x2, int y2, int steps, uint64_t period);
/* 21-B9yi(续46)：CLI 配置（headless 入口创建 runner 后自动套用）。 */
void runner_set_touch_series(uint64_t frame, int x, int y, uint64_t period);
void runner_set_touch_drag_series(uint64_t frame, int x1, int y1, int x2, int y2,
                                  int steps, uint64_t period);
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

/* 21-B9xj：总线写监视（把写入者身份/PC 打出来，用于“谁改了这个寄存器”）。
   idx 0-3；lo==hi 表示该组关闭。配合 CLI `--watch LO-HI` 使用。 */
void runner_set_watch(int idx, uint32_t lo, uint32_t hi);
/* 21-B9xk：总线读监视（把读取者与读到的值打出来）。 */
void runner_set_watch_read(int idx, uint32_t lo, uint32_t hi);
/* 21-B9xu：每 every 帧把画面存成 `<prefix>_NNNNN.bmp`（画面时间线对照用）。 */
void runner_set_shot_series(uint64_t every, const char *prefix);
/* 21-B9yi(续32)：每 every 帧打印一次双屏画面统计（非黑比例/均值 RGB）。 */
void runner_set_stats_series(uint64_t every);

/* 21-B9yi(续83)：窗口模式退出时用的两个「人工验收判据」接口。
   savechip_report：打印存档芯片状态（类型/大小/非 0xFF 字节数/哈希）——
   与无头同一口径，人工在游戏里存过档后 nonzero 会明显大于 24。
   save_screenshot：把当前双屏帧缓冲写成 24 位 BMP（顶屏在上），
   与无头 `--shot` 完全同口径，便于人工留存证据/对照。 */
void runner_savechip_report(const struct nds *nds);
int runner_save_screenshot(const struct nds *nds, const char *path);

#endif /* NDS_EMU_RUNNER_H */
