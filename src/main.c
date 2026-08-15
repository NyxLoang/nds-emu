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
#include "cpu/exec.h"
#include "cpu/thumb.h"
#include "io/io.h"
#include "snd/snd.h"
#include "ppu/ppu.h"
#include "cart/cart.h"

/* 阶段 9.7：真 2D 演示——不再把 VRAM 当线性 framebuffer，而是用
   DISPCNT / BGxCNT / tile / tilemap / 调色板 寄存器驱动 2D 引擎出图。
   顶屏（Engine A）：8bpp tile 棋盘格 + 一个 OBJ 白色方块；
   底屏（Engine B）：8bpp tile 竖条纹（副引擎自己的寄存器/调色板/VRAM 窗口）。 */

/* ---- 阶段 18.4：SDL 音频回调，把 snd_render 合成的样本送进声卡 ---- */
static nds_t *g_audio_nds = NULL;
static SDL_AudioDeviceID g_audio_dev = 0;
#define AUDIO_BUF_FRAMES 1024
static int16_t g_audio_l[AUDIO_BUF_FRAMES];
static int16_t g_audio_r[AUDIO_BUF_FRAMES];

static void audio_callback(void *userdata, Uint8 *stream, int len)
{
    (void)userdata;
    int frames = len / 4;               /* 2 通道 × 16 位 */
    if (frames > AUDIO_BUF_FRAMES)
        frames = AUDIO_BUF_FRAMES;

    nds_t *nds = g_audio_nds;
    if (nds != NULL) {
        snd_render(&nds->io->snd, nds->bus, g_audio_l, g_audio_r, frames);
    } else {
        for (int i = 0; i < frames; i++) {
            g_audio_l[i] = 0;
            g_audio_r[i] = 0;
        }
    }

    int16_t *out = (int16_t *)stream;
    for (int i = 0; i < frames; i++) {
        out[i * 2 + 0] = g_audio_l[i];
        out[i * 2 + 1] = g_audio_r[i];
    }
}

