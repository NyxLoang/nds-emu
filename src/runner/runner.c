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
static int s_arm9_div = 1;
#define RUNNER_ARM9_DIV (s_arm9_div)
#define RUNNER_SYS9(c) ((c) / (uint64_t)RUNNER_ARM9_DIV)

/* 21-B9wz 诊断：整段运行的 ARM9 累积热点（hot9: 前 16 + 占比 + 总数）。
   和参考核的 ins9/hist9 对照用：两边热点集合一致说明代码路径相同，
   差异只可能在时序/外设状态。 */
static uint32_t s_h9_pc[1 << 16];
static uint64_t s_h9_cnt[1 << 16];
static uint64_t s_h9_total;

runner_t *runner_create(nds_t *nds)
{
    if (nds == NULL)
        return NULL;
    runner_t *r = (runner_t *)calloc(1, sizeof(runner_t));
    if (r == NULL)
        return NULL;
    {   /* 21-B9yg：ARM9 时钟折算口径（默认 1；NDS_ARM9_DIV=2 切回旧口径做 A/B） */
        const char *e = getenv("NDS_ARM9_DIV");
        s_arm9_div = (e != NULL && e[0] == '2') ? 2 : 1;
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

uint64_t runner_frame_index(const runner_t *r)
{
    return (r != NULL) ? (r->tm.now / r->frame_cycles) : 0;
}

uint64_t runner_now(const runner_t *r)
{
    return (r != NULL) ? r->tm.now : 0;
}

/* 帧号到达脚本时刻时注入按键，保持 8 帧后释放（游戏按帧轮询）。 */
static void runner_keys(runner_t *r)
{
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
static int runner_step(runner_t *r)
{
    nds_t *nds = r->nds;
    int was9 = r->a9_wait, was7 = r->a7_wait;

    if (r->a9_wait && RUNNER_WAKE9_IO(nds->io)) {
        r->a9_wait = 0;
        r->cost9 = r->tm.now * RUNNER_ARM9_DIV;
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
        {
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
    runner_set_keys(r, key_frame, key_mask, key_period);
    uint64_t start = runner_frame_index(r);
    for (uint64_t fi = 0; fi < frames; fi++) {
        if (!runner_run_frame(r))
            break;
        uint64_t fr = runner_frame_index(r);
        g_dbg_frame = fr;
        /* 21-B9yi(续32)：画面统计（每 N 帧一行，与参考核 harness 同口径） */
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
        /* 21-B9wz：ARM9 累积热点前 16（含占比） */
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
