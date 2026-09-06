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
        int src_y = total_h - 1 - y;
        const uint32_t *src = (src_y < h) ? fb_bot : fb_top;
        if (src_y >= h) src_y -= h;
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
