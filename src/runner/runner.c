#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nds/nds.h"      /* nds_t / bus_t */
#include "bus/bus.h"      /* bus_set_diag */

/* 21-B9xi：诊断用的“当前帧号”（定义在 io.c，供 IPC 报文记录带帧号）。 */
extern unsigned long long g_dbg_frame;

/* 21-B9xj：CLI `--watch LO-HI` 配置的写监视区间（最多 4 组）。 */
static uint32_t s_watch_lo[4];
static uint32_t s_watch_hi[4];
/* 21-B9xu：`--shot-every N --shot-prefix P` 每 N 帧存一张截图（画面时间线对照） */
static uint64_t s_shot_every = 0;
static const char *s_shot_prefix = NULL;
/* 21-B9yi(续32)：`--stats-every N` 每 N 帧打印双屏画面统计
   （非黑像素比例 + 均值 RGB），与参考核 harness 的同名统计口径一致，
   用于客观判定“画面是在推进还是定格”。 */
static uint64_t s_stats_every = 0;
/* 21-B9yi(续75)：`--screen-hash-every N` 的画面指纹间隔（见 runner_headless_frames） */
static uint64_t s_hash_every = 0;

void runner_set_hash_series(uint64_t every)
{
    s_hash_every = every;
}
/* 21-B9yi(续46)：`--touch-frame/-x/-y/-period` 配置的触摸注入脚本 */
static int s_touch_on = 0;
static uint64_t s_touch_frame = 0, s_touch_period = 0;
static int s_touch_x = 128, s_touch_y = 96;
/* 21-B9yi(续67)：`--touch-drag X1,Y1,X2,Y2 --touch-drag-steps N` 配置的拖拽注入 */
static int s_drag_on = 0;
static int s_drag_x1 = 96, s_drag_y1 = 96, s_drag_x2 = 160, s_drag_y2 = 96;
static int s_drag_steps = 12;

void runner_set_touch_drag_series(uint64_t frame, int x1, int y1, int x2, int y2,
                                  int steps, uint64_t period)
{
    s_drag_on = 1;
    s_touch_frame = frame;
    s_touch_period = period;
    s_drag_x1 = x1; s_drag_y1 = y1;
    s_drag_x2 = x2; s_drag_y2 = y2;
    s_drag_steps = (steps > 0) ? steps : 12;
}

/* 21-B9yi(续71)：随机按键/随机触摸的 CLI 配置（见 runner_keys / runner_touch）。 */
static int s_keyrandom_on;
static uint32_t s_keyrandom_seed = 1;
static uint64_t s_keyrandom_frame, s_keyrandom_period = 60;
static int s_touchrandom_on;
static uint32_t s_touchrandom_seed = 1;
static uint64_t s_touchrandom_frame, s_touchrandom_period = 90;

void runner_set_keys_random_series(uint64_t frame, uint32_t seed, uint64_t period)
{
    s_keyrandom_on = 1;
    s_keyrandom_frame = frame;
    s_keyrandom_seed = seed ? seed : 1u;
    s_keyrandom_period = (period > 0) ? period : 60;
}

void runner_set_touch_random_series(uint64_t frame, uint32_t seed, uint64_t period)
{
    s_touchrandom_on = 1;
    s_touchrandom_frame = frame;
    s_touchrandom_seed = seed ? seed : 1u;
    s_touchrandom_period = (period > 0) ? period : 90;
}

void runner_set_touch_series(uint64_t frame, int x, int y, uint64_t period)
{
    s_touch_on = 1;
    s_touch_frame = frame;
    s_touch_x = x;
    s_touch_y = y;
    s_touch_period = period;
}

void runner_set_stats_series(uint64_t every)
{
    s_stats_every = every;
}

void runner_set_shot_series(uint64_t every, const char *prefix)
{
    s_shot_every = every;
    s_shot_prefix = prefix;
}
static uint32_t s_watch_r_lo[4];
static uint32_t s_watch_r_hi[4];

void runner_set_watch(int idx, uint32_t lo, uint32_t hi)
{
    if (idx < 0 || idx >= 4)
        return;
    s_watch_lo[idx] = lo;
    s_watch_hi[idx] = hi;
}

void runner_set_watch_read(int idx, uint32_t lo, uint32_t hi)
{
    if (idx < 0 || idx >= 4)
        return;
    s_watch_r_lo[idx] = lo;
    s_watch_r_hi[idx] = hi;
}

static void runner_apply_watch(nds_t *nds)
{
    for (int i = 0; i < 4; i++) {
        bus_set_watch(nds->bus, i, s_watch_lo[i], s_watch_hi[i]);
        bus_set_watch_read(nds->bus, i, s_watch_r_lo[i], s_watch_r_hi[i]);
    }
}
#include "cpu/cpu.h"      /* cpu_step / arm_cpu_t（r / cycles） */
#include "cpu/exec.h"     /* exec_set_trace */
#include "cpu/thumb.h"    /* thumb_set_trace */
#include "io/io.h"        /* io_set_vblank */
#include "ppu/render.h"   /* render_frame：双屏纯软件出图 */
#include "snd/snd.h"      /* snd_advance / snd_host_render_active（21-B9wu） */
#include "io/rtc.h"       /* rtc_advance_seconds（21-B9wx） */
#include "bios/bios7_low.h" /* bios7_low_dump_hist（21-B9ye） */
#include "runner.h"
#include "timing/timing.h"

/* 事件目标 headless：把扫描线/VBlank 挂到 timing 事件表上。 */
typedef struct runner_ev_ctx {
    io_t *io;
    timing_t *tm;
    uint64_t next_line;
    uint64_t next_frame;
    uint64_t line_cycles;
    uint64_t frame_cycles;
} runner_ev_ctx_t;

static int save_bmp(const char *path, const uint32_t *fb_top,
                    const uint32_t *fb_bot);

static void runner_ev_line(void *ctx)
{
    runner_ev_ctx_t *c = (runner_ev_ctx_t *)ctx;
    io_advance_scanline(c->io);
    /* 21-B9xc：真机 VBlank 从第 192 扫描线开始（263 行/帧，192-262 为 VBlank） */
    if (c->io->vcount == 192u)
        io_set_vblank(c->io);
    c->next_line += c->line_cycles;
    timing_arm(c->tm, 0, c->next_line - c->tm->now, runner_ev_line, c);
}

static void runner_ev_frame(void *ctx)
{
    runner_ev_ctx_t *c = (runner_ev_ctx_t *)ctx;
    io_frame_boundary(c->io);
    c->next_frame += c->frame_cycles;
    timing_arm(c->tm, 1, c->next_frame - c->tm->now, runner_ev_frame, c);
}

/* 21-B9wq：持久化帧驱动调度器。窗口模式与 headless-frames 共用同一套
   事件/周期成本模型；每次 runner_run_frame 推进一个 VBlank 帧。 */
struct runner {
    nds_t *nds;
    timing_t tm;
    runner_ev_ctx_t ctx;
    uint64_t frame_cycles;
    uint64_t cost9, cost7;
    int a9_wait, a7_wait;
    uint64_t last_now;
    uint64_t key_frame, key_period, next_press;
    uint32_t key_mask;
    int key_down;
    uint64_t key_release_frame;
    /* 21-B9yi(续46)：触摸注入脚本（屏幕像素坐标 → ADC，按固件默认校准换算） */
    int touch_x, touch_y;
    uint64_t touch_frame, touch_period, next_touch;
    uint64_t touch_release_frame;
    int touch_down;
    int touch_enabled;
    /* 21-B9yi(续67)：拖拽注入状态（见 runner_touch） */
    int drag_on, drag_x1, drag_y1, drag_x2, drag_y2, drag_steps, drag_step;
    int drag_active;              /* 拖拽进行中（按下到抬起之间） */
    uint64_t drag_start_frame;    /* 按下时的帧号（步进按帧计） */
    /* 21-B9yi(续71)：随机输入浸泡（见 runner_keys / runner_touch） */
    int key_random;
    uint32_t key_seed;
    int touch_random;
    uint32_t touch_seed;
    uint64_t snd_done;        /* 21-B9wu：已推进的音频样本数（无头模式补推用） */
    uint64_t rtc_done;        /* 21-B9wx：已推进的 RTC 秒数 */
};

#define RUNNER_WAKE7_IO(io_) (((io_)->irq[1].ie & (io_)->irq[1].ifl) != 0)
#define RUNNER_WAKE9_IO(io_) irq_pending(&(io_)->irq[0])

