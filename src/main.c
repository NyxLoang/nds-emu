#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#include <wchar.h>
#endif
#include <SDL.h>
#include <SDL_ttf.h>
#include "window/window.h"
#include "menu/menu.h"
#include "nds/nds.h"
#include "bus/bus.h"
#include "cpu/cpu.h"
#include "cpu/exec.h"   /* 21-B9yi(续49)：exec_set_trace */
#include "cpu/thumb.h"  /* 21-B9yi(续49)：thumb_set_trace */
#include "io/io.h"
#include "ppu/ppu.h"
#include "cart/cart.h"
#include "cart/save.h"
#include "audio/audio.h"
#include "demo/demo.h"
#include "runner/runner.h"

/* 小端工具：装载阶段给 0x027FFxxx 直接启动表填 ROM 头信息时用
   （melonDS SetupDirectBoot 口径，阶段 21-B8）。 */
#ifdef _WIN32
/* 21-B9yi(续42)：宽路径 → UTF-8 窄字符串（控制台已切 UTF-8，直接 %s 打印），
   避免 `%ls` 在中文文件名处截断。buf 由调用者提供且往返使用。 */
static const char *utf8_path(const wchar_t *w)
{
    static char buf[1024];
    if (w == NULL)
        return "";
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)sizeof buf, NULL, NULL) <= 0)
        buf[0] = '\0';
    return buf;
}
#endif
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* 把卡带头信息写进 ARM9 主存 0x027FFxxx 系统表（对应 melonDS SetupDirectBoot）：
   0x027FFE00 起 0x170 字节 = 完整 ROM 头；0x027FF800/0x027FFC00 两组
   「卡带 ID + CRC + 装载器签名」表。FFXII 的 ARM9/ARM7 启动代码会读这些表，
   缺了会停在事件轮询上。 */
static void direct_boot_tables(cart_t *cart, bus_t *bus)
{
    if (cart == NULL || cart->size < 0x170 || bus == NULL)
        return;
    /* 21-B9w: cart ID is a melonDS NDSCart::ParseROM value derived from the
       power-of-two padded ROM size, not the ASCII game code at header 0x0C.
       FFXII compares the value read back from CARD_DATA against 0x027FFC00;
       writing the game code sends it down the wrong service-14 path. */
    size_t padded = 1;
    while (padded < cart->size)
        padded <<= 1;
    uint32_t cartid = 0x000000C2u;
    if (padded >= 1024u * 1024u && padded <= 128u * 1024u * 1024u)
        cartid |= (uint32_t)((padded >> 20) - 1u) << 8;
    else
        cartid |= (uint32_t)(0x100u - (padded >> 28)) << 8;
    uint16_t hcrc = le16(cart->data + 0x0FC);   /* 头校验和 */
    uint16_t scrc = le16(cart->data + 0x0FE);   /* 安全区校验和 */
    for (uint32_t i = 0; i < 0x170; i += 4)
        bus_write32(bus, 0x027FFE00u + i, le32(cart->data + i));
    bus_write32(bus, 0x027FF800u, cartid);
    bus_write32(bus, 0x027FF804u, cartid);
    bus_write16(bus, 0x027FF808u, hcrc);
    bus_write16(bus, 0x027FF80Au, scrc);
    bus_write16(bus, 0x027FF850u, 0x5835u);
    bus_write32(bus, 0x027FFC00u, cartid);
    bus_write32(bus, 0x027FFC04u, cartid);
    bus_write16(bus, 0x027FFC08u, hcrc);
    bus_write16(bus, 0x027FFC0Au, scrc);
    bus_write16(bus, 0x027FFC10u, 0x5835u);
    bus_write16(bus, 0x027FFC30u, 0xFFFFu);
    bus_write16(bus, 0x027FFC40u, 0x0001u);
    printf("boot : direct-boot tables @ 027FFxxx written\n");
}

/* 21-B9wt：bring-up 内存转储（`--dump <前缀>`）。
   把两核可见的物理内存整体写盘，便于和参考核 harness 的
   `ref_fNNN_*.bin` 做逐字节 diff，定位「本地停住、参考继续」这类分歧。
   结构：<前缀>_mainram.bin / _arm7wram.bin / _sharedwram.bin / _vram.bin /
   _itcm.bin / _dtcm.bin。 */
typedef struct dump_part {
    const char *name;
    const uint8_t *data;
    size_t size;
} dump_part_t;

