#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
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
#include "state/state.h"   /* 21-B9yi(续96)：即时存档/读档 */

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

/* 21-B9yi(续102)：**Ctrl+C / 控制台关闭时也要优雅退出**。
   为什么需要：窗口模式的退出路径会写回 `.sav`（以及 `--save-state` 的存档），
   但如果用户是从终端里 Ctrl+C 结束进程，默认的 SIGINT 会**直接终止**，
   存档就丢了。这里只做一件事：把「收到停止请求」记成标志，主循环下一轮
   主动退出 ⇒ 走完整退出路径（写 .sav / 关闭音频 / 释放）。 */
static volatile sig_atomic_t g_stop_requested;

static void on_stop_signal(int sig)
{
    (void)sig;
    g_stop_requested = 1;
}

/* 21-B9yi(续104)：`--help` 用法说明（用户不必翻 README 就能知道怎么玩/怎么测）。
   内容与 README 的开关表保持一致；新增开关时记得同步。 */
static void usage(const char *exe)
{
    printf("nds-emu —— 一个学习用的 NDS 模拟器（C11 + SDL2）\n\n");
    printf("用法： %s <ROM.nds> [选项]\n\n", exe != NULL ? exe : "nds-emu");
    printf("怎么玩（窗口模式）：\n");
    printf("  Z X S D          NDS 的 A / B / X / Y\n");
    printf("  A F              L / R\n");
    printf("  Enter Backspace  START / SELECT\n");
    printf("  方向键           十字键\n");
    printf("  鼠标按住底屏     触摸屏（按下=触点、拖动=笔移动、抬起=抬笔）\n");
    printf("  按住 Tab         快进 ×4（跳过过场；快进时静音、SPU 跟随模拟时间）\n");
    printf("  F5 / F8          即时存档 / 读档（默认文件 nds_quick.state0）\n");
    printf("  点顶部菜单栏     切换缩放（1x–4x）/ 语言\n\n");
    printf("常用选项：\n");
    printf("  --frames N               窗口跑满 N 帧自动退出（退出时写回 .sav）\n");
    printf("  --fps-every N            每 N 帧打印分段帧率 + phases/render/pace/drift\n");
    printf("  --speed N                帧节奏倍速（1=真机 59.83fps，0=不限速）\n");
    printf("  --snd-wav 文件.wav       无头模式把模拟音频导成 WAV（32768Hz/16bit/立体声）\n");
    printf("  --load-state / --save-state 文件    即时存档读/写（--state-save-frame N 指定帧）\n");
    printf("  --headless-frames N      无头跑 N 帧（长跑/对照用；配合下面的诊断）\n");
    printf("  --key-random SEED [--key-period N]    随机按键浸泡\n");
    printf("  --touch-random SEED [--touch-period N] 随机触摸浸泡\n");
    printf("  --screen-hash-every N    每 N 帧打印双屏画面指纹（跑过多少张不同画面）\n");
    printf("  --stats-every N          每 N 帧打印双屏统计（非黑像素数 + 均值 RGB）\n");
    printf("  --shot 文件.bmp          结束时存双屏截图；--shot-every N --shot-prefix P 存时间线\n");
    printf("  --dump 前缀              结束时导出内存镜像（mainram/arm7wram/vram/itcm/dtcm…）\n");
    printf("  --watch LO-HI / --watch-r LO-HI        总线写/读监视（打印访问者 PC/LR/SP）\n\n");
    printf("常用环境变量：\n");
    printf("  NDS_NOSYNC=1        关闭帧节奏（测性能用）\n");
    printf("  NDS_PACE_DEBT_MS=N  帧节奏欠账上限（默认 30000；100=不追帧）\n");
    printf("  NDS_FF_MUL=N        快进倍数（默认 4）\n");
    printf("  NDS_SNDSTAT=1       打印混音统计；NDS_UNKIOSUM=1 列出全部未知 IO 地址\n");
    printf("  NDS_STATEDBG=1      存档/读档时打印中断与定时器状态\n");
    printf("  NDS_TRACE_FRAME=N + NDS_TRACE_COUNT=K   指令级 trace（对照两条时间线用）\n");
    printf("  NDS_NORENDER=1 / NDS_NOAUDIO=1          窗口跳过宿主渲染 / 不打开声卡\n");
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
    /* 21-B9yi(续96)：即时存档（`--load-state` / `--save-state` [+ `--state-save-frame N`]）。
       两个路径缓冲区是 static，方便把宽路径转成 UTF-8 后长期持有。 */
    static char g_cli_state_buf1[512], g_cli_state_buf2[512];
    const char *g_cli_state_load = NULL, *g_cli_state_save = NULL;
    uint64_t g_cli_state_frame = 0;
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
        else if (wcscmp(wargv[i], L"--dump") == 0 && i + 1 < wargc) {
            /* 21-B9yi(续107)：**必须拷进静态缓冲**，不能存 `wargv[i+1]` 指针。
               Windows 路径在这之后（ROM/存档装载完、开跑之前）会 `LocalFree(wargv)`，
               而 `--dump` 真正写文件是在**整段跑完之后** ⇒ 存指针就是读已释放内存：
               实测前缀时有时无，丢了前缀就把 `_mainram.bin` 之类的文件写到**当前目录**
               （仓库里出现过的那批根目录 `_*.bin` 就是这么来的）。
               `--shot` 当时已经拷进 `shot_buf`，这里是同一个坑的另一处。 */
            static wchar_t dump_prefix_buf[512];
            wcsncpy(dump_prefix_buf, wargv[i + 1], 511);
            dump_prefix_buf[511] = 0;
            dump_prefix_w = dump_prefix_buf;
        }
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
        /* 21-B9yi(续96)：即时存档开关（宽路径 → UTF-8 窄路径，参数解析完再统一下发） */
        else if (wcscmp(wargv[i], L"--load-state") == 0 && i + 1 < wargc) {
            g_cli_state_load = g_cli_state_buf1;
            WideCharToMultiByte(CP_UTF8, 0, wargv[i + 1], -1, g_cli_state_buf1,
                                (int)sizeof g_cli_state_buf1, NULL, NULL);
        }
        else if (wcscmp(wargv[i], L"--save-state") == 0 && i + 1 < wargc) {
            g_cli_state_save = g_cli_state_buf2;
            WideCharToMultiByte(CP_UTF8, 0, wargv[i + 1], -1, g_cli_state_buf2,
                                (int)sizeof g_cli_state_buf2, NULL, NULL);
        }
        else if (wcscmp(wargv[i], L"--state-save-frame") == 0 && i + 1 < wargc)
            g_cli_state_frame = _wcstoui64(wargv[i + 1], NULL, 10);
        else if (wcscmp(wargv[i], L"--help") == 0 || wcscmp(wargv[i], L"-h") == 0) {
            static char exe_buf[260];
            WideCharToMultiByte(CP_UTF8, 0, wargv[0], -1, exe_buf,
                                (int)sizeof exe_buf, NULL, NULL);
            usage(exe_buf);
            LocalFree(wargv);
            return 0;
        }
        else if (wcscmp(wargv[i], L"--speed") == 0 && i + 1 < wargc)
            g_cli_speed = wcstod(wargv[i + 1], NULL);
        else if (wcscmp(wargv[i], L"--snd-wav") == 0 && i + 1 < wargc) {
            /* 21-B9yi(续85)：无头模式把模拟音频写成 WAV（宽路径 → UTF-8 窄路径） */
            static char wav_buf[512];
            WideCharToMultiByte(CP_UTF8, 0, wargv[i + 1], -1, wav_buf,
                                (int)sizeof wav_buf, NULL, NULL);
            runner_set_wav_path(wav_buf);
        }
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
        else if (strcmp(argv[i], "--load-state") == 0 && i + 1 < argc)
            g_cli_state_load = argv[i + 1];
        else if (strcmp(argv[i], "--save-state") == 0 && i + 1 < argc)
            g_cli_state_save = argv[i + 1];
        else if (strcmp(argv[i], "--state-save-frame") == 0 && i + 1 < argc)
            g_cli_state_frame = strtoull(argv[i + 1], NULL, 10);
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        }
        else if (strcmp(argv[i], "--speed") == 0 && i + 1 < argc)
            g_cli_speed = strtod(argv[i + 1], NULL);
        else if (strcmp(argv[i], "--snd-wav") == 0 && i + 1 < argc)
            runner_set_wav_path(argv[i + 1]);
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

    /* 21-B9yi(续96)：把命令行里的即时存档配置下发给 state 模块
       （放在解析循环之后，避免 `--state-save-frame` 与 `--save-state` 的先后顺序影响结果）。 */
    if (g_cli_state_load != NULL)
        state_cli_set_load(g_cli_state_load);
    if (g_cli_state_save != NULL)
        state_cli_set_save(g_cli_state_save, g_cli_state_frame);

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

    /* 21-B9yi(续96)：`--load-state` 在读档点之前把整机状态覆盖掉
       （在 ROM/存档装载之后、开跑之前），并让 runner 把时间轴拉到存档时刻。 */
    if (state_cli_load_now(nds) < 0) {
        fprintf(stderr, "state: 读档失败，退出\n");
        return 1;
    }

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
    int audio_on = 0;   /* 21-B9yi(续92)：声卡是否真的开着（快进恢复时要据此决定宿主渲染标志） */
    {
        const char *e = getenv("NDS_NOAUDIO");
        if (e != NULL && e[0] != '0')
            printf("window: NDS_NOAUDIO=1（不打开声卡，仅用于测帧率）\n");
        else if (audio_init(nds) == 0)
            audio_on = 1;
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
    /* 21-B9yi(续107)：窗口模式同样要在读档后把时间轴拉齐；但**必须放在脚本配置之后**
       ——`runner_resync_time()` 会按当前帧号反推输入脚本相位，而上面的
       `runner_set_keys*()` 会把 `next_press` 重置回脚本起点，顺序反了就白做
       （读档后第一帧立刻补按一次，见 src/runner/runner.c 里同一处的说明）。 */
    if (frame_runner != NULL && state_take_pending_load())
        runner_resync_time(frame_runner);
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
    /* 21-B9yi(续83)：窗口退出摘要用（平均帧率）。 */
    uint64_t win_start_ms = SDL_GetTicks64();
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
    /* 21-B9yi(续92)：**按住 Tab 快进**（默认 ×4）。快进时：
       ①帧节奏目标改成 59.8261×4；②把 SPU 从「声卡按墙钟推进」切回
       「runner 按模拟时间推进」（`snd_set_host_render_active(0)`），
       声卡回调则只输出静音（见 audio.c）——这样 4 倍速下游戏逻辑仍然正确，
       又不会发出错乱的音频。松开 Tab 全部还原。 */
    int ff_hold = 0;
    double ff_mul = 4.0;
    /* 21-B9yi(续93b)：帧节奏误差统计（每帧超出目标截止时刻多少毫秒）——
       用来定位「1× 下重场景只有 58.3 fps、目标 59.83」到底是哪一段吃掉了时间。 */
    double pace_late_sum = 0.0, pace_late_max = 0.0;
    uint64_t pace_late_n = 0;
    uint64_t pace_resync_n = 0;   /* 超时超过一整帧 ⇒ 必须丢弃欠账的帧数（不可追回） */
    /* 21-B9yi(续103)：欠账上限（秒）。默认 **30 s**（≈允许一直追帧）；
       `NDS_PACE_DEBT_MS=N` 可改（毫秒），设成很小的值可以复现旧行为做 A/B。
       为什么默认放大：声卡是**按墙钟**推进的，而重场景会掉到 ~52 fps ⇒
       游戏时间越落越远（实测 12000 帧累计 **7.6 s** 音画漂移）。允许追帧后，
       落下的部分会在随后的轻场景补回来（实测总时长 208.2 s → **200.6 s**，
       漂移 ≈ 0.1 s，平均帧率仍 59.8）。上限保留 30 s 只是为了防「进程被挂起几分钟」
       之后拼命追帧。 */
    double pace_debt_cap = 30.0;
    {
        const char *e = getenv("NDS_PACE_DEBT_MS");
        if (e != NULL && atof(e) > 0.0)
            pace_debt_cap = atof(e) / 1000.0;
    }
    {
        const char *e = getenv("NDS_FF_MUL");
        if (e != NULL && atof(e) > 0.0)
            ff_mul = atof(e);
        /* `NDS_FF_HOLD=1`：启动即视为按住快进（自动化验证 / 基准测试用，等价按 Tab） */
        const char *h = getenv("NDS_FF_HOLD");
        if (h != NULL && h[0] != '0')
            ff_hold = 1;
    }
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
        /* 21-B9yi(续92)：启动时若已「按住快进」（NDS_FF_HOLD=1）就按倍速起步，
           并把 SPU 交给 runner 按模拟时间推进（声卡回调只吐静音）。 */
        double eff = pace_speed * (ff_hold ? ff_mul : 1.0);
        pace_ticks = (double)pace_freq / (nds_frame_hz * eff);
        pace_next = (double)SDL_GetPerformanceCounter() + pace_ticks;
        if (ff_hold) {
            snd_set_host_render_active(0);
            printf("window: 快进 开（启动即按住，×%.1f → 目标 %.1f fps，音频静音）\n",
                   ff_mul, nds_frame_hz * eff);
        }
        printf("window: 帧节奏开启 目标 %.2f fps（真机 %.2f fps x%.2f）"
               "；NDS_NOSYNC=1 或 --speed 0 关闭，--speed N 倍速\n",
               nds_frame_hz * eff, nds_frame_hz, eff);
        fflush(stdout);
    }
    /* 21-B9yi(续47)：鼠标 → 触摸屏（底屏）。布局（逻辑坐标）：
       菜单栏 [0,28)、顶屏 [28,220)、底屏 [220,412)。
       触摸 ADC 换算与 runner `--touch-*` 同口径（固件默认校准，每像素 16 单位）。 */
    int touch_mouse_down = 0;
    /* 21-B9yi(续102)：Ctrl+C 也能走完整退出路径（写回 .sav / 存档状态） */