/* 21-B9yg：ARM9 每条指令折算成多少个「系统时钟单位」（33.513982MHz）。
   参考核（melonDS）按 1 指令 ≈ 1 单位计费（另有 MemTimings 取指代价）；
   本地旧口径是 ÷2（把 ARM9 当成 2× ARM7 时钟的「硬件周期」计数），实测
   ARM9 每帧指令数达参考核的 2.25 倍（frame 8：本地 758 万 vs 参考 337 万），
   于是开机时间线整体提前（参考核第 26 帧才开显示，本地第 ~13 帧就开了）。
   默认改为 ÷1；`NDS_ARM9_DIV=2` 可切回旧口径做 A/B。 */
/* 21-B9yi(续64)：**热点修正** —— 原来是 `(c) / s_arm9_div`，除数是**运行时变量**，
   编译器只能生成 64 位除法指令（~20-40 周期），而它在**每条指令**的调度路径上
   被调用 3 次（选择 ARM9/ARM7 该走谁、以及推进系统时间）。口径只有 1 或 2 两种，
   直接换成移位：÷1 = 不移、÷2 = 右移 1 位（无符号除法等价）。 */
static unsigned s_arm9_shift;             /* 0 = ÷1（默认）、1 = ÷2（旧口径 A/B） */
#define RUNNER_SYS9(c) ((uint64_t)(c) >> s_arm9_shift)

/* 21-B9wz 诊断：整段运行的 ARM9 累积热点（hot9: 前 16 + 占比 + 总数）。
   和参考核的 ins9/hist9 对照用：两边热点集合一致说明代码路径相同，
   差异只可能在时序/外设状态。 */
static uint32_t s_h9_pc[1 << 16];
static uint64_t s_h9_cnt[1 << 16];
static uint64_t s_h9_total;
/* 21-B9yi(续41)：热点 PC 直方图是**诊断**设施，此前**每条 ARM9 指令**都在更新
   （64KB 数组的随机写 + 计数，缓存局部性极差），是解释器最热的纯开销之一。
   改成只在 `NDS_PCHOT=1` 时统计（默认关闭）。 */
static int s_h9_on = -2;

static int h9_enabled(void)
{
    if (s_h9_on == -2)
        s_h9_on = (getenv("NDS_PCHOT") != NULL) ? 1 : 0;
    return s_h9_on;
}

runner_t *runner_create(nds_t *nds)
{
    if (nds == NULL)
        return NULL;
    runner_t *r = (runner_t *)calloc(1, sizeof(runner_t));
    if (r == NULL)
        return NULL;
    {   /* 21-B9yg：ARM9 时钟折算口径（默认 1；NDS_ARM9_DIV=2 切回旧口径做 A/B） */
    const char *e = getenv("NDS_ARM9_DIV");
    s_arm9_shift = (e != NULL && e[0] == '2') ? 1u : 0u;   /* 见 RUNNER_SYS9 注释 */
    }
    r->nds = nds;
    r->frame_cycles = 560190;
    uint64_t line_cycles = r->frame_cycles / 263;
    timing_init(&r->tm);
    r->ctx.io = nds->io;
    r->ctx.tm = &r->tm;
    r->ctx.line_cycles = line_cycles;
    r->ctx.frame_cycles = r->frame_cycles;
    r->ctx.next_line = line_cycles;
    r->ctx.next_frame = r->frame_cycles;
    timing_arm(&r->tm, 0, line_cycles, runner_ev_line, &r->ctx);
    timing_arm(&r->tm, 1, r->frame_cycles, runner_ev_frame, &r->ctx);
    r->next_press = UINT64_MAX;
    return r;
}

void runner_destroy(runner_t *r)
{
    free(r);
}

void runner_set_keys(runner_t *r, uint64_t frame, uint32_t mask,
                     uint64_t period)
{
    if (r == NULL)
        return;
    r->key_frame = frame;
    r->key_period = period;
    r->key_mask = mask;
    r->key_down = 0;
    r->key_release_frame = 0;
    r->next_press = (mask != 0) ? frame : UINT64_MAX;
}

/* 21-B9yi(续71)：随机按键/触摸浸泡（每次触发取随机值，见 runner_keys/runner_touch） */
void runner_set_keys_random(runner_t *r, uint64_t frame, uint32_t seed,
                            uint64_t period)
{
    if (r == NULL)
        return;
    r->key_random = 1;
    r->key_mask = 0;
    r->key_seed = seed ? seed : 1u;
    r->key_period = (period > 0) ? period : 60;
    r->key_down = 0;
    r->key_release_frame = 0;
    r->next_press = frame;
}

void runner_set_touch_random(runner_t *r, uint64_t frame, uint32_t seed,
                             uint64_t period)
{
    if (r == NULL)
        return;
    r->touch_enabled = 1;
    r->touch_random = 1;
    r->touch_seed = seed ? seed : 1u;
    r->touch_period = (period > 0) ? period : 90;
    r->touch_down = 0;
    r->next_touch = frame;
}

uint64_t runner_frame_index(const runner_t *r)
{
    return (r != NULL) ? (r->tm.now / r->frame_cycles) : 0;
}

uint64_t runner_now(const runner_t *r)
{
    return (r != NULL) ? r->tm.now : 0;
}

/* 21-B9yi(续77)：打印**存档芯片状态** —— 判断「游戏有没有在运行中真的写过存档」。
   打印类型/大小/非 0xFF 字节数（擦除态=0xFF）与内容哈希；与初始态不同即说明写过。
   （此前这一项只能靠人工进菜单存档确认。）
   21-B9yi(续83)：改为公开接口，两个 headless 汇总路径 + **窗口退出摘要**都调用它，
   这样「人工在游戏里存一次档」的验收有客观输出（nonzero 会从 24 明显增大）。 */
void runner_savechip_report(const struct nds *nds)
{
    save_t *sv = io_get_save(nds->io);
    if (sv == NULL || sv->data == NULL || sv->size == 0)
        return;
    uint64_t h = 1469598103934665603ull;
    size_t nz = 0;
    for (size_t i = 0; i < sv->size; i++) {
        h = (h ^ sv->data[i]) * 1099511628211ull;
        if (sv->data[i] != 0xFFu)
            nz++;
    }
    printf("savechip: type=%d size=%zu nonzero(vs 0xFF)=%zu hash=%016llX\n",
           (int)sv->type, sv->size, nz, (unsigned long long)h);
}

/* 21-B9yi(续83)：把当前双屏帧缓冲写成 BMP（顶屏在上）——与无头 `--shot` 同口径。
   供窗口模式在退出时用，人工验收可以顺手留一张「我看到的最后一屏」。
   soft 渲染：不依赖 SDL，窗口已关也能用。返回 0 = 成功。 */
int runner_save_screenshot(const struct nds *nds, const char *path)
{
    if (nds == NULL || path == NULL)
        return -1;
    uint32_t *fb_top = (uint32_t *)malloc(sizeof(uint32_t) * RENDER_SCREEN_W
                                          * RENDER_SCREEN_H);
    uint32_t *fb_bot = (uint32_t *)malloc(sizeof(uint32_t) * RENDER_SCREEN_W
                                          * RENDER_SCREEN_H);
    if (fb_top == NULL || fb_bot == NULL) {
        free(fb_top);
        free(fb_bot);
        return -1;
    }
    render_frame(nds->bus, fb_top, fb_bot);
    int rc = save_bmp(path, fb_top, fb_bot);
    free(fb_top);
    free(fb_bot);
    return rc;
}

/* 帧号到达脚本时刻时注入按键，保持 8 帧后释放（游戏按帧轮询）。 */
static void runner_keys(runner_t *r)
{
    /* 21-B9yi(续71)：**随机按键浸泡**（`--key-random SEED`）。
       固定脚本只能走游戏的一条固定路径；随机输入用来「撞」出没走过的代码/未实现
       路径（这是模拟器常见的 soak 测试）。每次触发时用种子 LCG 取一个新掩码。 */
    if (r->key_random) {
        uint64_t fr = runner_frame_index(r);
        if (r->key_down) {
            if (fr >= r->key_release_frame) {
                io_set_keyinput(r->nds->io, 0);
                r->key_down = 0;
                r->next_press = (r->key_period > 0) ? (fr + r->key_period) : UINT64_MAX;
            }
            return;
        }
        if (fr >= r->next_press) {
            r->key_seed = r->key_seed * 1664525u + 1013904223u;   /* LCG */
            /* 只取真实存在的 12 个键位（bit0-11），并强制非 0 */
            uint16_t m = (uint16_t)((r->key_seed >> 8) & 0xFFFu);
            if (m == 0)
                m = 0x001u;
            io_set_keyinput(r->nds->io, m);
            r->key_mask = m;
            r->key_down = 1;
            r->key_release_frame = fr + 12;
        }
        return;
    }
    if (r->key_mask == 0)
        return;
    uint64_t fr = runner_frame_index(r);
    if (r->key_down) {
        if (fr >= r->key_release_frame) {
            io_set_keyinput(r->nds->io, 0);
            r->key_down = 0;
            if (r->key_period == 0)
                r->next_press = UINT64_MAX;
            else
                r->next_press += r->key_period;
        }
    } else if (fr >= r->next_press) {
        io_set_keyinput(r->nds->io, (uint16_t)r->key_mask);
        r->key_down = 1;
        /* 21-B9xv：持续 12 帧（与参考 harness 的 ((frame-1700)%120)<12 对齐） */
        r->key_release_frame = fr + 12;
        printf("runner: key mask=%04X at frame=%llu\n", r->key_mask,
               (unsigned long long)fr);
    }
}