static int audio_init(nds_t *nds)
{
    g_audio_nds = nds;
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = (int)SND_MIX_RATE;      /* 32768 Hz */
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = AUDIO_BUF_FRAMES;
    want.callback = audio_callback;

    g_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (g_audio_dev == 0) {
        fprintf(stderr, "audio: SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return -1;
    }
    SDL_PauseAudioDevice(g_audio_dev, 0); /* 开播 */
    printf("audio: device opened %d Hz, %d ch, format=%d\n",
           have.freq, have.channels, have.format);
    fflush(stdout);
    return 0;
}

static void audio_shutdown(void)
{
    if (g_audio_dev != 0) {
        SDL_CloseAudioDevice(g_audio_dev);
        g_audio_dev = 0;
    }
    g_audio_nds = NULL;
}

/* 阶段 18.4：demo 音源——PCM8 方波（64 采样/周期）循环播放，约 256Hz 提示音。
   写入 Main RAM 空闲区并配置通道 0 循环播放，验证「寄存器→合成→声卡」全链路。 */
static void setup_audio_demo(nds_t *nds)
{
    const uint32_t base = BUS_MAIN_RAM_BASE + 0x2000;
    const int cycle = 64;
    for (int i = 0; i < cycle; i++)
        bus_write8(nds->bus, base + (uint32_t)i, (i < cycle / 2) ? 0x7F : 0x81);

    bus_write16(nds->bus, SND_SOUNDCNT, 0x807F);   /* 主使能 + 主音量 127 */
    bus_write16(nds->bus, SND_SOUNDBIAS, 0x0200);  /* 中心偏置 0x200 */

    uint32_t ch = SND_BASE;
    bus_write32(nds->bus, ch + 0x0, 0x8840007Fu);  /* PCM8, loop, pan=64, vol=127, start */
    bus_write32(nds->bus, ch + 0x4, base);
    bus_write16(nds->bus, ch + 0x8, 0x0800);       /* tmr=2048 → 约 256Hz */
    bus_write32(nds->bus, ch + 0xC, cycle / 4);    /* len = 64 字节 = 16 字 */

    printf("18 audio: demo tone configured (PCM8 square wave, ~256 Hz)\n");
    fflush(stdout);
}

/* 写 n 个 RGB555 颜色到调色板基址（每项 2 字节） */
static void write_palette(nds_t *nds, uint32_t base, int n, const uint16_t *colors)
{
    for (int i = 0; i < n; i++)
        bus_write16(nds->bus, base + (uint32_t)i * 2u, colors[i]);
}

/* 把一个 8×8 的 8bpp tile 填成单一调色板索引（64 字节全 = idx） */
static void fill_tile_8bpp(nds_t *nds, uint32_t tile_base, int idx)
{
    for (int i = 0; i < 64; i++)
        bus_write8(nds->bus, tile_base + (uint32_t)i, (uint8_t)idx);
}

static void setup_2d_demo(nds_t *nds)
{
    static const uint16_t pal[8] = {
        0x0000, /* 0 黑（背景/透明露出色） */
        0x7C00, /* 1 红 */
        0x03E0, /* 2 绿 */
        0x001F, /* 3 蓝 */
        0x7FE0, /* 4 黄 */
        0x03FF, /* 5 青 */
        0x7C1F, /* 6 品红 */
        0x7FFF, /* 7 白 */
    };

    /* === 顶屏 Engine A：8bpp tile 棋盘格 + OBJ === */
    /* 主 BG 调色板（0x05000000） */
    write_palette(nds, BUS_PALETTE_BASE, 8, pal);

    /* 4 个 8bpp tile：tile0=红 tile1=绿 tile2=蓝 tile3=黄，字符块 0（0x06000000） */
    fill_tile_8bpp(nds, BUS_VRAM_BASE + 0u * 64u, 1);
    fill_tile_8bpp(nds, BUS_VRAM_BASE + 1u * 64u, 2);
    fill_tile_8bpp(nds, BUS_VRAM_BASE + 2u * 64u, 3);
    fill_tile_8bpp(nds, BUS_VRAM_BASE + 3u * 64u, 4);

    /* tilemap（屏幕块 1 → 0x06000800）：32×32 棋盘格，用 (tx+ty)&3 选 tile */
    for (int ty = 0; ty < 32; ty++)
        for (int tx = 0; tx < 32; tx++)
            bus_write16(nds->bus, BUS_VRAM_BASE + 0x800u + (uint32_t)(ty * 32 + tx) * 2u,
                        (uint16_t)((tx + ty) & 3));

    /* BG0CNT：256 色(bit7) + 屏幕块 1(bit8-12 单位 2KB) */
    bus_write16(nds->bus, IO_BGCNT_BASE,
                BGCNT_COLORS_256 | (1u << BGCNT_SCREEN_BASE_SHIFT));

    /* OBJ：一个白色 8×8 方块（16 色），放在 (120,80) */
    bus_write16(nds->bus, BUS_PALETTE_BASE + 0x200u + 2u, 0x7FFFu); /* OBJ 调色板[1]=白 */
    for (int r = 0; r < 8; r++) {   /* OBJ tile0（4bpp）：全部像素 = 索引1 */
        bus_write8(nds->bus, BUS_VRAM_MAIN_OBJ_BASE + 4u * r + 0u, 0xFFu);
        bus_write8(nds->bus, BUS_VRAM_MAIN_OBJ_BASE + 4u * r + 1u, 0x00u);
        bus_write8(nds->bus, BUS_VRAM_MAIN_OBJ_BASE + 4u * r + 2u, 0x00u);
        bus_write8(nds->bus, BUS_VRAM_MAIN_OBJ_BASE + 4u * r + 3u, 0x00u);
    }
    for (int n = 0; n < 128; n++)   /* 全 0 的 OAM = 原点可见 sprite，先统一禁用 */
        bus_write16(nds->bus, BUS_OAM_BASE + 8u * n, 0x0200u);
    bus_write16(nds->bus, BUS_OAM_BASE + 0u, 80u);   /* 属性0：Y=80 方形 16 色 */
    bus_write16(nds->bus, BUS_OAM_BASE + 2u, 120u);  /* 属性1：X=120 尺寸 8×8 */
    bus_write16(nds->bus, BUS_OAM_BASE + 4u, 0u);    /* 属性2：tile0 调色板0 */

    /* DISPCNT（主）：mode 0 + BG0 + OBJ + 显示模式 1（正常） */
    bus_write32(nds->bus, IO_DISPCNT,
                DISPCNT_BG0 | DISPCNT_OBJ | (1u << DISPCNT_DISPLAY_MODE_SHIFT));

    /* === 底屏 Engine B：8bpp tile 竖条纹（青/品红），副引擎独立资源 === */
    static const uint16_t pal_sub[8] = {
        0x0000, 0x03FF, 0x7C1F, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    };
    write_palette(nds, BUS_PALETTE_BASE + 0x400u, 8, pal_sub); /* 副 BG 调色板 */

    /* 副 BG 图形窗口 0x06200000（物理 vram[0x40000]）：tile0=青 tile1=品红 */
    fill_tile_8bpp(nds, BUS_VRAM_SUB_BG_BASE + 0u * 64u, 1);
    fill_tile_8bpp(nds, BUS_VRAM_SUB_BG_BASE + 1u * 64u, 2);

    /* 副 tilemap（屏幕块 1 → 0x06200800）：按列奇偶选 tile，形成竖条纹 */
    for (int ty = 0; ty < 32; ty++)
        for (int tx = 0; tx < 32; tx++)
            bus_write16(nds->bus, BUS_VRAM_SUB_BG_BASE + 0x800u + (uint32_t)(ty * 32 + tx) * 2u,
                        (uint16_t)(tx & 1));

    /* 副 BG0CNT：256 色 + 屏幕块 1 */
    bus_write16(nds->bus, IO_BGCNT_SUB_BASE,
                BGCNT_COLORS_256 | (1u << BGCNT_SCREEN_BASE_SHIFT));

    /* DISPCNT_SUB：mode 0 + BG0 + 显示模式 1 */
    bus_write32(nds->bus, IO_DISPCNT_SUB,
                DISPCNT_BG0 | (1u << DISPCNT_DISPLAY_MODE_SHIFT));

    printf("9 display: true 2D demo configured (top=tiled checkerboard+OBJ, bottom=stripes)\n");
    fflush(stdout);
}

/* 阶段 16：由 ROM 路径派生 .sav 存档路径（替换扩展名为 .sav）。 */
#ifdef _WIN32
static wchar_t *make_save_path_w(const wchar_t *rom_path)
{
    size_t len = wcslen(rom_path);
    wchar_t *p = (wchar_t *)malloc((len + 5) * sizeof(wchar_t));
    if (p == NULL)
        return NULL;
    wcscpy(p, rom_path);
    wchar_t *dot = wcsrchr(p, L'.');
    if (dot != NULL)
        *dot = L'\0';
    wcscat(p, L".sav");
    return p;
}
#else
static char *make_save_path(const char *rom_path)
{
    size_t len = strlen(rom_path);
    char *p = (char *)malloc(len + 5);
    if (p == NULL)
        return NULL;
    strcpy(p, rom_path);
    char *dot = strrchr(p, '.');
    if (dot != NULL)
        *dot = '\0';
    strcat(p, ".sav");
    return p;
}
#endif

/* 阶段 21 bring-up：headless 跑 N 步，只打印诊断事件与周期性进度，最后打印两核状态。
   用于在不开窗口/音频的情况下定位 ROM 的第一个卡点。 */
static void run_headless(nds_t *nds, uint64_t steps, int trace)
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
        /* 每 100 万步打一次进度 */
        if ((i & 0xFFFFFu) == 0xFFFFFu)
            printf("headless: step=%llu ARM9 PC=%08X cyc=%llu | ARM7 PC=%08X cyc=%llu\n",
                   (unsigned long long)(i + 1),
                   nds->cpu->r[15], (unsigned long long)nds->cpu->cycles,
                   nds->cpu7->r[15], (unsigned long long)nds->cpu7->cycles);
    }

    printf("headless: done. ARM9 PC=%08X cyc=%llu | ARM7 PC=%08X cyc=%llu\n",
           nds->cpu->r[15], (unsigned long long)nds->cpu->cycles,
           nds->cpu7->r[15], (unsigned long long)nds->cpu7->cycles);
    fflush(stdout);
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
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
#ifdef _WIN32
    for (int i = 1; i < wargc; i++) {
        if (wcscmp(wargv[i], L"--headless") == 0 && i + 1 < wargc)
            headless_steps = wcstol(wargv[i + 1], NULL, 10);
        else if (wcscmp(wargv[i], L"--trace") == 0)
            headless_trace = 1;
    }