static void dump_state(const nds_t *nds, 
#ifdef _WIN32
                       const wchar_t *prefix
#else
                       const char *prefix
#endif
                       )
{
    /* 21-B9y4：打印 dump 时刻的 DISPCNT，用于与 runner 截图时刻对照 */
    printf("dump: t=after-runner DISPCNT=%08X DISPCNT_SUB=%08X\n",
           bus_read32(nds->bus, 0x04000000u),
           bus_read32(nds->bus, 0x04001000u));
    dump_part_t parts[] = {
        { "mainram",    nds->bus->main_ram,    BUS_MAIN_RAM_SIZE },
        { "arm7wram",   nds->bus->arm7_wram,   BUS_ARM7_WRAM_SIZE },
        { "sharedwram", nds->bus->shared_wram, BUS_SHARED_WRAM_SIZE },
        { "vram",       nds->bus->vram,        BUS_VRAM_SIZE },
        { "itcm",       nds->bus->arm9_itcm,   BUS_ARM9_ITCM_SIZE },
        { "dtcm",       nds->bus->arm9_dtcm,   BUS_ARM9_DTCM_SIZE },
        /* 21-B9y2：调色板 RAM（0x05000000，2KB）也纳入 dump——帧 1900 的
           "黑+青色条纹"在 VRAM/寄存器都一致后，调色板是下一个待比对象。 */
        { "palette",    nds->bus->palette,     BUS_PALETTE_SIZE },
    };
    for (size_t i = 0; i < sizeof parts / sizeof parts[0]; i++) {
        FILE *f = NULL;
#ifdef _WIN32
        wchar_t path[512];
        _snwprintf(path, 512, L"%ls_%hs.bin", prefix, parts[i].name);
        f = _wfopen(path, L"wb");
        if (f != NULL) {
            fwrite(parts[i].data, 1, parts[i].size, f);
            fclose(f);
            printf("dump : %ls (%zu bytes)\n", path, parts[i].size);
        }
#else
        char path[512];
        snprintf(path, sizeof path, "%s_%s.bin", prefix, parts[i].name);
        f = fopen(path, "wb");
        if (f != NULL) {
            fwrite(parts[i].data, 1, parts[i].size, f);
            fclose(f);
            printf("dump : %s (%zu bytes)\n", path, parts[i].size);
        }
#endif
    }
    /* 21-B9wu：两核视角的 IO 寄存器快照（0x04000000-0x04000FFF），
       与参考核 ARM9IORead 的 dump 对照，用于定位“同一阶段硬件状态不同”。 */
    {
        static uint32_t io9[0x400], io7[0x400];
        for (uint32_t i = 0; i < 0x400u; i++) {
            nds->bus->active_is_arm7 = 0;
            io9[i] = bus_read32(nds->bus, 0x04000000u + i * 4u);
            nds->bus->active_is_arm7 = 1;
            io7[i] = bus_read32(nds->bus, 0x04000000u + i * 4u);
        }
        nds->bus->active_is_arm7 = 0;
#ifdef _WIN32
        wchar_t p9[512], p7[512];
        _snwprintf(p9, 512, L"%ls_io9.bin", prefix);
        _snwprintf(p7, 512, L"%ls_io7.bin", prefix);
        FILE *f9 = _wfopen(p9, L"wb");
        FILE *f7 = _wfopen(p7, L"wb");
#else
        char p9[512], p7[512];
        snprintf(p9, sizeof p9, "%s_io9.bin", prefix);
        snprintf(p7, sizeof p7, "%s_io7.bin", prefix);
        FILE *f9 = fopen(p9, "wb");
        FILE *f7 = fopen(p7, "wb");
#endif
        if (f9) { fwrite(io9, 1, sizeof io9, f9); fclose(f9); }
        if (f7) { fwrite(io7, 1, sizeof io7, f7); fclose(f7); }
    }
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
#ifdef _WIN32
    /* Windows 控制台：程序日志按 UTF-8 输出，先切输出代码页，避免 936(GBK) 下中文乱码 */
    SetConsoleOutputCP(CP_UTF8);
#endif
    /* Windows：用宽字符命令行拿路径，避免窄 argv 在非 UTF-8 代码页下中文乱码 */
#ifdef _WIN32
    int wargc = 0;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    const wchar_t *rom_path = (wargc > 1) ? wargv[1] : NULL;
#else
    const char *rom_path = (argc > 1) ? argv[1] : NULL;
#endif

    /* 阶段 21 bring-up：`--headless N` 跑 N 步后退出（不开窗口/音频），定位第一个卡点 */
    long headless_steps = 0;
    int headless_trace = 0;
    int headless_cycles = 0;
    long headless_frames = 0;
    const char *headless_shot = NULL;
#ifdef _WIN32
    const wchar_t *dump_prefix_w = NULL;
    const wchar_t *shot_path_w = NULL;
#else
    const char *dump_prefix = NULL;
    const char *shot_path = NULL;
#endif
    uint64_t key_frame = 0;
    uint32_t key_mask = 0;
    uint64_t key_period = 0;
    int watch_n = 0;   /* 21-B9xj：--watch LO-HI 的组数（最多 4） */
    extern uint32_t g_pchit_addr[16];
    extern int g_pchit_n;
    uint64_t shot_every = 0;
    const char *shot_prefix = NULL;
    uint64_t stats_every = 0;   /* 21-B9yi(续32)：--stats-every N */
    /* 21-B9yi(续42)：窗口模式自动退出帧数（--frames N，冒烟测试用） */
    uint64_t g_cli_frames = 0;
    /* 21-B9yi(续61)：窗口模式每 N 帧打一行实测 fps（--fps-every N）。
       动机：此前只有「整段平均帧率」，看不出**哪一段**卡；游戏不同场景
       （地牢/对话/战斗指挥界面）负载差别很大，需要分段数据。 */
    uint64_t g_cli_fps_every = 0;
    /* 21-B9yi(续82)：帧节奏倍速（--speed N）。
       -1 = 未指定（默认 1.0 倍速，即 NDS 真机帧频）；
        0 = 不限速（等同 NDS_NOSYNC=1，自动化压测用）；
        N>0 = N 倍速快进。见主循环末尾的 frame pacing 注释。 */
    double g_cli_speed = -1.0;
    /* 21-B9yi(续46)：触摸注入脚本（--touch-frame/-x/-y/-period） */
    uint64_t touch_frame = 0, touch_period = 0;
    int touch_x = 128, touch_y = 96;
    /* 21-B9yi(续67)：拖拽注入（--touch-drag X1,Y1,X2,Y2 --touch-drag-steps N）。
       参数在解析阶段只记录，等整条命令行解析完（touch_frame/period 都确定后）再下发。 */
    int drag_on = 0, drag_x1 = 96, drag_y1 = 96, drag_x2 = 160, drag_y2 = 96;
    int drag_steps = 12;
    /* 21-B9yi(续71)：随机输入浸泡（--key-random SEED / --touch-random SEED） */
    int key_random_on = 0;
    uint32_t key_random_seed = 1;
    int touch_random_on = 0;
    uint32_t touch_random_seed = 1;
#ifdef _WIN32
    for (int i = 1; i < wargc; i++) {
        if (wcscmp(wargv[i], L"--headless") == 0 && i + 1 < wargc)
            headless_steps = wcstol(wargv[i + 1], NULL, 10);
        else if (wcscmp(wargv[i], L"--trace") == 0)
            headless_trace = 1;
        else if (wcscmp(wargv[i], L"--screenshot") == 0)
            headless_shot = "headless_shot.bmp";
        else if (wcscmp(wargv[i], L"--shot") == 0 && i + 1 < wargc) {
            shot_path_w = wargv[i + 1];
            /* 截图写入用窄路径（ASCII 路径可用）：宽→本地代码页转换 */
            static char shot_buf[512];
            WideCharToMultiByte(CP_ACP, 0, shot_path_w, -1, shot_buf, 512, NULL, NULL);
            headless_shot = shot_buf;
        }
        else if (wcscmp(wargv[i], L"--headless-cycles") == 0 && i + 1 < wargc) {
            headless_steps = wcstol(wargv[i + 1], NULL, 10);
            headless_cycles = 1;
        }
        else if (wcscmp(wargv[i], L"--headless-frames") == 0 && i + 1 < wargc)
            headless_frames = wcstol(wargv[i + 1], NULL, 10);
        else if (wcscmp(wargv[i], L"--shot-prefix") == 0 && i + 1 < wargc) {
            static char prefix_buf[400];
            WideCharToMultiByte(CP_UTF8, 0, wargv[i + 1], -1, prefix_buf,
                                (int)sizeof prefix_buf, NULL, NULL);
            shot_prefix = prefix_buf;
            runner_set_shot_series(shot_every, shot_prefix);
        }
        else if (wcscmp(wargv[i], L"--key-frame") == 0 && i + 1 < wargc)
            key_frame = _wcstoui64(wargv[i + 1], NULL, 0);
        else if (wcscmp(wargv[i], L"--key-mask") == 0 && i + 1 < wargc)
            key_mask = (uint32_t)wcstoul(wargv[i + 1], NULL, 0);
        else if (wcscmp(wargv[i], L"--key-period") == 0 && i + 1 < wargc)
            key_period = _wcstoui64(wargv[i + 1], NULL, 0);
        else if (wcscmp(wargv[i], L"--dump") == 0 && i + 1 < wargc)
            dump_prefix_w = wargv[i + 1];
        else if (wcscmp(wargv[i], L"--watch") == 0 && i + 1 < wargc) {
            /* 21-B9xj：--watch LO-HI（如 --watch 04000106-04000108），最多 4 组 */
            uint32_t lo = 0, hi = 0;
            if (swscanf(wargv[i + 1], L"%x-%x", &lo, &hi) == 2)
                runner_set_watch(watch_n++, lo, hi);
        }
        else if (wcscmp(wargv[i], L"--watch-r") == 0 && i + 1 < wargc) {
            /* 21-B9xk：--watch-r LO-HI 读监视（如 --watch-r 04100000-04100004） */
            uint32_t lo = 0, hi = 0;
            if (swscanf(wargv[i + 1], L"%x-%x", &lo, &hi) == 2)
                runner_set_watch_read(watch_n++, lo, hi);
        }
        else if (wcscmp(wargv[i], L"--pchit") == 0 && i + 1 < wargc) {
            /* 21-B9xs：--pchit ADDR（PC 命中计数，最多 4 个） */
            uint32_t a = 0;
            if (swscanf(wargv[i + 1], L"%x", &a) == 1 && g_pchit_n < 16)
                g_pchit_addr[g_pchit_n++] = a;
        }
        else if (wcscmp(wargv[i], L"--shot-every") == 0 && i + 1 < wargc) {
            /* 21-B9xu：--shot-every N（配合 --shot-prefix 存画面时间线） */
            shot_every = _wcstoui64(wargv[i + 1], NULL, 10);
            runner_set_shot_series(shot_every, shot_prefix);
        }
        else if (wcscmp(wargv[i], L"--stats-every") == 0 && i + 1 < wargc) {
            /* 21-B9yi(续32)：--stats-every N（每 N 帧打印双屏画面统计） */
            stats_every = _wcstoui64(wargv[i + 1], NULL, 10);
            runner_set_stats_series(stats_every);
        }
        else if (wcscmp(wargv[i], L"--screen-hash-every") == 0 && i + 1 < wargc)
            runner_set_hash_series(_wcstoui64(wargv[i + 1], NULL, 10));
        else if (wcscmp(wargv[i], L"--frames") == 0 && i + 1 < wargc)
            g_cli_frames = _wcstoui64(wargv[i + 1], NULL, 10);
        else if (wcscmp(wargv[i], L"--fps-every") == 0 && i + 1 < wargc)
            g_cli_fps_every = _wcstoui64(wargv[i + 1], NULL, 10);
        else if (wcscmp(wargv[i], L"--speed") == 0 && i + 1 < wargc)
            g_cli_speed = wcstod(wargv[i + 1], NULL);
        else if (wcscmp(wargv[i], L"--touch-frame") == 0 && i + 1 < wargc)
            touch_frame = _wcstoui64(wargv[i + 1], NULL, 0);
        else if (wcscmp(wargv[i], L"--touch-x") == 0 && i + 1 < wargc)
            touch_x = (int)wcstol(wargv[i + 1], NULL, 10);
        else if (wcscmp(wargv[i], L"--touch-y") == 0 && i + 1 < wargc)
            touch_y = (int)wcstol(wargv[i + 1], NULL, 10);
        else if (wcscmp(wargv[i], L"--touch-period") == 0 && i + 1 < wargc)
            touch_period = _wcstoui64(wargv[i + 1], NULL, 0);
        else if (wcscmp(wargv[i], L"--touch-drag") == 0 && i + 1 < wargc) {
            /* 21-B9yi(续67)：--touch-drag X1,Y1,X2,Y2（配合 --touch-frame/-period） */
            int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
            if (swscanf(wargv[i + 1], L"%d,%d,%d,%d", &x1, &y1, &x2, &y2) == 4) {
                drag_on = 1;
                drag_x1 = x1; drag_y1 = y1; drag_x2 = x2; drag_y2 = y2;
            }
        }
        else if (wcscmp(wargv[i], L"--touch-drag-steps") == 0 && i + 1 < wargc)
            drag_steps = (int)wcstol(wargv[i + 1], NULL, 10);
        else if (wcscmp(wargv[i], L"--key-random") == 0 && i + 1 < wargc) {
            key_random_on = 1;
            key_random_seed = (uint32_t)wcstoul(wargv[i + 1], NULL, 0);
        }
        else if (wcscmp(wargv[i], L"--touch-random") == 0 && i + 1 < wargc) {
            touch_random_on = 1;
            touch_random_seed = (uint32_t)wcstoul(wargv[i + 1], NULL, 0);
        }
    }
#else
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--headless") == 0 && i + 1 < argc)
            headless_steps = strtol(argv[i + 1], NULL, 10);
        else if (strcmp(argv[i], "--trace") == 0)
            headless_trace = 1;
        else if (strcmp(argv[i], "--screenshot") == 0)
            headless_shot = "headless_shot.bmp";
        else if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) {
            shot_path = argv[i + 1];
            headless_shot = shot_path;
        }
        else if (strcmp(argv[i], "--headless-cycles") == 0 && i + 1 < argc) {
            headless_steps = strtol(argv[i + 1], NULL, 10);
            headless_cycles = 1;
        }
        else if (strcmp(argv[i], "--headless-frames") == 0 && i + 1 < argc)
            headless_frames = strtol(argv[i + 1], NULL, 10);
        else if (strcmp(argv[i], "--key-frame") == 0 && i + 1 < argc)
            key_frame = strtoull(argv[i + 1], NULL, 0);
        else if (strcmp(argv[i], "--key-mask") == 0 && i + 1 < argc)
            key_mask = (uint32_t)strtoul(argv[i + 1], NULL, 0);
        else if (strcmp(argv[i], "--key-period") == 0 && i + 1 < argc)
            key_period = strtoull(argv[i + 1], NULL, 0);
        else if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc)
            dump_prefix = argv[i + 1];
        else if (strcmp(argv[i], "--watch") == 0 && i + 1 < argc) {
            uint32_t lo = 0, hi = 0;
            if (sscanf(argv[i + 1], "%x-%x", &lo, &hi) == 2)
                runner_set_watch(watch_n++, lo, hi);
        }
        else if (strcmp(argv[i], "--watch-r") == 0 && i + 1 < argc) {
            uint32_t lo = 0, hi = 0;
            if (sscanf(argv[i + 1], "%x-%x", &lo, &hi) == 2)
                runner_set_watch_read(watch_n++, lo, hi);
        }
        else if (strcmp(argv[i], "--pchit") == 0 && i + 1 < argc) {
            uint32_t a = 0;
            if (sscanf(argv[i + 1], "%x", &a) == 1 && g_pchit_n < 16)
                g_pchit_addr[g_pchit_n++] = a;
        }
        else if (strcmp(argv[i], "--shot-every") == 0 && i + 1 < argc) {
            shot_every = strtoull(argv[i + 1], NULL, 10);
            runner_set_shot_series(shot_every, shot_prefix);
        }
        else if (strcmp(argv[i], "--stats-every") == 0 && i + 1 < argc) {
            stats_every = strtoull(argv[i + 1], NULL, 10);
            runner_set_stats_series(stats_every);
        }
        else if (strcmp(argv[i], "--screen-hash-every") == 0 && i + 1 < argc)
            runner_set_hash_series(strtoull(argv[i + 1], NULL, 10));
        else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            g_cli_frames = strtoull(argv[i + 1], NULL, 10);
        else if (strcmp(argv[i], "--fps-every") == 0 && i + 1 < argc)
            g_cli_fps_every = strtoull(argv[i + 1], NULL, 10);
        else if (strcmp(argv[i], "--speed") == 0 && i + 1 < argc)
            g_cli_speed = strtod(argv[i + 1], NULL);
        else if (strcmp(argv[i], "--touch-frame") == 0 && i + 1 < argc)
            touch_frame = strtoull(argv[i + 1], NULL, 0);
        else if (strcmp(argv[i], "--touch-x") == 0 && i + 1 < argc)
            touch_x = (int)strtol(argv[i + 1], NULL, 10);
        else if (strcmp(argv[i], "--touch-y") == 0 && i + 1 < argc)
            touch_y = (int)strtol(argv[i + 1], NULL, 10);
        else if (strcmp(argv[i], "--touch-period") == 0 && i + 1 < argc)
            touch_period = strtoull(argv[i + 1], NULL, 0);
        else if (strcmp(argv[i], "--touch-drag") == 0 && i + 1 < argc) {
            int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
            if (sscanf(argv[i + 1], "%d,%d,%d,%d", &x1, &y1, &x2, &y2) == 4) {
                drag_on = 1;
                drag_x1 = x1; drag_y1 = y1; drag_x2 = x2; drag_y2 = y2;
            }
        }
        else if (strcmp(argv[i], "--touch-drag-steps") == 0 && i + 1 < argc)
            drag_steps = (int)strtol(argv[i + 1], NULL, 10);
        else if (strcmp(argv[i], "--key-random") == 0 && i + 1 < argc) {
            key_random_on = 1;
            key_random_seed = (uint32_t)strtoul(argv[i + 1], NULL, 0);
        }
        else if (strcmp(argv[i], "--touch-random") == 0 && i + 1 < argc) {
            touch_random_on = 1;
            touch_random_seed = (uint32_t)strtoul(argv[i + 1], NULL, 0);
        }
        else if (strcmp(argv[i], "--shot-prefix") == 0 && i + 1 < argc) {
            shot_prefix = argv[i + 1];
            runner_set_shot_series(shot_every, shot_prefix);
        }
    }