/* 一次调度迭代：返回 0 表示没有后续硬件事件，无法继续推进。 */
static void runner_touch(runner_t *r);   /* 21-B9yi(续46)：触摸注入（定义在下方） */
static int runner_step(runner_t *r)
{
    nds_t *nds = r->nds;
    int was9 = r->a9_wait, was7 = r->a7_wait;

    if (r->a9_wait && RUNNER_WAKE9_IO(nds->io)) {
        r->a9_wait = 0;
        /* 21-B9yi(续64)：与 RUNNER_SYS9 配套 —— 折算口径 1 或 2 用移位表达 */
        r->cost9 = r->tm.now << s_arm9_shift;
    }
    if (r->a7_wait && RUNNER_WAKE7_IO(nds->io)) {
        r->a7_wait = 0;
        r->cost7 = r->tm.now;
    }

    if (r->a9_wait && r->a7_wait) {
        uint64_t next = timing_next(&r->tm);
        if (next == UINT64_MAX)
            return 0;
        timing_advance(&r->tm, next);
        uint64_t delta = r->tm.now - r->last_now;
        r->last_now = r->tm.now;
        if (delta != 0) {
            io_advance_timers(nds->io, 0, (uint32_t)delta);
            io_advance_timers(nds->io, 1, (uint32_t)delta);
            /* 21-B9yi：两核都空闲时也要推进卡带时钟——否则一笔「数据取完、
               等软件读走」的传输永远结束不了，`end_irq`（传输完成中断）
               也送不出去，游戏任务就此睡死（实测卡在空闲任务、IF bit19 不亮）。 */
            io_advance_cart(nds->io, 0, (uint32_t)delta);
        }
        runner_keys(r);
        return 1;
    }

    int step7;
    if (r->a9_wait)
        step7 = 1;
    else if (r->a7_wait)
        step7 = 0;
    else
        step7 = (RUNNER_SYS9(r->cost9) > r->cost7);
    if (step7) {
        cpu_step(nds->cpu7);
        r->a7_wait = (nds->cpu7->step_cycles == 0);
        if (!r->a7_wait) r->cost7 += nds->cpu7->step_cycles;
    } else {
        cpu_step(nds->cpu);
        r->a9_wait = (nds->cpu->step_cycles == 0);
        if (!r->a9_wait) r->cost9 += nds->cpu->step_cycles;
        if (s_h9_on != 0) {
            uint32_t pc = nds->cpu->r[15];
            uint32_t h = (pc >> 4) & 0xFFFFu;
            s_h9_pc[h] = pc;
            s_h9_cnt[h]++;
            s_h9_total++;
        }
    }

    uint64_t sys;
    if (r->a9_wait)
        sys = r->cost7;
    else if (r->a7_wait)
        sys = RUNNER_SYS9(r->cost9);
    else
        sys = (RUNNER_SYS9(r->cost9) < r->cost7) ? RUNNER_SYS9(r->cost9) : r->cost7;
    timing_advance(&r->tm, sys);
    uint64_t delta = r->tm.now - r->last_now;
    r->last_now = r->tm.now;
    if (delta != 0) {
        if (was9) io_advance_timers(nds->io, 0, (uint32_t)delta);
        if (was7) io_advance_timers(nds->io, 1, (uint32_t)delta);
    }
    runner_keys(r);
    runner_touch(r);      /* 21-B9yi(续46)：触摸注入脚本 */
    return 1;
}

int runner_run_frame(runner_t *r)
{
    if (r == NULL)
        return 0;
    uint64_t target = runner_frame_index(r) + 1;
    uint64_t guard = 0;
    while (runner_frame_index(r) < target) {
        if (!runner_step(r))
            return 0;
        /* 安全上限：正常情况下一个帧远低于该迭代数。 */
        if (++guard > 50000000ull)
            return 0;
    }
    /* 21-B9wu：把这一帧的音频时间补给 SPU。NDS 的 32768Hz = 33.51MHz/1024，
       无头模式没有 SDL 回调，不补的话游戏轮询 SOUNDxCNT 播放状态会死等
       （FFXII 开场 ARM7 就卡在这种轮询上）。 */
    if (!snd_host_render_active()) {
        uint64_t want = r->tm.now / 1024ull;
        if (want > r->snd_done) {
            uint64_t delta = want - r->snd_done;
            if (delta > 4096ull)
                delta = 4096ull;
            snd_advance(&r->nds->io->snd, r->nds->bus, (uint32_t)delta);
            r->snd_done = want;
        }
    }
    /* 21-B9wx：RTC 走时（1 秒 = 33513982 个 ARM9 周期）。游戏读日期/时间时
       会看到连续递增的时间，不会因为秒数永远不变而卡住。 */
    {
        uint64_t want = r->tm.now / 33513982ull;
        if (want > r->rtc_done) {
            uint64_t delta = want - r->rtc_done;
            if (delta > 600ull)
                delta = 600ull;
            rtc_advance_seconds(&r->nds->io->rtc, (uint32_t)delta);
            r->rtc_done = want;
        }
    }
    return 1;
}

int runner_run_to_frame(runner_t *r, uint64_t target_frame)
{
    if (r == NULL)
        return 0;
    while (runner_frame_index(r) < target_frame) {
        if (!runner_run_frame(r))
            return 0;
    }
    return 1;
}

