#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nds/nds.h"      /* nds_t / bus_t */
#include "bus/bus.h"      /* bus_set_diag */
#include "cpu/cpu.h"      /* cpu_step / arm_cpu_t（r / cycles） */
#include "cpu/exec.h"     /* exec_set_trace */
#include "cpu/thumb.h"    /* thumb_set_trace */
#include "io/io.h"        /* io_set_vblank */
#include "ppu/render.h"   /* render_frame：双屏纯软件出图 */
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
    c->next_line += c->line_cycles;
    timing_arm(c->tm, 0, c->next_line - c->tm->now, runner_ev_line, c);
}

static void runner_ev_frame(void *ctx)
{
    runner_ev_ctx_t *c = (runner_ev_ctx_t *)ctx;
    io_set_vblank(c->io);
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
};

#define RUNNER_WAKE7_IO(io_) (((io_)->irq[1].ie & (io_)->irq[1].ifl) != 0)
#define RUNNER_WAKE9_IO(io_) irq_pending(&(io_)->irq[0])

runner_t *runner_create(nds_t *nds)
{
    if (nds == NULL)
        return NULL;
    runner_t *r = (runner_t *)calloc(1, sizeof(runner_t));
    if (r == NULL)
        return NULL;
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
        r->key_release_frame = fr + 8;
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
        r->cost9 = r->tm.now * 2;
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
        step7 = (r->cost9 / 2 > r->cost7);
    if (step7) {
        cpu_step(nds->cpu7);
        r->a7_wait = (nds->cpu7->step_cycles == 0);
        if (!r->a7_wait) r->cost7 += nds->cpu7->step_cycles;
    } else {
        cpu_step(nds->cpu);
        r->a9_wait = (nds->cpu->step_cycles == 0);
        if (!r->a9_wait) r->cost9 += nds->cpu->step_cycles;
    }

    uint64_t sys;
    if (r->a9_wait)
        sys = r->cost7;
    else if (r->a7_wait)
        sys = r->cost9 / 2;
    else
        sys = (r->cost9 / 2 < r->cost7) ? r->cost9 / 2 : r->cost7;
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
            step7 = (cost9 / 2 > cost7);
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
            sys = cost9 / 2;
        else
            sys = (cost9 / 2 < cost7) ? cost9 / 2 : cost7;
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

void runner_headless_frames(nds_t *nds, uint64_t frames, const char *shot_path,
                            uint64_t key_frame, uint32_t key_mask,
                            uint64_t key_period)
{
    bus_set_diag(nds->bus, 1);
    exec_set_trace(0);
    thumb_set_trace(0);

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
        if ((fr % 100) == 0) {
            printf("headless-frames: f=%llu ARM9=%08X ARM7=%08X disp=%08X\n",
                   (unsigned long long)fr, nds->cpu->r[15],
                   nds->cpu7->r[15], bus_read32(nds->bus, 0x04000000u));
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

        /* 近似一帧（约 100 万步）触发一次 VBlank，模拟显示硬件，让等 VBlank 的游戏能继续 */
        if ((i & 0xFFFFFu) == 0xFFFFFu)
            io_set_vblank(nds->io);
        /* VCOUNT 逐行：一帧约 263 条扫描线，100 万步内约 4000 步一条线 */
        if ((i & 0xFFFu) == 0)
            io_advance_scanline(nds->io);
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