#endif

    char err[256];

    /* 21-B9yi(续46)：把触摸注入脚本交给 runner（headless 帧驱动会套用）。 */
    if (drag_on)
        runner_set_touch_drag_series(touch_frame, drag_x1, drag_y1,
                                     drag_x2, drag_y2, drag_steps, touch_period);
    else if (touch_frame != 0)
        runner_set_touch_series(touch_frame, touch_x, touch_y, touch_period);
    /* 21-B9yi(续71)：随机输入浸泡（见 runner_keys / runner_touch） */
    if (key_random_on)
        runner_set_keys_random_series(key_frame ? key_frame : 1, key_random_seed,
                                      key_period ? key_period : 60);
    if (touch_random_on)
        runner_set_touch_random_series(touch_frame, touch_random_seed,
                                       touch_period ? touch_period : 90);

    /* 一台空机器：整机状态容器，bus 已挂入。
       必须先建 nds，后续装载镜像时才有可写的 Main RAM。headless 诊断也用它。 */
    nds_t *nds = nds_create();
    if (nds == NULL) {
        fprintf(stderr, "nds_create failed\n");
        return 1;
    }

    /* 阶段 2：装载 .nds（若有）→ 解析头 → 把 ARM9 镜像写进 bus 的 Main RAM，
       再从 bus 读回验证「装载-读回」闭环成立。 */
    cart_t *cart = NULL;