void runner_headless_cycles(nds_t *nds, uint64_t steps, int trace,
                            const char *shot_path,
                            uint64_t key_frame, uint32_t key_mask,
                            uint64_t key_period)
{
    bus_set_diag(nds->bus, 1);
    exec_set_trace(trace);
    thumb_set_trace(trace);

    timing_t tm;
    timing_init(&tm);
    const uint64_t frame_cycles = 560190;
    const uint64_t line_cycles = frame_cycles / 263;
    runner_ev_ctx_t ctx = { nds->io, &tm, line_cycles, frame_cycles,
                            line_cycles, frame_cycles };
    timing_arm(&tm, 0, line_cycles, runner_ev_line, &ctx);
    timing_arm(&tm, 1, frame_cycles, runner_ev_frame, &ctx);

    /* 21-B9wf：ARM7 的 HALTCNT 暂停由 (IF&IE) 唤醒（不要求 IME），
       ARM9 WFI 仍要求 irq_pending（IME=1 才可能唤醒）。 */
#define RUNNER_WAKE7(io_) (((io_)->irq[1].ie & (io_)->irq[1].ifl) != 0)
#define RUNNER_WAKE9(io_) irq_pending(&(io_)->irq[0])

    uint64_t cost9 = 0, cost7 = 0;
    int a9_wait = 0, a7_wait = 0;
    uint64_t i = 0;
    uint64_t next_press = key_frame;
    int key_hold = 0;
    uint64_t last_now = 0;
    while (i < steps) {
        int was9 = a9_wait, was7 = a7_wait;
        /* 等待本身不消耗指令，但会消耗系统时间：唤醒时把该核的已用周期
           跳到当前系统时间 tm.now（ARM9 时钟为 ARM7 的 2 倍），否则调度器
           会把“暂停期间流逝的时间”误当成积压指令连续补跑，导致 ARM7
           沿主存跑飞（21-B9wf 复现并修复）。 */
        if (a9_wait && RUNNER_WAKE9(nds->io)) {
            a9_wait = 0;
            cost9 = tm.now * 2;
        }
        if (a7_wait && RUNNER_WAKE7(nds->io)) {
            a7_wait = 0;
            cost7 = tm.now;
        }
        if (a9_wait && a7_wait) {
            /* 两核都在低功耗等待：不能空转，推进到下一个硬件事件（扫描线/
               VBlank），事件回调会把对应 IF 置位唤醒核心。 */
            uint64_t next = timing_next(&tm);
            if (next == UINT64_MAX)
                break;
            timing_advance(&tm, next);
            {
                uint64_t delta = tm.now - last_now;
                last_now = tm.now;
                if (delta != 0) {
                    io_advance_timers(nds->io, 0, (uint32_t)delta);
                    io_advance_timers(nds->io, 1, (uint32_t)delta);
                }
            }
            i++;
            if ((i & 0xFFFFFu) == 0xFFFFFu) {
                printf("headless-cyc: step=%llu both-wait now=%llu next=%llu"
                       " line=%llu frame=%llu\n",
                       (unsigned long long)(i + 1),
                       (unsigned long long)tm.now,
                       (unsigned long long)next,
                       (unsigned long long)ctx.next_line,
                       (unsigned long long)ctx.next_frame);
            }
            continue;
        }
        int step7;
        if (a9_wait)
            step7 = 1;
        else if (a7_wait)
            step7 = 0;
        else
            step7 = (RUNNER_SYS9(cost9) > cost7);
        if (step7) {
            cpu_step(nds->cpu7);
            a7_wait = (nds->cpu7->step_cycles == 0);
            if (!a7_wait) cost7 += nds->cpu7->step_cycles;
        } else {
            cpu_step(nds->cpu);
            a9_wait = (nds->cpu->step_cycles == 0);
            if (!a9_wait) cost9 += nds->cpu->step_cycles;
        }
        /* 单核等待时，时间必须由仍在跑的核推进：若取 min(cost9/2,cost7)，
           等待核的成本停住会把系统时间钉死在原地，VBlank/扫描线事件再也
           到不了期（21-B9wf 修复）。唤醒后另一核的成本落后，会按 2:1
           连续补跑，等价暂停期间时间照常流逝。 */
        uint64_t sys;
        if (a9_wait)
            sys = cost7;
        else if (a7_wait)
            sys = RUNNER_SYS9(cost9);
        else
            sys = (RUNNER_SYS9(cost9) < cost7) ? RUNNER_SYS9(cost9) : cost7;
        timing_advance(&tm, sys);
        {
            uint64_t delta = tm.now - last_now;
            last_now = tm.now;
            if (delta != 0) {
                if (was9) io_advance_timers(nds->io, 0, (uint32_t)delta);
                if (was7) io_advance_timers(nds->io, 1, (uint32_t)delta);
            }
        }
        i++;
        if (key_mask != 0) {
            if (key_hold > 0) {
                key_hold--;
                if (key_hold == 0) {
                    io_set_keyinput(nds->io, 0);
                    next_press += (key_period != 0) ? key_period : UINT64_MAX;
                }
            } else if (tm.now / frame_cycles >= next_press) {
                io_set_keyinput(nds->io, (uint16_t)key_mask);
                key_hold = 8;
                printf("headless-cyc: scripted key mask=%04X at frame=%llu\n",
                       key_mask, (unsigned long long)(tm.now / frame_cycles));
            }
        }
        if ((i & 0xFFFFFu) == 0xFFFFFu) {
            printf("headless-cyc: step=%llu ARM9 PC=%08X cyc=%llu cpsr=%08X if=%08X"
                   " | ARM7 PC=%08X cyc=%llu cpsr=%08X if=%08X\n",
                   (unsigned long long)(i + 1), nds->cpu->r[15],
                   (unsigned long long)nds->cpu->cycles, nds->cpu->cpsr,
                   nds->io->irq[0].ifl,
                   nds->cpu7->r[15], (unsigned long long)nds->cpu7->cycles,
                   nds->cpu7->cpsr, nds->io->irq[1].ifl);
        }
    }
#undef RUNNER_WAKE7
#undef RUNNER_WAKE9
    printf("headless-cyc: done. ARM9 PC=%08X cyc=%llu cpsr=%08X"
           " | ARM7 PC=%08X cyc=%llu cpsr=%08X\n",
           nds->cpu->r[15], (unsigned long long)nds->cpu->cycles,
           nds->cpu->cpsr,
           nds->cpu7->r[15], (unsigned long long)nds->cpu7->cycles,
          nds->cpu7->cpsr);
    uint64_t vram_nz = 0;
    for (size_t vi = 0; vi < BUS_VRAM_SIZE; vi++)
        if (nds->bus->vram[vi]) vram_nz++;
    printf("headless-cyc: summary now=%llu frame=%llu vram-nz=%llu"
           " disp=%08X dispb=%08X irq9=%d irq7=%d\n",
           (unsigned long long)tm.now,
           (unsigned long long)(tm.now / frame_cycles),
           (unsigned long long)vram_nz,
           bus_read32(nds->bus, 0x04000000u),
           bus_read32(nds->bus, 0x04001000u),
        nds->cpu->irq_count, nds->cpu7->irq_count);
    runner_savechip_report(nds);
    if (shot_path != NULL) {
        /* 21-B9y4：打印截图时刻的 DISPCNT，用于与 main 的 dump 时刻对照 */
        printf("shot: t=frame-end DISPCNT=%08X DISPCNT_SUB=%08X\n",
               bus_read32(nds->bus, 0x04000000u),
               bus_read32(nds->bus, 0x04001000u));
        uint32_t *fb_top = (uint32_t *)malloc(sizeof(uint32_t) * RENDER_SCREEN_W
                                              * RENDER_SCREEN_H);
        uint32_t *fb_bot = (uint32_t *)malloc(sizeof(uint32_t) * RENDER_SCREEN_W
                                              * RENDER_SCREEN_H);
        if (fb_top != NULL && fb_bot != NULL) {
            render_frame(nds->bus, fb_top, fb_bot);
            if (save_bmp(shot_path, fb_top, fb_bot) == 0)
                printf("headless-cyc: screenshot saved to %s\n", shot_path);
            else
                printf("headless-cyc: screenshot FAILED (%s)\n", shot_path);
        } else {
            printf("headless-cyc: screenshot OOM\n");
        }
        free(fb_top);
        free(fb_bot);
    }
    fflush(stdout);
    (void)shot_path;
}

/* 21-B9yi(续32)：单屏统计（非黑像素数 + 各通道均值）。本地帧缓冲是
   0x00RRGGBB（与 save_bmp 同口径）；参考核 harness 里用同一套定义，
   两边的「非黑比例」可直接对比。 */
static void runner_screen_stat(const uint32_t *fb, unsigned *nz, unsigned *mr,
                               unsigned *mg, unsigned *mb)
{
    const size_t n = (size_t)RENDER_SCREEN_W * RENDER_SCREEN_H;
    unsigned long long sr = 0, sg = 0, sb = 0;
    unsigned count = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t px = fb[i];
        if ((px & 0x00FFFFFFu) != 0u)
            count++;
        sr += (px >> 16) & 0xFFu;
        sg += (px >> 8) & 0xFFu;
        sb += px & 0xFFu;
    }
    *nz = count;
    *mr = (unsigned)(sr / n);
    *mg = (unsigned)(sg / n);
    *mb = (unsigned)(sb / n);
}

/* 21-B9yi(续46)：触摸注入。
   固件默认校准（touch.c 的 `fw_user_body`）：ADC x1=0x200 ↔ 像素 0x21(33)、
   ADC x2=0xE00 ↔ 像素 0xE1(225)；y 同理由 0x200/0x800 ↔ 33/129。
   ⇒ 每像素 16 个 ADC 单位：`adc = 0x200 + (px - 33) * 16`。
   脚本与按键一致：到点按下、保持 `touch_hold` 帧后抬起；period>0 时周期重复。 */
static uint16_t runner_touch_adc(int px, int base_px, int units)
{
    int v = 0x200 + (px - base_px) * units;
    if (v < 0) v = 0;
    if (v > 0xFFF) v = 0xFFF;
    return (uint16_t)v;
}