#else
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--headless") == 0 && i + 1 < argc)
            headless_steps = strtol(argv[i + 1], NULL, 10);
        else if (strcmp(argv[i], "--trace") == 0)
            headless_trace = 1;
    }
#endif

    char err[256];

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
        save_path = make_save_path_w(rom_path);
        if (save_path != NULL) {
            save_load_file_w(io_get_save(nds->io), save_path);
            printf("save : loaded %ls (%zu bytes)\n", save_path,
                   io_get_save(nds->io)->size);
        }
#else
        save_path = make_save_path(rom_path);
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
    if (headless_steps > 0) {
        run_headless(nds, (uint64_t)headless_steps, headless_trace);
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
        setup_2d_demo(nds);
        setup_audio_demo(nds);

        /* demo 完毕后恢复 mini.nds 入口，让主循环继续死循环空转 */
        cpu_reset(nds->cpu, 0x02000800);
        fflush(stdout);
    }

    /* 阶段 18.4：打开音频设备，回调线程开始按当前寄存器状态合成输出 */
    audio_init(nds);

    /* 阶段 4.6：默认 2× 缩放启动（菜单下拉可切回 1x/2x） */
    window_set_scale(2);

    /* 每帧执行的 CPU 步数（阶段 3a 起固定 N 步；阶段 4 已能出图） */
    const int steps_per_frame = 8;

    /* 阶段 6：SDL 按键 → NDS 按键状态（pressed 位=1 表示按下，按下=0 是 NDS 读值）。
       映射见下方 switch；KEY_* 常量来自 io/key.h。 */
    uint16_t keys_pressed = 0;

    /* 阶段 6.4：IRQ pending 只在首次出现时打印一次，避免每帧刷屏 */
    int irq_logged = 0;

    int quit = 0;
    while (!quit) {
        int scale = window_get_scale();

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (window_handle_event(&e)) {
                quit = 1;
            } else if (e.type == SDL_MOUSEBUTTONDOWN &&
                       e.button.button == SDL_BUTTON_LEFT) {
                /* 无 logical size，事件坐标即物理坐标；窗口尺寸恒为
                   WIN_W*scale × WIN_H*scale，除以 scale 得逻辑坐标 */
                int new_scale = menu_handle_click(e.button.x / scale,
                                                  e.button.y / scale);
                if (new_scale >= 1) {
                    window_set_scale(new_scale);
                    scale = new_scale;
                }
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
            for (int i = 0; i < steps_per_frame; i++) {
                if (i % 3 == 2)
                    cpu_step(nds->cpu7);
                else
                    cpu_step(nds->cpu);
            }
            if (nds->cpu->cycles % 60u == 0) {
                printf("cpu: frame done, ARM9 PC=%08X cycles=%llu | ARM7 PC=%08X cycles=%llu\n",
                       nds->cpu->r[15], (unsigned long long)nds->cpu->cycles,
                       nds->cpu7->r[15], (unsigned long long)nds->cpu7->cycles);
                fflush(stdout);
            }
        }

        /* 阶段 6.3：指令计数近似产生 VBlank——每帧步数跑完即视为一帧结束。
           真机是显示硬件自动置位，这里模拟同一件事。 */
        io_set_vblank(nds->io);

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

        /* 清屏（物理坐标） */
        SDL_SetRenderDrawColor(renderer, 45, 45, 45, 255);
        SDL_RenderClear(renderer);

        menu_render_bar(renderer, scale);

        /* 阶段 4：每帧从 VRAM framebuffer 读图 → 转 RGB888 → 上传纹理 → 画双屏。
           顶屏在菜单栏下，底屏紧随其后，按当前 scale 缩放。 */
        ppu_render(ppu, scale);

        /* 下拉菜单（画在游戏区之上） */
        menu_render_dropdown(renderer, scale);

        SDL_RenderPresent(renderer);
    }

    /* 阶段 16：退出前把存档写回 .sav（须在 nds_destroy 释放存档缓冲之前）。 */
    if (save_path != NULL) {
#ifdef _WIN32
        if (save_save_file_w(io_get_save(nds->io), save_path) == 0)
            printf("save : stored %ls\n", save_path);
        else
            printf("save : failed to write %ls\n", save_path);
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
    ppu_destroy(ppu);
    nds_destroy(nds);
    cart_free(cart);
    return 0;
}