#ifdef _WIN32
    wchar_t *save_path = NULL;
#else
    char *save_path = NULL;
#endif
    if (rom_path != NULL) {
#ifdef _WIN32
        cart = cart_load_w(rom_path, err, sizeof err);
#else
        cart = cart_load(rom_path, err, sizeof err);
#endif
        if (cart == NULL) {
            fprintf(stderr, "%s\n", err);
            nds_destroy(nds);
            return 1;
        }
        printf("=== NDS cartridge ===\n");
        printf("file  : %ls (%zu bytes)\n", rom_path, cart->size);
        fflush(stdout);

        cart_header_t hdr;
        if (cart_parse_header(cart, &hdr) != 0) {
            printf("header: too short (%zu bytes), cannot parse\n", cart->size);
        } else {
            direct_boot_tables(cart, nds->bus);
            printf("arm9  : offset=%08X entry=%08X ram=%08X size=%08X\n",
                   hdr.arm9.offset, hdr.arm9.entry, hdr.arm9.ram, hdr.arm9.size);
            printf("arm7  : offset=%08X entry=%08X ram=%08X size=%08X\n",
                   hdr.arm7.offset, hdr.arm7.entry, hdr.arm7.ram, hdr.arm7.size);

            /* 阶段 14：若为加密商业卡带，先就地解密 ROM 缓冲里的安全区
               （ARM9 镜像前 0x800 字节），使后面拷进 Main RAM 的已是明文。 */
            if (cart_decrypt_secure_area(cart))
                printf("secure: KEY1 area decrypted (encryObj OK)\n");
            else
                printf("secure: not encrypted (homebrew) or decrypt failed, kept as-is\n");

            /* 把 ARM9 镜像逐字节写进 bus 的 Main RAM（地址 = 头里的 ram 字段）。
               逐字节走 bus_write8，让每个字节都经过地址换算，验证 bus 语义。 */
            if (hdr.arm9.offset + hdr.arm9.size > cart->size) {
                printf("image : arm9 out of file range\n");
            } else if (hdr.arm9.size > BUS_MAIN_RAM_SIZE) {
                printf("image : arm9 too large for Main RAM (%u bytes)\n",
                       hdr.arm9.size);
            } else {
                for (uint32_t i = 0; i < hdr.arm9.size; i++)
                    bus_write8(nds->bus, hdr.arm9.ram + i,
                               cart->data[hdr.arm9.offset + i]);

                /* 读回验证：镜像头 4 字节应从 bus 读出且与文件一致。
                   直接读前 32 位，验证小端拼拆与地址换算双正确。 */
                uint32_t readback = bus_read32(nds->bus, hdr.arm9.ram);
                printf("image : loaded %u bytes into Main RAM @ %08X, first word readback %08X\n",
                       hdr.arm9.size, hdr.arm9.ram, readback);

                /* 阶段 3a：镜像就位后，让 CPU 从 ARM9 入口开始执行 */
                cpu_reset(nds->cpu, hdr.arm9.entry);
                /* 21-B9ws：直接启动的寄存器/栈初值按 melonDS SetupDirectBoot 口径
                   （sp=0x03002F7C、sp_irq=0x03003F80、sp_svc=0x03003FC0），
                   否则 BIOS 低地址路径压栈会落到栈外。 */
                cpu_direct_boot(nds->cpu, hdr.arm9.entry);
                printf("cpu   : reset PC=%08X\n", hdr.arm9.entry);
            }

            /* 阶段 8.3：把 ARM7 镜像逐字节写进头里 ram 字段指向的目标区。
               该地址可能是 Main RAM（如 FFXII 的 0x02380000）或 ARM7 WRAM（0x03800000），
               故按目标区范围校验，而不是固定 64KB WRAM。 */
            if (hdr.arm7.offset + hdr.arm7.size > cart->size) {
                printf("image : arm7 out of file range\n");
            } else {
                uint32_t a7_end = hdr.arm7.ram + hdr.arm7.size;
                int a7_ok = 0;
                if (hdr.arm7.ram >= BUS_MAIN_RAM_BASE &&
                    a7_end <= BUS_MAIN_RAM_BASE + BUS_MAIN_RAM_SIZE)
                    a7_ok = 1;
                else if (hdr.arm7.ram >= BUS_ARM7_WRAM_BASE &&
                         a7_end <= BUS_ARM7_WRAM_BASE + BUS_ARM7_WRAM_SIZE)
                    a7_ok = 1;
                if (!a7_ok) {
                    printf("image : arm7 target 0x%08X out of range (%u bytes)\n",
                           hdr.arm7.ram, hdr.arm7.size);
                } else {
                    for (uint32_t i = 0; i < hdr.arm7.size; i++)
                        bus_write8(nds->bus, hdr.arm7.ram + i,
                                   cart->data[hdr.arm7.offset + i]);

                    uint32_t readback7 = bus_read32(nds->bus, hdr.arm7.ram);
                    printf("image : loaded %u bytes into ARM7 RAM @ %08X, first word readback %08X\n",
                           hdr.arm7.size, hdr.arm7.ram, readback7);

                    /* 阶段 8.3：ARM7 镜像就位，让第二颗 CPU 从 ARM7 入口开始执行 */
                    cpu_reset(nds->cpu7, hdr.arm7.entry);
                    cpu_direct_boot(nds->cpu7, hdr.arm7.entry);
                    printf("cpu7  : reset PC=%08X\n", hdr.arm7.entry);
                }
            }
        }

        /* 阶段 15：把 ROM 缓冲借给卡带总线，让游戏运行时能经 ROMCTRL/CARD_DATA
           按需读卡带数据（真游戏不靠一次性装载，而是流式读）。 */
        io_attach_cart(nds->io, cart->data, cart->size);

        /* 阶段 16：配置存档芯片（默认 EEPROM 8K，最常用），并从 <rom>.sav 读回进度。
           存档类型自动检测（按游戏芯片 ID/访问模式）留待后续阶段补。 */
        io_attach_save(nds->io, SAVE_EEPROM_8K);
#ifdef _WIN32
        save_path = save_make_path_w(rom_path);
        if (save_path != NULL) {
            save_load_file_w(io_get_save(nds->io), save_path);
            /* 21-B9yi(续42)：改成 UTF-8 打印路径。此前 `%ls` 在控制台
               （已切 UTF-8）遇到中文 ROM 名会在第一个中文处截断，
               看起来像「路径被吃掉」。 */
            printf("save : loaded %s (%zu bytes)\n",
                   utf8_path(save_path),
                   io_get_save(nds->io)->size);
        }
#else
        save_path = save_make_path(rom_path);
        if (save_path != NULL) {
            save_load_file(io_get_save(nds->io), save_path);
            printf("save : loaded %s (%zu bytes)\n", save_path,
                   io_get_save(nds->io)->size);
        }
#endif

        fflush(stdout);
    }