static void runner_touch(runner_t *r)
{
    if (!r->touch_enabled)
        return;
    uint64_t fr = runner_frame_index(r);
    /* 21-B9yi(续71)：随机触摸浸泡（`--touch-random SEED`）——每次按下一个随机位置
       （底屏 0..255 × 0..191），与随机按键配合用来撞未走过的 UI 路径。 */
    if (r->touch_random) {
        if (r->touch_down) {
            if (fr >= r->touch_release_frame) {
                io_set_touch(r->nds->io, 0, 0xFFFu, 0);
                r->touch_down = 0;
                r->next_touch = (r->touch_period > 0) ? (fr + r->touch_period)
                                                      : UINT64_MAX;
            }
            return;
        }
        if (fr >= r->next_touch) {
            r->touch_seed = r->touch_seed * 1664525u + 1013904223u;
            int px = (int)((r->touch_seed >> 8) & 0xFFu);
            int py = (int)((r->touch_seed >> 16) % 192u);
            io_set_touch(r->nds->io, runner_touch_adc(px, 33, 16),
                         runner_touch_adc(py, 33, 16), 1);
            r->touch_down = 1;
            r->touch_release_frame = fr + 12;
        }
        return;
    }
    /* 21-B9yi(续67)：**拖拽注入**（`--touch-drag X1,Y1,X2,Y2 --touch-drag-steps N`）。
       战斗里下指令是「从单位拖到目标点」的手势，只点一下验证不了；
       这里按帧推进：按下 → 沿直线分 N 帧移动 → 抬起。坐标同样是底屏像素。 */
    if (r->drag_on) {
        /* 注意：runner_touch() 是**每条指令**调用一次（不是每帧），所以这里必须用
           `runner_frame_index()` 判断「过了几帧」，否则 12 步会在同一帧内瞬间走完，
           游戏只看得到一次「按下又立刻抬起」（实测就是因此完全不响应）。 */
        if (r->drag_active) {
            uint64_t el = fr - r->drag_start_frame;   /* 已经按下几帧 */
            if (el > (uint64_t)r->drag_steps) {
                io_set_touch(r->nds->io, 0, 0xFFFu, 0);   /* 抬起（NDS 惯例 0/0xFFF） */
                r->drag_active = 0;
                r->touch_down = 0;
                if (r->touch_period == 0)
                    r->next_touch = UINT64_MAX;
                else
                    r->next_touch += r->touch_period;
                return;
            }
            if (el != 0) {
                int n = r->drag_steps;
                int px = r->drag_x1 + (r->drag_x2 - r->drag_x1) * (int)el / n;
                int py = r->drag_y1 + (r->drag_y2 - r->drag_y1) * (int)el / n;
                io_set_touch(r->nds->io, runner_touch_adc(px, 33, 16),
                             runner_touch_adc(py, 33, 16), 1);
            }
            return;
        }
        if (fr >= r->next_touch) {
            uint16_t ax = runner_touch_adc(r->drag_x1, 33, 16);
            uint16_t ay = runner_touch_adc(r->drag_y1, 33, 16);
            io_set_touch(r->nds->io, ax, ay, 1);
            r->touch_down = 1;
            r->drag_active = 1;
            r->drag_start_frame = fr;
            r->drag_step = 0;
            printf("runner: drag(%d,%d)->(%d,%d) steps=%d at frame=%llu\n",
                   r->drag_x1, r->drag_y1, r->drag_x2, r->drag_y2,
                   r->drag_steps, (unsigned long long)fr);
        }
        return;
    }
    if (r->touch_down) {
        if (fr >= r->touch_release_frame) {
            io_set_touch(r->nds->io, 0, 0, 0);
            r->touch_down = 0;
            if (r->touch_period == 0)
                r->next_touch = UINT64_MAX;
            else
                r->next_touch += r->touch_period;
        }
    } else if (fr >= r->next_touch) {
        uint16_t ax = runner_touch_adc(r->touch_x, 33, 16);
        uint16_t ay = runner_touch_adc(r->touch_y, 33, 16);
        io_set_touch(r->nds->io, ax, ay, 1);
        r->touch_down = 1;
        r->touch_release_frame = fr + 12;
        printf("runner: touch (%d,%d) adc=(%03X,%03X) at frame=%llu\n",
               r->touch_x, r->touch_y, ax, ay,
               (unsigned long long)fr);
    }
}

void runner_set_touch(runner_t *r, uint64_t frame, int x, int y, uint64_t period)
{
    if (r == NULL)
        return;
    r->touch_enabled = 1;
    r->touch_frame = frame;
    r->touch_x = x;
    r->touch_y = y;
    r->touch_period = period;
    r->next_touch = frame;
    r->touch_down = 0;
    r->touch_release_frame = 0;
}

/* 21-B9yi(续67)：设置一次拖拽手势（`--touch-drag`）。x1,y1→x2,y2 是底屏像素坐标，
   steps 是移动分几帧完成（1 帧 ≈ 1/60 秒；太小可能被游戏当成瞬移）。 */
void runner_set_touch_drag(runner_t *r, uint64_t frame,
                           int x1, int y1, int x2, int y2,
                           int steps, uint64_t period)
{
    if (r == NULL)
        return;
    r->touch_enabled = 1;
    r->drag_on = 1;
    r->drag_active = 0;
    r->drag_start_frame = 0;
    r->drag_x1 = x1;
    r->drag_y1 = y1;
    r->drag_x2 = x2;
    r->drag_y2 = y2;
    r->drag_steps = (steps > 0) ? steps : 1;
    r->drag_step = 0;
    r->touch_frame = frame;
    r->touch_period = period;
    r->next_touch = frame;
    r->touch_down = 0;
}

static void runner_print_screen_stats(uint64_t fr, const uint32_t *fb_t,
                                      const uint32_t *fb_b)
{
    const unsigned total = (unsigned)(RENDER_SCREEN_W * RENDER_SCREEN_H);
    unsigned nzt = 0, rt = 0, gt = 0, bt = 0;
    unsigned nzb = 0, rb = 0, gb = 0, bb = 0;
    runner_screen_stat(fb_t, &nzt, &rt, &gt, &bt);
    runner_screen_stat(fb_b, &nzb, &rb, &gb, &bb);
    printf("stats: f=%llu top nz=%u/%u rgb=%u,%u,%u bot nz=%u/%u"
           " rgb=%u,%u,%u\n",
           (unsigned long long)fr, nzt, total, rt, gt, bt, nzb, total,
           rb, gb, bb);
}