#ifdef _WIN32
    signal(SIGINT, on_stop_signal);
    signal(SIGBREAK, on_stop_signal);
#else
    signal(SIGINT, on_stop_signal);
    signal(SIGTERM, on_stop_signal);
#endif
    /* 21-B9yi(续104)：启动时打一遍按键/快捷键（省得翻文档；`--help` 有完整说明）。 */
    printf("window: 按键 Z/X/S/D=A/B/X/Y  A/F=L/R  Enter/Backspace=START/SELECT  方向键=十字键\n");
    printf("window: 鼠标按住底屏=触摸屏  Tab=快进  F5/F8=即时存档/读档  菜单栏=缩放/语言"
           "（--help 看全部开关）\n");
    fflush(stdout);
    while (!quit) {
        if (g_stop_requested) {
            printf("window: 收到停止请求，准备退出（写回存档）…\n");
            fflush(stdout);
            quit = 1;
            break;
        }
        int scale = window_get_scale();

        SDL_Event e;
        uint64_t c0 = (g_cli_fps_every != 0) ? SDL_GetPerformanceCounter() : 0;
        /* 21-B9yi(续93b)：本帧的分段计时（事件 / 模拟 / 渲染），供重帧诊断使用 */
        uint64_t c1 = 0, c2 = 0;
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
                case SDLK_TAB:     bit = 0;         break;  /* 快进键（下面单独处理） */
                default:           bit = 0;         break;
                }
                /* 21-B9yi(续92)：按住 Tab = 快进 ×N（松手还原）。 */
                if (e.key.keysym.sym == SDLK_TAB) {
                    ff_hold = down;
                    double eff = pace_speed * (ff_hold ? ff_mul : 1.0);
                    pace_ticks = (pace_freq != 0)
                               ? (double)pace_freq / (nds_frame_hz * eff) : 0.0;
                    pace_next = (double)SDL_GetPerformanceCounter() + pace_ticks;
                    snd_set_host_render_active((ff_hold || !audio_on) ? 0 : 1);
                    printf("window: 快进 %s（目标 %.1f fps，%s）\n",
                           ff_hold ? "开" : "关", nds_frame_hz * eff,
                           ff_hold ? "音频静音、SPU 跟随模拟时间" : "恢复实时音频");
                    fflush(stdout);
                }
                /* 21-B9yi(续96)：F5 = 存即时存档、F8 = 读即时存档。
                   路径默认取 `--save-state/--load-state` 给的；没给就用 `<ROM>.state0`。
                   读档后会重同步调度时间轴（见 runner_resync_time）。 */
                if (down && (e.key.keysym.sym == SDLK_F5 || e.key.keysym.sym == SDLK_F8)) {
                    const char *sp = g_cli_state_save != NULL ? g_cli_state_save
                                                              : "nds_quick.state0";
                    const char *lp = g_cli_state_load != NULL ? g_cli_state_load
                                                              : sp;
                    if (e.key.keysym.sym == SDLK_F5) {
                        /* 注：窗口模式在帧边界上保存，读档方会用「帧号 × 一帧周期」
                           还原时间轴（无需额外时间戳）；无头路径则会带上 runner 的时间戳。 */
                        if (state_save(nds, frames_done, sp) == 0)
                            printf("state: 已保存 %s（帧 %llu）——按 F8 可读回\n", sp,
                                   (unsigned long long)frames_done);
                        else
                            printf("state: 保存失败 %s\n", sp);
                    } else {
                        if (state_load(nds, lp) == 0) {
                            if (frame_runner != NULL)
                                runner_resync_time(frame_runner);
                            printf("state: 已读档 %s（回到帧 %llu）\n", lp,
                                   (unsigned long long)state_last_frame());
                        } else {
                            printf("state: 读档失败 %s\n", lp);
                        }
                    }
                    fflush(stdout);
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
            c1 = (g_cli_fps_every != 0) ? SDL_GetPerformanceCounter() : 0;
            if (g_cli_fps_every != 0)
                t_evt += c1 - c0;
            if (frame_runner != NULL)
                runner_run_frame(frame_runner);
            c2 = (g_cli_fps_every != 0) ? SDL_GetPerformanceCounter() : 0;
            if (g_cli_fps_every != 0)
                t_run += c2 - c1;
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

        /* 21-B9yi(续95)：**快进时隔帧渲染**。
           快进时音频已静音（见续92），观众要的是速度不是画面 ⇒ 隔帧跳过宿主渲染
           （ppu_render + Clear/menu/Present 实测 2.4~4.8 ms/帧），
           让重场景的快进倍率更接近 4×（续92b 实测重场景只有 ~1.1×）。
           注意：只跳过**宿主渲染**，模拟（runner_run_frame）与音频状态照常推进。 */
        int ff_skip_frame = (ff_hold && ((frames_done & 1u) != 0));
        if (!skip_render && !ff_skip_frame) {
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

        /* 21-B9yi(续93b)：**重帧逐帧诊断**（仅 `--fps-every` 打开时）。
           续93 发现重场景每 3000 帧有 27~35 个「超出一整帧预算」的突发帧（最重 37~40 ms），
           这里把超过阈值的帧单独打出来，看时间花在 模拟 / ppu 渲染 / SDL 的哪一块。 */
        if (g_cli_fps_every != 0 && c0 != 0) {
            uint64_t fend = SDL_GetPerformanceCounter();
            double tot_ms = 1000.0 * (double)(fend - c0) / (double)freq;
            if (tot_ms > 25.0) {
                printf("heavy: f=%llu tot=%.1f ms  evt=%.1f  emu=%.1f  render=%.1f\n",
                       (unsigned long long)(frames_done + 1), tot_ms,
                       1000.0 * (double)(c1 - c0) / (double)freq,
                       1000.0 * (double)(c2 - c1) / (double)freq,
                       1000.0 * (double)(fend - c2) / (double)freq);
                fflush(stdout);
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
                if (pace_late_n > 0)
                    printf("     pace: 超时 %llu/%llu 帧，平均 %.3f ms/帧，最大 %.3f ms，"
                           "丢帧重同步 %llu 次（目标 %.2f ms）\n",
                           (unsigned long long)pace_late_n,
                           (unsigned long long)fps_frames,
                           pace_late_sum / (double)pace_late_n, pace_late_max,
                           (unsigned long long)pace_resync_n,
                           1000.0 / (nds_frame_hz * pace_speed));
                else
                    printf("     pace: 无超时（%llu 帧全部赶上目标 %.2f ms，重同步 %llu 次）\n",
                           (unsigned long long)fps_frames,
                           1000.0 / (nds_frame_hz * pace_speed),
                           (unsigned long long)pace_resync_n);
                /* 21-B9yi(续103)：**音画漂移读数** —— 这一段「游戏时间」比墙钟慢/快多少。
                   声卡是按墙钟走的，所以这个数就是「声音相对画面超前多少」的直接量度
                   （正值=画面落后、声音超前）。 */
                {
                    double want_ms = 1000.0 * (double)fps_frames
                                     / (nds_frame_hz * pace_speed);
                    printf("     drift: 本段模拟 %.1f ms vs 墙钟 %llu ms ⇒ 画面落后 %+.1f ms"
                           "（声卡按墙钟走，正值=声音超前）\n",
                           want_ms, (unsigned long long)dt, (double)dt - want_ms);
                }
            }
            fflush(stdout);
            fps_frames = 0;
            fps_mark_ms = now_ms;
            t_evt = 0; t_run = 0;
            t_ppu = 0; t_sdl = 0;
            pace_late_sum = 0.0; pace_late_max = 0.0; pace_late_n = 0;
            pace_resync_n = 0;
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
           21-B9yi(续93)：**落后时不要一律重置截止时刻**。此前「只要落后就
           `pace_next = now + 一帧`」会把每次睡眠多睡的那点时间**丢掉**，
           于是每帧系统性偏慢：实测 18000 帧长跑平均 **58.4 fps**（每帧 17.15 ms，
           目标是 16.71 ms，慢 2.6%）。正确做法是**保留欠账**（下一帧少等一会儿补回来），
           只有落后超过一整帧（拖窗口/断点/场景比真机还慢）才重新对齐。 */
        if (pace_ticks > 0.0) {
            double now = (double)SDL_GetPerformanceCounter();
            if (now < pace_next) {
                double wait_ms = (pace_next - now) * 1000.0 / (double)pace_freq;
                /* 21-B9yi(续93c)：阈值 1.5 ms 的对照实验（试过提到 3 ms「少睡多转」）
                   显示 fps 无变化（58.3/59.8/58.7 完全一样）⇒ 说明误差不来自睡眠粒度，
                   于是**还原 1.5 ms**，避免白烧 CPU。 */
                if (wait_ms > 1.5)
                    SDL_Delay((uint32_t)(wait_ms - 1.0));
                while ((double)SDL_GetPerformanceCounter() < pace_next) {
                    /* 忙等收尾（<1.5 ms） */
                }
            }
            pace_next += pace_ticks;
            now = (double)SDL_GetPerformanceCounter();
            {
                double late_ms = (now - (pace_next - pace_ticks)) * 1000.0
                                 / (double)pace_freq;
                if (late_ms > 0.0) {
                    pace_late_sum += late_ms;
                    if (late_ms > pace_late_max)
                        pace_late_max = late_ms;
                    pace_late_n++;
                }
            }
            /* 21-B9yi(续93c)：**欠账上限改成时间制（100 ms）**。
               保留欠账让下几帧少等、把短突发补回来（旧实现「落后 1 帧就丢」，
               重场景里每次丢掉一整段欠账，27~37 次/3000 帧 ⇒ 58.4 fps）；
               但不能无限保留：声卡是按墙钟推进的，游戏时间必须跟住墙钟，
               所以只允许最多 100 ms 的欠账（够吸收突发，又让漂移有界）。 */
            /* 21-B9yi(续103)：上限可用 `NDS_PACE_DEBT_MS` 调整（默认 100 ms）。
               放宽上限 ⇒ 重场景掉队后会在随后的轻场景**追帧**补回来，
               让「模拟时间」跟住墙钟（声卡按墙钟走）⇒ 音画漂移有界。
               代价：追赶期间会短暂跑得比真机快（视觉上几乎察觉不到）。 */
            if (now - pace_next > pace_debt_cap * (double)pace_freq) {
                pace_next = now + pace_ticks;
                pace_resync_n++;
            }
        }
    }

    /* 21-B9yi(续83)：窗口退出摘要 —— 人工验收的客观判据。
       背景：「战斗内拖拽下令」与「游戏内存档」两项只能人工试玩确认，但**不需要看代码**
       就能判定成败：前者看画面/指令是否变化（配合按 F12 或 `--shot` 留图），后者看下面
       这一行 `savechip:` 的非 0xFF 字节数——**只要从 24 明显变大，就说明游戏真的写过存档**。
       `--shot 路径.bmp` 在窗口模式同样生效（退出时把双屏存成 BMP，顶屏在上）。 */
    if (nds != NULL) {
        uint64_t el = SDL_GetTicks64() - win_start_ms;
        printf("window: summary frames=%llu elapsed=%llu ms avg=%.1f fps"
               "（含帧节奏等待）\n",
               (unsigned long long)frames_done, (unsigned long long)el,
               el ? 1000.0 * (double)frames_done / (double)el : 0.0);
        runner_savechip_report(nds);
        state_cli_save_at_exit(nds, frames_done);   /* 21-B9yi(续96)：--save-state 在退出时落盘 */
        if (headless_shot != NULL) {
            if (runner_save_screenshot(nds, headless_shot) == 0)
                printf("window: screenshot saved to %s\n", headless_shot);
            else
                printf("window: screenshot FAILED (%s)\n", headless_shot);
        }
        fflush(stdout);
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