#ifdef _WIN32
    LocalFree(wargv);
#endif

    /* 阶段 21 bring-up：headless 模式下跑完 N 步即退出，不开窗口/音频 */
    if (headless_steps > 0 || headless_frames > 0) {
        if (headless_frames > 0)
            runner_headless_frames(nds, (uint64_t)headless_frames,
                                   headless_shot,
                                   key_frame, key_mask, key_period);
        else if (headless_cycles)
            runner_headless_cycles(nds, (uint64_t)headless_steps,
                                   headless_trace, headless_shot,
                                   key_frame, key_mask, key_period);
        else
            runner_headless(nds, (uint64_t)headless_steps, headless_trace,
                            headless_shot);
#ifdef _WIN32
        if (dump_prefix_w != NULL)
            dump_state(nds, dump_prefix_w);
#else
        if (dump_prefix != NULL)
            dump_state(nds, dump_prefix);
#endif
        if (save_path != NULL)
            free(save_path);
        cart_free(cart);
        nds_destroy(nds);
        return 0;
    }

    if (window_init(err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        cart_free(cart);
        nds_destroy(nds);
        return 1;
    }
    SDL_Renderer *renderer = window_get_renderer();

    if (menu_init(renderer) != 0) {
        fprintf(stderr, "menu_init failed, TTF error: %s\n", TTF_GetError());
        window_shutdown();
        cart_free(cart);
        nds_destroy(nds);
        return 1;
    }

    /* 阶段 4.3：ppu 把 VRAM framebuffer 转成 SDL 纹理（渲染器来自 window） */
    ppu_t *ppu = ppu_create(nds, renderer);
    if (ppu == NULL) {
        fprintf(stderr, "ppu_create failed\n");
        menu_shutdown();
        window_shutdown();
        nds_destroy(nds);
        return 1;
    }

    /* 阶段 9.7：真 2D 演示——只在未装载 ROM 时跑；有 ROM 时让 ROM 自己驱动画面。 */
    if (rom_path == NULL && nds->cpu != NULL) {
        demo_setup(nds);

        /* demo 完毕后恢复 mini.nds 入口，让主循环继续死循环空转 */
        cpu_reset(nds->cpu, 0x02000800);
        fflush(stdout);
    }

    /* 阶段 18.4：打开音频设备，回调线程开始按当前寄存器状态合成输出 */
    /* 21-B9yi(续68)：`NDS_NOAUDIO=1` 跳过声卡（诊断用，量化音频回调的开销）。
       跳过时 snd 仍按模拟时间推进（runner 的 snd_advance 路径，与无头一致），
       游戏侧行为等价，只是没有声音输出。 */
    {
        const char *e = getenv("NDS_NOAUDIO");
        if (e != NULL && e[0] != '0')
            printf("window: NDS_NOAUDIO=1（不打开声卡，仅用于测帧率）\n");
        else
            audio_init(nds);
    }

    /* 阶段 4.6：默认 2× 缩放启动（菜单下拉可切回 1x/2x） */
    window_set_scale(2);

    /* 21-B9wq：持久化帧驱动调度器（与 headless 共用事件/周期成本模型） */
    runner_t *frame_runner = runner_create(nds);
    /* 21-B9yi(续69)：**窗口模式也套用键盘脚本**（`--key-frame/-mask/-period`）。
       此前这些参数只在无头路径生效 ⇒ 所有「窗口带按键」的测量其实都是**无输入**状态，
       而游戏的输入状态会让工作量差 2.6 倍（见 docs/21 续68）⇒ 之前的窗口帧率
       并不能代表「真的在玩」时的表现。这里让窗口与无头用同一套脚本，测量才可比。 */
    if (key_frame != 0 && key_mask != 0)
        runner_set_keys(frame_runner, key_frame, key_mask, key_period);
    /* 21-B9yi(续71)：窗口模式同样支持随机输入浸泡 */
    if (key_random_on)
        runner_set_keys_random(frame_runner, key_frame ? key_frame : 1,
                               key_random_seed, key_period ? key_period : 60);
    if (touch_random_on)
        runner_set_touch_random(frame_runner, touch_frame, touch_random_seed,
                                touch_period ? touch_period : 90);
    /* 21-B9yi(续49)：显式关掉指令级 trace。它是 bring-up 用的诊断设施，
       开着时**每条指令都会 printf**（窗口模式实测 <2 fps + GB 级日志）。 */
    exec_set_trace(0);
    thumb_set_trace(0);

    /* 阶段 6：SDL 按键 → NDS 按键状态（pressed 位=1 表示按下，按下=0 是 NDS 读值）。
       映射见下方 switch；KEY_* 常量来自 io/key.h。 */
    uint16_t keys_pressed = 0;

    /* 阶段 6.4：IRQ pending 只在首次出现时打印一次，避免每帧刷屏 */
    int irq_logged = 0;

    int quit = 0;
    /* 21-B9yi(续42)：`--frames N` 跑满 N 帧后自动退出（窗口模式冒烟测试用：
       验证 SDL 初始化/出图/退出与存档写回，又不用人工关窗口）。 */
    uint64_t frame_limit = g_cli_frames;
    uint64_t frames_done = 0;
    /* 21-B9yi(续61)：分段 fps 统计（--fps-every N）。 */
    uint64_t fps_frames = 0, fps_mark_ms = SDL_GetTicks64();
    /* 21-B9yi(续68)：宿主渲染开销诊断 `NDS_NORENDER=1` —— 跳过清屏/菜单/双屏合成
       与纹理上传（窗口内容不更新），用来量化「窗口模式的帧率里有多少是宿主渲染」。
       依据：同一段 f=8000-10000 在无头下 116.9 fps，窗口下只有 52 fps，
       说明慢段的开销在宿主侧而不是模拟本身。 */
    int skip_render = 0;
    {
        const char *e = getenv("NDS_NORENDER");
        skip_render = (e != NULL && e[0] != '0') ? 1 : 0;
        if (skip_render)
            printf("window: NDS_NORENDER=1（跳过宿主渲染，仅用于测帧率）\n");
    }
    /* 21-B9yi(续68)：分段计时用（仅在 --fps-every 打开时收集，避免干扰正常游玩）。
       目的：找出「窗口模式为什么比无头慢」——把每帧拆成
       「SDL 事件泵」与「runner_run_frame（模拟）」两块。 */
    uint64_t t_evt = 0, t_run = 0, freq = SDL_GetPerformanceFrequency();
    uint64_t t_ppu = 0, t_sdl = 0;   /* 21-B9yi(续70)：宿主渲染再拆分 */
    /* 21-B9yi(续82)：帧节奏（frame pacing）初始化。
       为什么需要：轻场景下模拟器能跑到 150–265 fps，**快于真机**。而音频回调
       是按真实时间驱动 SPU 的 ⇒ 声音与画面脱节（真机不存在这种状态）。
       默认把每帧对齐 NDS 真机帧频（59.8261 Hz ⇒ 16.715 ms/帧）。
       开关：NDS_NOSYNC=1 或 --speed 0 关闭（自动化压测要保持原速度）；
             --speed N 以 N 倍速快进。 */
    const double nds_frame_hz = 59.8261;
    int pace_on = 1;
    double pace_speed = 1.0;
    {
        const char *e = getenv("NDS_NOSYNC");
        if (e != NULL && e[0] != '0') {
            pace_on = 0;
            printf("window: NDS_NOSYNC=1（关闭帧节奏，全速运行）\n");
        }
        if (g_cli_speed < 0.0) {
            /* 未指定：保持默认限速 */
        } else if (g_cli_speed == 0.0) {
            pace_on = 0;
            printf("window: --speed 0（关闭帧节奏，全速运行）\n");
        } else {
            pace_speed = g_cli_speed;
            pace_on = 1;
        }
    }
    uint64_t pace_freq = SDL_GetPerformanceFrequency();
    double pace_ticks = 0.0, pace_next = 0.0;
    if (pace_on && pace_freq != 0) {
        pace_ticks = (double)pace_freq / (nds_frame_hz * pace_speed);
        pace_next = (double)SDL_GetPerformanceCounter() + pace_ticks;
        printf("window: 帧节奏开启 目标 %.2f fps（真机 %.2f fps x%.2f）"
               "；NDS_NOSYNC=1 或 --speed 0 关闭，--speed N 倍速\n",
               nds_frame_hz * pace_speed, nds_frame_hz, pace_speed);
        fflush(stdout);
    }
    /* 21-B9yi(续47)：鼠标 → 触摸屏（底屏）。布局（逻辑坐标）：
       菜单栏 [0,28)、顶屏 [28,220)、底屏 [220,412)。
       触摸 ADC 换算与 runner `--touch-*` 同口径（固件默认校准，每像素 16 单位）。 */
    int touch_mouse_down = 0;
    while (!quit) {
        int scale = window_get_scale();

        SDL_Event e;
        uint64_t c0 = (g_cli_fps_every != 0) ? SDL_GetPerformanceCounter() : 0;
        while (SDL_PollEvent(&e)) {
            if (window_handle_event(&e)) {
                quit = 1;
            } else if (e.type == SDL_MOUSEBUTTONDOWN &&
                       e.button.button == SDL_BUTTON_LEFT) {
                /* 无 logical size，事件坐标即物理坐标；窗口尺寸恒为
                   WIN_W*scale × WIN_H*scale，除以 scale 得逻辑坐标 */
                int lx = e.button.x / scale;
                int ly = e.button.y / scale;
                if (ly >= MENU_H + SCREEN_H && lx < SCREEN_W) {
                    /* 21-B9yi(续47)：底屏区域 → 触摸屏按下（FFXII 这类游戏靠触控操作）。
                       换算：`adc = 0x200 + (px - 33) * 16`（固件默认校准）。 */
                    int sx = lx, sy = ly - (MENU_H + SCREEN_H);
                    int ax = 0x200 + (sx - 33) * 16;
                    int ay = 0x200 + (sy - 33) * 16;
                    if (ax < 0) ax = 0; else if (ax > 0xFFF) ax = 0xFFF;
                    if (ay < 0) ay = 0; else if (ay > 0xFFF) ay = 0xFFF;
                    io_set_touch(nds->io, (uint16_t)ax, (uint16_t)ay, 1);
                    touch_mouse_down = 1;
                } else {
                    int new_scale = menu_handle_click(lx, ly);
                    if (new_scale >= 1) {
                        window_set_scale(new_scale);
                        scale = new_scale;
                    }
                }
            } else if (e.type == SDL_MOUSEBUTTONUP &&
                       e.button.button == SDL_BUTTON_LEFT && touch_mouse_down) {
                io_set_touch(nds->io, 0, 0, 0);   /* 抬笔 */
                touch_mouse_down = 0;
            } else if (e.type == SDL_MOUSEMOTION && touch_mouse_down) {
                /* 拖动 = 笔移动（保持在底屏范围内） */
                int lx = e.motion.x / scale;
                int ly = e.motion.y / scale - (MENU_H + SCREEN_H);
                int sx = lx < 0 ? 0 : (lx > SCREEN_W - 1 ? SCREEN_W - 1 : lx);
                int sy = ly < 0 ? 0 : (ly > SCREEN_H - 1 ? SCREEN_H - 1 : ly);
                int ax = 0x200 + (sx - 33) * 16;
                int ay = 0x200 + (sy - 33) * 16;
                if (ax < 0) ax = 0; else if (ax > 0xFFF) ax = 0xFFF;
                if (ay < 0) ay = 0; else if (ay > 0xFFF) ay = 0xFFF;
                io_set_touch(nds->io, (uint16_t)ax, (uint16_t)ay, 1);
            } else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
                /* 阶段 6.6：把 SDL 键码映射成 NDS 键位并写入 io 模块 */
                int down = (e.type == SDL_KEYDOWN);
                uint16_t bit = 0;
                switch (e.key.keysym.sym) {
                case SDLK_z:       bit = KEY_A;     break;
                case SDLK_x:       bit = KEY_B;     break;
                case SDLK_s:       bit = KEY_X;     break;
                case SDLK_d:       bit = KEY_Y;     break;
                case SDLK_a:       bit = KEY_L;     break;
                case SDLK_f:       bit = KEY_R;     break;
                case SDLK_RETURN:  bit = KEY_START; break;
                case SDLK_BACKSPACE: bit = KEY_SELECT; break;
                case SDLK_UP:      bit = KEY_UP;    break;
                case SDLK_DOWN:    bit = KEY_DOWN;  break;
                case SDLK_LEFT:    bit = KEY_LEFT;  break;
                case SDLK_RIGHT:   bit = KEY_RIGHT; break;
                default:           bit = 0;         break;
                }
                if (bit != 0) {
                    if (down) keys_pressed |= bit;
                    else      keys_pressed &= (uint16_t)~bit;
                    io_set_keyinput(nds->io, keys_pressed);
                }
            }
        }

        /* 阶段 3a 起：每帧推进固定 N 步 CPU（mini.nds 是死循环，PC 原地打转）。
           阶段 4 的画面由上面 CPU 写 VRAM 的测试码产生，这里继续空转。
           阶段 8.4：双核按 ARM9:ARM7 = 2:1 交错调度（i%3==2 时跑 ARM7）。
           每 60 周期打一次状态，避免死循环时刷屏。 */
        if (nds->cpu != NULL && nds->cpu7 != NULL) {
            uint64_t c1 = (g_cli_fps_every != 0) ? SDL_GetPerformanceCounter() : 0;
            if (g_cli_fps_every != 0)
                t_evt += c1 - c0;
            if (frame_runner != NULL)
                runner_run_frame(frame_runner);
            if (g_cli_fps_every != 0)
                t_run += SDL_GetPerformanceCounter() - c1;
            if (nds->cpu->cycles % 60u == 0) {
                printf("cpu: frame done, ARM9 PC=%08X cycles=%llu | ARM7 PC=%08X cycles=%llu\n",
                       nds->cpu->r[15], (unsigned long long)nds->cpu->cycles,
                       nds->cpu7->r[15], (unsigned long long)nds->cpu7->cycles);
                fflush(stdout);
            }
        }

        /* 阶段 6.4：最小 IRQ 检测（不进异常向量，仅观察挂起）。
           真机上这时 CPU 会被叫走跑 handler；本阶段只打印一次。 */
        if (io_irq_pending(nds->io) && !irq_logged) {
            printf("irq: IRQ pending (VBlank IF=%08X IE=%08X IME=%u)\n",
                   bus_read32(nds->bus, IO_IF_ADDR),
                   bus_read32(nds->bus, IO_IE_ADDR),
                   bus_read32(nds->bus, IO_IME_ADDR) & 1u);
            fflush(stdout);
            irq_logged = 1;
        }

        if (!skip_render) {
            uint64_t r0 = (g_cli_fps_every != 0) ? SDL_GetPerformanceCounter() : 0;
            /* 清屏（物理坐标） */
            SDL_SetRenderDrawColor(renderer, 45, 45, 45, 255);
            SDL_RenderClear(renderer);

            menu_render_bar(renderer, scale);

            /* 阶段 4：每帧从 VRAM framebuffer 读图 → 转 RGB888 → 上传纹理 → 画双屏。
               顶屏在菜单栏下，底屏紧随其后，按当前 scale 缩放。 */
            uint64_t r1 = (g_cli_fps_every != 0) ? SDL_GetPerformanceCounter() : 0;
            ppu_render(ppu, scale);
            uint64_t r2 = (g_cli_fps_every != 0) ? SDL_GetPerformanceCounter() : 0;

            /* 下拉菜单（画在游戏区之上） */
            menu_render_dropdown(renderer, scale);

            SDL_RenderPresent(renderer);
            uint64_t r3 = (g_cli_fps_every != 0) ? SDL_GetPerformanceCounter() : 0;
            if (g_cli_fps_every != 0) {
                t_ppu += r2 - r1;                 /* ppu_render：软件合成 + 纹理上传 + 双屏绘制 */
                t_sdl += (r1 - r0) + (r3 - r2);   /* 清屏/菜单 + Present */
            }
        }

        /* 21-B9yi(续61)：每 N 帧打一行「这一段的实测 fps」（--fps-every N）。
           用真实时钟（SDL_GetTicks64）算，含渲染/音频宿主开销，就是玩家实际体验。 */
        if (g_cli_fps_every != 0 && ++fps_frames >= g_cli_fps_every) {
            uint64_t now_ms = SDL_GetTicks64();
            uint64_t dt = now_ms - fps_mark_ms;
            printf("fps: frames=%llu..%llu  %llu ms  %.1f fps\n",
                   (unsigned long long)(frames_done + 1 - fps_frames),
                   (unsigned long long)(frames_done + 1),
                   (unsigned long long)dt,
                   dt ? (1000.0 * (double)fps_frames / (double)dt) : 0.0);
            if (freq != 0) {
                printf("     phases: events=%.0f ms  emulation=%.0f ms  other=%.0f ms\n",
                       1000.0 * (double)t_evt / (double)freq,
                       1000.0 * (double)t_run / (double)freq,
                       (double)dt - 1000.0 * (double)(t_evt + t_run) / (double)freq);
                printf("     render: ppu=%.0f ms  sdl(clear/menu/present)=%.0f ms\n",
                       1000.0 * (double)t_ppu / (double)freq,
                       1000.0 * (double)t_sdl / (double)freq);
            }
            fflush(stdout);
            fps_frames = 0;
            fps_mark_ms = now_ms;
            t_evt = 0; t_run = 0;
            t_ppu = 0; t_sdl = 0;
        }

        if (frame_limit != 0 && ++frames_done >= frame_limit) {
            printf("window: reached --frames %llu, exiting\n",
                   (unsigned long long)frame_limit);
            fflush(stdout);
            quit = 1;
        }

        /* 21-B9yi(续82)：帧节奏等待 —— 把这一帧对齐到「下一帧截止时刻」。
           做法：SDL_Delay 睡掉大部分时间，最后 <1.5 ms 忙等（SDL_Delay 只有
           毫秒粒度，单靠它会带来 ~1 ms 抖动，让 fps 在 55–65 之间晃）。
           若已经落后（拖动窗口、命中断点、或场景比真机还慢），把截止时刻
           重置到「现在 + 一帧」，避免连续追赶式狂跑。 */
        if (pace_ticks > 0.0) {
            double now = (double)SDL_GetPerformanceCounter();
            if (now < pace_next) {
                double wait_ms = (pace_next - now) * 1000.0 / (double)pace_freq;
                if (wait_ms > 1.5)
                    SDL_Delay((uint32_t)(wait_ms - 1.0));
                while ((double)SDL_GetPerformanceCounter() < pace_next) {
                    /* 忙等收尾（<1.5 ms） */
                }
            }
            pace_next += pace_ticks;
            now = (double)SDL_GetPerformanceCounter();
            if (pace_next < now)
                pace_next = now + pace_ticks;
        }
    }

    /* 阶段 16：退出前把存档写回 .sav（须在 nds_destroy 释放存档缓冲之前）。 */
    if (save_path != NULL) {
#ifdef _WIN32
        if (save_save_file_w(io_get_save(nds->io), save_path) == 0)
            printf("save : stored %s\n", utf8_path(save_path));
        else
            printf("save : failed to write %s\n", utf8_path(save_path));
        free(save_path);
#else
        if (save_save_file(io_get_save(nds->io), save_path) == 0)
            printf("save : stored %s\n", save_path);
        else
            printf("save : failed to write %s\n", save_path);
        free(save_path);
#endif
    }

    menu_shutdown();
    audio_shutdown();
    window_shutdown();
    runner_destroy(frame_runner);
    ppu_destroy(ppu);
    nds_destroy(nds);
    cart_free(cart);
    return 0;
}