void runner_headless_frames(nds_t *nds, uint64_t frames, const char *shot_path,
                            uint64_t key_frame, uint32_t key_mask,
                            uint64_t key_period)
{
    bus_set_diag(nds->bus, 1);
    exec_set_trace(0);
    thumb_set_trace(0);
    runner_apply_watch(nds);

    runner_t *r = runner_create(nds);
    if (r == NULL) {
        printf("headless-frames: runner_create failed\n");
        return;
    }
    h9_enabled();   /* 21-B9yi(续41)：初始化热点 PC 统计开关（默认关） */
    /* 21-B9yi(续71)：随机按键浸泡优先于固定按键脚本 */
    if (s_keyrandom_on)
        runner_set_keys_random(r, s_keyrandom_frame, s_keyrandom_seed,
                               s_keyrandom_period);
    else
        runner_set_keys(r, key_frame, key_mask, key_period);
    if (s_touchrandom_on)
        runner_set_touch_random(r, s_touchrandom_frame, s_touchrandom_seed,
                                s_touchrandom_period);
    if (s_drag_on)   /* 21-B9yi(续67)：拖拽注入（优先于单击注入） */
        runner_set_touch_drag(r, s_touch_frame, s_drag_x1, s_drag_y1,
                              s_drag_x2, s_drag_y2, s_drag_steps, s_touch_period);
    else if (s_touch_on)  /* 21-B9yi(续46)：触摸注入脚本 */
        runner_set_touch(r, s_touch_frame, s_touch_x, s_touch_y, s_touch_period);
    uint64_t start = runner_frame_index(r);
    for (uint64_t fi = 0; fi < frames; fi++) {
        if (!runner_run_frame(r))
            break;
        uint64_t fr = runner_frame_index(r);
        g_dbg_frame = fr;
        /* 21-B9yi(续32)：画面统计（每 N 帧一行，与参考核 harness 同口径） */
        /* 21-B9yi(续35)：NDS_MAT_FRAME=N → 第 N 帧结束时打印 3D 矩阵快照
           （与参考核 `REF_MAT_FRAME=N` 的 refmat 行同口径对照）。 */
        {
            static long mat_frame = -2;
            static long io_frame = -2;
            if (mat_frame == -2) {
                const char *e = getenv("NDS_MAT_FRAME");
                mat_frame = (e != NULL) ? strtol(e, NULL, 10) : -1;
                const char *e2 = getenv("NDS_IODUMP_FRAME");
                io_frame = (e2 != NULL) ? strtol(e2, NULL, 10) : -1;
            }
            /* 21-B9yi(续36)：NDS_IODUMP_FRAME=N → 打印该帧的 2D 显示寄存器
               （与参考核 `REF_IODUMP_FRAME=N` 的 refio 行同口径对照）。 */
            if (io_frame >= 0 && fr == (uint64_t)io_frame) {
                static const uint32_t regs[] = {
                    0x04000000u, 0x04000008u, 0x0400000Au, 0x0400000Cu, 0x0400000Eu,
                    0x04000010u, 0x04000012u, 0x04000014u, 0x04000016u,
                    0x04001000u, 0x04001008u, 0x0400100Au, 0x0400100Cu, 0x0400100Eu,
                    0x04001010u, 0x04001012u, 0x04001014u, 0x04001016u,
                    0x04000060u, 0x04000064u
                    , 0x04000050u, 0x04000052u, 0x04000054u, 0x0400006Cu,
                    0x04001050u, 0x04001052u, 0x04001054u, 0x0400106Cu,
                    0x04000240u, 0x04000241u, 0x04000242u, 0x04000243u,
                    0x04000244u, 0x04000245u, 0x04000246u, 0x04000247u,
                    0x04000248u
                };
                for (size_t i = 0; i < sizeof regs / sizeof regs[0]; i++)
                    printf("ourio: %08X=%04X\n", regs[i],
                           (unsigned)(bus_read16(nds->bus, regs[i]) & 0xFFFFu));
                fflush(stdout);
            }
            if (mat_frame >= 0 && fr >= (uint64_t)mat_frame
                && fr <= (uint64_t)mat_frame + 20u)
                printf("mat: f=%llu proj=(%lld,%lld,%lld,%lld) pos=(%lld,%lld,%lld,%lld)"
                       " tex=(%lld,%lld,%lld,%lld)\n",
                       (unsigned long long)fr,
                       (long long)nds->io->gx.proj[0], (long long)nds->io->gx.proj[5],
                       (long long)nds->io->gx.proj[15], (long long)nds->io->gx.proj[3],
                       (long long)nds->io->gx.pos[0], (long long)nds->io->gx.pos[5],
                       (long long)nds->io->gx.pos[15], (long long)nds->io->gx.pos[3],
                       (long long)nds->io->gx.tex[0], (long long)nds->io->gx.tex[5],
                       (long long)nds->io->gx.tex[15], (long long)nds->io->gx.tex[3]);
            fflush(stdout);
        }
        if (s_stats_every != 0 && (fr % s_stats_every) == 0) {
            uint32_t *fb_t = (uint32_t *)malloc(sizeof(uint32_t)
                                                * RENDER_SCREEN_W * RENDER_SCREEN_H);
            uint32_t *fb_b = (uint32_t *)malloc(sizeof(uint32_t)
                                                * RENDER_SCREEN_W * RENDER_SCREEN_H);
            if (fb_t != NULL && fb_b != NULL) {
                render_frame(nds->bus, fb_t, fb_b);
                runner_print_screen_stats(fr, fb_t, fb_b);
            }
            free(fb_t);
            free(fb_b);
            fflush(stdout);
        }
        /* 21-B9yi(续75)：**画面指纹**（`--screen-hash-every N`）。
           每 N 帧渲染一次双屏、各算一个 64 位 FNV-1a 哈希并打印。用途：
           ①客观统计「一段运行跑过多少张不同画面」（比 disp-change 更细、
              比逐帧截图便宜）；②比较不同输入策略能把游戏带到多远。 */
        if (s_hash_every != 0 && (fr % s_hash_every) == 0) {
            uint32_t *fb_t = (uint32_t *)malloc(sizeof(uint32_t)
                                                * RENDER_SCREEN_W * RENDER_SCREEN_H);
            uint32_t *fb_b = (uint32_t *)malloc(sizeof(uint32_t)
                                                * RENDER_SCREEN_W * RENDER_SCREEN_H);
            if (fb_t != NULL && fb_b != NULL) {
                render_frame(nds->bus, fb_t, fb_b);
                uint64_t ht = 1469598103934665603ull, hb = ht;
                for (size_t i = 0; i < (size_t)RENDER_SCREEN_W * RENDER_SCREEN_H; i++) {
                    ht = (ht ^ fb_t[i]) * 1099511628211ull;
                    hb = (hb ^ fb_b[i]) * 1099511628211ull;
                }
                printf("screenhash: f=%llu top=%016llX bot=%016llX\n",
                       (unsigned long long)fr, (unsigned long long)ht,
                       (unsigned long long)hb);
            }
            free(fb_t);
            free(fb_b);
            fflush(stdout);
        }
        /* 21-B9xu：画面时间线（每 N 帧一张） */
        if (s_shot_every != 0 && s_shot_prefix != NULL && (fr % s_shot_every) == 0) {
            uint32_t *fb_t = (uint32_t *)malloc(sizeof(uint32_t) * RENDER_SCREEN_W
                                                * RENDER_SCREEN_H);
            uint32_t *fb_b = (uint32_t *)malloc(sizeof(uint32_t) * RENDER_SCREEN_W
                                                * RENDER_SCREEN_H);
            if (fb_t != NULL && fb_b != NULL) {
                char path[512];
                snprintf(path, sizeof path, "%s_%05llu.bmp", s_shot_prefix,
                         (unsigned long long)fr);
                render_frame(nds->bus, fb_t, fb_b);
                save_bmp(path, fb_t, fb_b);
            }
            free(fb_t);
            free(fb_b);
        }
        /* 21-B9xi 诊断：DISPCNT 每次变化都记一行（谁在什么状态下改了显示模式） */
        {
            static uint32_t last_disp = 0xFFFFFFFFu, last_dispb = 0xFFFFFFFFu;
            static uint32_t last_tm1 = 0xFFFFFFFFu;
            static uint32_t last_ie = 0xFFFFFFFFu;
            uint32_t d9 = bus_read32(nds->bus, 0x04000000u);
            uint32_t db = bus_read32(nds->bus, 0x04001000u);
            int prev7 = nds->bus->active_is_arm7;
            nds->bus->active_is_arm7 = 0;
            uint32_t tm1 = bus_read32(nds->bus, 0x04000104u); /* TM1D|TM1CNT */
            uint32_t ie = bus_read32(nds->bus, 0x04000210u);
            nds->bus->active_is_arm7 = prev7;
            /* 只在“控制字/使能”变化时打印（TM1D 计数器每帧都在变，不记录） */
            if ((tm1 & 0xFFFF0000u) != (last_tm1 & 0xFFFF0000u) ||
                ie != last_ie) {
                printf("headless-frames: timer-change f=%llu tm1=%08X (was %08X)"
                       " ie9=%08X (was %08X) ARM9=%08X lr9=%08X\n",
                       (unsigned long long)fr, tm1, last_tm1, ie, last_ie,
                       nds->cpu->r[15], nds->cpu->r[14]);
                fflush(stdout);
                last_tm1 = tm1;
                last_ie = ie;
            }
            if (d9 != last_disp || db != last_dispb) {
                printf("headless-frames: disp-change f=%llu disp9=%08X (was %08X)"
                       " dispb=%08X (was %08X) ARM9=%08X lr9=%08X ARM7=%08X"
                       " fifo=%d/%d gx=%u tri=%u\n",
                       (unsigned long long)fr, d9, last_disp, db, last_dispb,
                       nds->cpu->r[15], nds->cpu->r[14], nds->cpu7->r[15],
                       nds->io->fifo.from7.count, nds->io->fifo.from9.count,
                       nds->io->gx.cmd_count, nds->io->gx.tri_count);
                fflush(stdout);
                last_disp = d9;
                last_dispb = db;
            }
        }
        /* 21-B9yh 诊断：NDS_FTRACE=LO-HI → 该帧区间内逐帧打印两核现场
           （定位「START 之后卡住」这类只在某段帧里发生的死锁）。 */
        {
            static long s_ft_lo = -1, s_ft_hi = -1;
            if (s_ft_lo == -1) {
                const char *e = getenv("NDS_FTRACE");
                s_ft_lo = 0;
                s_ft_hi = -1;
                if (e != NULL) {
                    long lo = 0, hi = 0;
                    if (sscanf(e, "%ld-%ld", &lo, &hi) == 2) {
                        s_ft_lo = lo;
                        s_ft_hi = hi;
                    }
                }
            }
            if ((long)fr >= s_ft_lo && (long)fr <= s_ft_hi) {
                printf("ftrace: f=%llu ARM9=%08X cpsr9=%08X ARM7=%08X cpsr7=%08X"
                       " if9=%08X if7=%08X ie9=%08X ie7=%08X ime=%u/%u"
                       " fifo=%d/%d lr9=%08X sp9=%08X\n",
                       (unsigned long long)fr,
                       nds->cpu->r[15], nds->cpu->cpsr,
                       nds->cpu7->r[15], nds->cpu7->cpsr,
                       nds->io->irq[0].ifl, nds->io->irq[1].ifl,
                       nds->io->irq[0].ie, nds->io->irq[1].ie,
                       nds->io->irq[0].ime, nds->io->irq[1].ime,
                       nds->io->fifo.from7.count, nds->io->fifo.from9.count,
                       nds->cpu->r[14], nds->cpu->r[13]);
                {
                    /* 21-B9yi(续25)：GX 队列/引擎状态（配合 NDS_GXDBG 定位 GX 停摆） */
                    uint32_t gxf = 0, gxb = 0; int gxq = 0, gxp = 0;
                    gx_state(&nds->io->gx, &gxf, &gxb, &gxq, &gxp);
                    printf("ftrace:   gxfifo=%u busy=%u q=%d pend=%d stat=%08X\n",
                           gxf, gxb, gxq, gxp, nds->io->gx.gxstat);
                }
                printf("ftrace:   cart romctrl=%08X rem=%u pos=%u wait=%u/%u"
                       " fifo=%d late=%d dma3=%08X/%08X/%08X\n",
                       nds->io->cartbus.romctrl, nds->io->cartbus.xfer_remaining,
                       nds->io->cartbus.xfer_pos, nds->io->cartbus.wait_phase,
                       nds->io->cartbus.wait_cycles, nds->io->cartbus.data_count,
                       nds->io->cartbus.data_late,
                       bus_read32(nds->bus, 0x040000D4u),
                       bus_read32(nds->bus, 0x040000D8u),
                       bus_read32(nds->bus, 0x040000DCu));
                fflush(stdout);
            }
        }
        if ((fr % 100) == 0) {
            printf("headless-frames: f=%llu ARM9=%08X ARM7=%08X disp=%08X"
                   " if9=%08X if7=%08X cnt=%04X/%04X fifo=%d/%d gx=%u tri=%u"
                   " sp9=%08X lr9=%08X\n",
                   (unsigned long long)fr, nds->cpu->r[15],
                   nds->cpu7->r[15], bus_read32(nds->bus, 0x04000000u),
                   nds->io->irq[0].ifl, nds->io->irq[1].ifl,
                   nds->io->fifo.cnt9, nds->io->fifo.cnt7,
                   nds->io->fifo.from7.count, nds->io->fifo.from9.count,
                   nds->io->gx.cmd_count, nds->io->gx.tri_count,
                   nds->cpu->r[13], nds->cpu->r[14]);
            fflush(stdout);
        }
    }

    printf("headless-frames: done. frame=%llu now=%llu"
           " | ARM9 PC=%08X cyc=%llu cpsr=%08X"
           " | ARM7 PC=%08X cyc=%llu cpsr=%08X\n",
           (unsigned long long)runner_frame_index(r),
           (unsigned long long)runner_now(r),
           nds->cpu->r[15], (unsigned long long)nds->cpu->cycles, nds->cpu->cpsr,
           nds->cpu7->r[15], (unsigned long long)nds->cpu7->cycles,
           nds->cpu7->cpsr);
    uint64_t vram_nz = 0;
    for (size_t vi = 0; vi < BUS_VRAM_SIZE; vi++)
        if (nds->bus->vram[vi]) vram_nz++;
    /* 21-B9wu：3D 引擎状态摘要（3D 场景是否真的出图） */
    {
        extern unsigned long long g_rtc_reads;
        extern unsigned long long g_snd_cnt_writes;
        extern unsigned long long g_snd_bias_writes;
        extern unsigned int g_snd_bias_last;
            printf("io: rtc-reads=%llu snd-cnt-writes=%llu snd-bias-writes=%llu last=%03X\n",
                   g_rtc_reads, g_snd_cnt_writes, g_snd_bias_writes, g_snd_bias_last);
        {
            extern unsigned long long g_vblank_events, g_scanline_events;
            printf("evt: vblank=%llu scanline=%llu\n", g_vblank_events,
                   g_scanline_events);
        }
        {
            /* 21-B9xs：PC 命中计数（--pchit 配置） */
            extern uint32_t g_pchit_addr[16];
            extern unsigned long long g_pchit_cnt[16];
            extern int g_pchit_n;
            for (int i = 0; i < g_pchit_n; i++)
                printf("pchit: %08X = %llu\n", g_pchit_addr[i], g_pchit_cnt[i]);
            {
                extern unsigned long long g_pchit_hist[16];
                printf("pchit-hist:");
                for (int i = 0; i < 16; i++)
                    if (g_pchit_hist[i])
                        printf(" [%X]=%llu", i, g_pchit_hist[i]);
                printf("\n");
            }
        }
        {
            extern unsigned long long g_ipc_sends[2];
            extern unsigned long long g_ipc_c187;
            extern uint32_t g_ipc_trace[32][3];
            extern unsigned g_ipc_trace_n;
            printf("ipc: sends9=%llu sends7=%llu c187=%llu\n", g_ipc_sends[0],
                   g_ipc_sends[1], g_ipc_c187);
            unsigned total = g_ipc_trace_n < 32u ? g_ipc_trace_n : 32u;
            unsigned start = g_ipc_trace_n - total;
            for (unsigned i = 0; i < total; i++) {
                unsigned k = (start + i) % 32u;
                printf("ipc: #%u arm%u f=%u val=%08X\n", start + i,
                       g_ipc_trace[k][0], g_ipc_trace[k][2], g_ipc_trace[k][1]);
            }
            extern uint32_t g_ipc9_trace[16][2];
            extern unsigned g_ipc9_n;
            unsigned t9 = g_ipc9_n < 16u ? g_ipc9_n : 16u;
            unsigned s9 = g_ipc9_n - t9;
            for (unsigned i = 0; i < t9; i++) {
                unsigned k = (s9 + i) % 16u;
                printf("ipc9: #%u f=%u val=%08X\n", s9 + i,
                       g_ipc9_trace[k][1], g_ipc9_trace[k][0]);
            }
        }
        /* 21-B9wz：ARM9 累积热点前 16（含占比）。21-B9yi(续41)：仅在
           NDS_PCHOT=1 时统计（默认关闭，见 runner_step 里的说明）。 */
        if (h9_enabled()) {
        for (int hi = 0; hi < 16; hi++) {
            uint32_t best = 0, bh = 0;
            for (uint32_t h = 0; h < (1u << 16); h++)
                if (s_h9_cnt[h] > best) { best = (uint32_t)s_h9_cnt[h]; bh = h; }
            if (best == 0)
                break;
            printf("hot9: pc=%08X cnt=%llu (%.1f%%)\n", s_h9_pc[bh],
                   (unsigned long long)s_h9_cnt[bh],
                   s_h9_total ? 100.0 * (double)s_h9_cnt[bh] / (double)s_h9_total : 0.0);
            s_h9_cnt[bh] = 0;
        }
        printf("hot9: total=%llu\n", (unsigned long long)s_h9_total);
        }
        const uint16_t *g3 = gx_framebuffer(&nds->io->gx);
        size_t n3 = 0;
        for (size_t i = 0; i < (size_t)GX_SCREEN_W * GX_SCREEN_H; i++)
            if (g3[i] != 0) n3++;
        printf("gx3d: fb-nz=%zu gxstat=%08X disp3dcnt=%08X cmd=%u tri=%u"
               " fifo-wr=%u port-wr=%u\n",
               n3, nds->io->gx.gxstat, nds->io->gx.disp3dcnt,
               nds->io->gx.cmd_count, nds->io->gx.tri_count,
               nds->io->gx.fifo_writes, nds->io->gx.port_writes);
        /* 21-B9yi(续34)：3D 路径计数（判断「几何到底有没有落到屏幕上」） */
        printf("gxpath: tex=%u flat=%u drawn=%u px=%llu vtx0w=%u"
               " texparam=%08X pltt=%04X polyattr=%08X\n",
               nds->io->gx.tri_tex, nds->io->gx.tri_flat, nds->io->gx.tri_drawn,
               (unsigned long long)nds->io->gx.px_written, nds->io->gx.vtx_zero_w,
               nds->io->gx.tex_param, nds->io->gx.pltt_base, nds->io->gx.poly_attr);
        printf("gxclip: X-=%u X+=%u Y-=%u Y+=%u Z-=%u Z+=%u\n",
               nds->io->gx.clip_rej[0], nds->io->gx.clip_rej[1],
               nds->io->gx.clip_rej[2], nds->io->gx.clip_rej[3],
               nds->io->gx.clip_rej[4], nds->io->gx.clip_rej[5]);
        {
            printf("gxexec:");
            for (int c = 0; c < 256; c++)
                if (nds->io->gx.exec_hist[c] != 0)
                    printf(" %02X=%u", c, nds->io->gx.exec_hist[c]);
            printf("\n");
        }
        printf("gxwork: bbox-px=%llu tested-px=%llu\n",
               (unsigned long long)nds->io->gx.bbox_px,
               (unsigned long long)nds->io->gx.px_tested);
        printf("gxfog: fog-px=%llu disp3dcnt=%08X fogcolor=%08X off=%04X\n",
               (unsigned long long)nds->io->gx.fog_px, nds->io->gx.disp3dcnt,
               nds->io->gx.fog_color, nds->io->gx.fog_offset);
        printf("gxblend: mode0=%u mode1=%u mode2=%u mode3=%u wire=%u\n",
               nds->io->gx.blend_hist[0], nds->io->gx.blend_hist[1],
               nds->io->gx.blend_hist[2], nds->io->gx.blend_hist[3],
               nds->io->gx.wire_polys);
        /* 21-B9yi(续32)：NDS_GXHIST=1 → 打印 GX 命令直方图 */
        if (getenv("NDS_GXHIST") != NULL)
            gx_cmd_hist_dump();
    }
    printf("headless-frames: summary vram-nz=%llu disp=%08X dispb=%08X"
           " irq9=%d irq7=%d\n",
           (unsigned long long)vram_nz,
           bus_read32(nds->bus, 0x04000000u),
           bus_read32(nds->bus, 0x04001000u),
        nds->cpu->irq_count, nds->cpu7->irq_count);
    runner_savechip_report(nds);   /* 21-B9yi(续77)：存档芯片是否被写过 */
    if (shot_path != NULL) {
        uint32_t *fb_top = (uint32_t *)malloc(sizeof(uint32_t) * RENDER_SCREEN_W
                                              * RENDER_SCREEN_H);
        uint32_t *fb_bot = (uint32_t *)malloc(sizeof(uint32_t) * RENDER_SCREEN_W
                                              * RENDER_SCREEN_H);
        if (fb_top != NULL && fb_bot != NULL) {
            render_frame(nds->bus, fb_top, fb_bot);
            if (save_bmp(shot_path, fb_top, fb_bot) == 0)
                printf("headless-frames: screenshot saved to %s\n", shot_path);
            else
                printf("headless-frames: screenshot FAILED (%s)\n", shot_path);
        } else {
            printf("headless-frames: screenshot OOM\n");
        }
        free(fb_top);
        free(fb_bot);
    }
    bios7_low_dump_hist("end"); /* 21-B9ye：低地址取指直方图（与参考核 hist7 对照） */
    runner_destroy(r);
    fflush(stdout);
}

/* 把两块 256×192 RGBA8888 帧缓冲纵向拼成一张 24 位 BMP（顶屏在上）。
   只用于 headless 诊断截图，不依赖 SDL。 */
static int save_bmp(const char *path, const uint32_t *fb_top,
                    const uint32_t *fb_bot)
{
    const int w = RENDER_SCREEN_W, h = RENDER_SCREEN_H, total_h = h * 2;
    const int row_size = (w * 3 + 3) & ~3;
    const int data_size = row_size * total_h;
    const int file_size = 14 + 40 + data_size;
    uint8_t hdr[54];
    uint8_t *rows = (uint8_t *)malloc((size_t)data_size);
    FILE *f;
    int ok = 0;

    if (rows == NULL)
        return -1;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = (uint8_t)file_size; hdr[3] = (uint8_t)(file_size >> 8);
    hdr[4] = (uint8_t)(file_size >> 16); hdr[5] = (uint8_t)(file_size >> 24);
    hdr[10] = 54;
    hdr[14] = 40;
    hdr[18] = (uint8_t)w; hdr[19] = (uint8_t)(w >> 8);
    hdr[22] = (uint8_t)total_h; hdr[23] = (uint8_t)(total_h >> 8);
    hdr[26] = 1;                    /* 单平面 */
    hdr[28] = 24;                   /* 24 位 RGB */
    hdr[34] = (uint8_t)data_size; hdr[35] = (uint8_t)(data_size >> 8);
    hdr[36] = (uint8_t)(data_size >> 16); hdr[37] = (uint8_t)(data_size >> 24);

    /* BMP 行自下而上；NDS 帧缓冲行自上而下。 */
    for (int y = 0; y < total_h; y++) {
        /* BMP 行自下而上：显示顶部应是 fb_top。file row 0 = 图像最底行。 */
        int disp_y = total_h - 1 - y;
        const uint32_t *src = (disp_y < h) ? fb_top : fb_bot;
        int src_y = (disp_y < h) ? disp_y : disp_y - h;
        uint8_t *dst = rows + (size_t)y * row_size;
        for (int x = 0; x < w; x++) {
            uint32_t px = src[(size_t)src_y * w + x];
            dst[x * 3 + 0] = (uint8_t)px;           /* B */
            dst[x * 3 + 1] = (uint8_t)(px >> 8);    /* G */
            dst[x * 3 + 2] = (uint8_t)(px >> 16);   /* R */
        }
    }

    f = fopen(path, "wb");
    if (f != NULL) {
        ok = fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr
          && fwrite(rows, 1, (size_t)data_size, f) == (size_t)data_size;
        fclose(f);
    }
    free(rows);
    return ok ? 0 : -1;
}

void runner_headless(nds_t *nds, uint64_t steps, int trace,
                     const char *shot_path)
{
    bus_set_diag(nds->bus, 1);      /* 只打异常事件：未知 SWI/未实现指令/未知 IO */
    exec_set_trace(trace);          /* trace=1 时逐条打印指令，用于追前几十步 */
    thumb_set_trace(trace);

    printf("headless: running %llu steps (ARM9:ARM7 = 2:1)\n",
           (unsigned long long)steps);
    fflush(stdout);

    for (uint64_t i = 0; i < steps; i++) {
        if (i % 3 == 2)
            cpu_step(nds->cpu7);
        else
            cpu_step(nds->cpu);

        /* 21-B9xc：VBlank 在第 192 行触发、帧边界把 VCOUNT 归零（与事件驱动一致） */
        if ((i & 0xFFFu) == 0) {
            io_advance_scanline(nds->io);
            if (nds->io->vcount == 192u)
                io_set_vblank(nds->io);
        }
        if ((i & 0xFFFFFu) == 0xFFFFFu)
            io_frame_boundary(nds->io);
        /* 每 100 万步打一次进度 */
        if ((i & 0xFFFFFu) == 0xFFFFFu) {
            /* 21-B9h：进度附带两核中断寄存器，便于判断“等 IRQ 但没来”是
               使能被关、挂起没置位还是 handler 没清位。 */
            printf("headless: step=%llu ARM9 PC=%08X cyc=%llu cpsr=%08X r0=%08X r1=%08X r2=%08X r3=%08X ime=%08X ie=%08X if=%08X\n",
                   (unsigned long long)(i + 1),
                   nds->cpu->r[15], (unsigned long long)nds->cpu->cycles,
                   nds->cpu->cpsr, nds->cpu->r[0], nds->cpu->r[1],
                   nds->cpu->r[2], nds->cpu->r[3],
                   nds->io->irq[0].ime,
                   nds->io->irq[0].ie, nds->io->irq[0].ifl);
            printf("headless: step=%llu ARM7 PC=%08X cyc=%llu ime=%08X ie=%08X if=%08X\n",
                   (unsigned long long)(i + 1),
                   nds->cpu7->r[15], (unsigned long long)nds->cpu7->cycles,
                   nds->io->irq[1].ime, nds->io->irq[1].ie, nds->io->irq[1].ifl);
        }
    }

    printf("headless: done. ARM9 PC=%08X cyc=%llu cpsr=%08X ime=%08X ie=%08X if=%08X"
           " | ARM7 PC=%08X cyc=%llu ime=%08X ie=%08X if=%08X\n",
           nds->cpu->r[15], (unsigned long long)nds->cpu->cycles,
           nds->cpu->cpsr,
           nds->io->irq[0].ime, nds->io->irq[0].ie, nds->io->irq[0].ifl,
           nds->cpu7->r[15], (unsigned long long)nds->cpu7->cycles,
           nds->io->irq[1].ime, nds->io->irq[1].ie, nds->io->irq[1].ifl);
    if (shot_path != NULL) {
        uint32_t *fb_top = (uint32_t *)malloc(sizeof(uint32_t) * RENDER_SCREEN_W
                                              * RENDER_SCREEN_H);
        uint32_t *fb_bot = (uint32_t *)malloc(sizeof(uint32_t) * RENDER_SCREEN_W
                                              * RENDER_SCREEN_H);
        if (fb_top != NULL && fb_bot != NULL) {
            render_frame(nds->bus, fb_top, fb_bot);
            if (save_bmp(shot_path, fb_top, fb_bot) == 0)
                printf("headless: screenshot saved to %s\n", shot_path);
            else
                printf("headless: screenshot FAILED (%s)\n", shot_path);
        } else {
            printf("headless: screenshot OOM\n");
        }
        free(fb_top);
        free(fb_bot);
        printf("headless: disp=%08X dispb=%08X disp3d=%08X bg0=%04X bg0b=%04X"
               " pal=%04X palb=%04X vram0=%08X\n",
               bus_read32(nds->bus, 0x04000000u),
               bus_read32(nds->bus, 0x04001000u),
               bus_read32(nds->bus, 0x04000060u),
               bus_read16(nds->bus, 0x04000008u),
               bus_read16(nds->bus, 0x04001008u),
               bus_read16(nds->bus, 0x05000000u),
               bus_read16(nds->bus, 0x05000400u),
               bus_read32(nds->bus, 0x06000000u));
    }
    fflush(stdout);
}
