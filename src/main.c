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
#include "io/io.h"
#include "ppu/ppu.h"
#include "cart/cart.h"
#include "cart/save.h"
#include "audio/audio.h"
#include "demo/demo.h"
#include "runner/runner.h"

/* 小端工具：装载阶段给 0x027FFxxx 直接启动表填 ROM 头信息时用
   （melonDS SetupDirectBoot 口径，阶段 21-B8）。 */
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
    dump_part_t parts[] = {
        { "mainram",    nds->bus->main_ram,    BUS_MAIN_RAM_SIZE },
        { "arm7wram",   nds->bus->arm7_wram,   BUS_ARM7_WRAM_SIZE },
        { "sharedwram", nds->bus->shared_wram, BUS_SHARED_WRAM_SIZE },
        { "vram",       nds->bus->vram,        BUS_VRAM_SIZE },
        { "itcm",       nds->bus->arm9_itcm,   BUS_ARM9_ITCM_SIZE },
        { "dtcm",       nds->bus->arm9_dtcm,   BUS_ARM9_DTCM_SIZE },
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
            printf("save : loaded %ls (%zu bytes)\n", save_path,
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
    audio_init(nds);

    /* 阶段 4.6：默认 2× 缩放启动（菜单下拉可切回 1x/2x） */
    window_set_scale(2);

    /* 21-B9wq：持久化帧驱动调度器（与 headless 共用事件/周期成本模型） */
    runner_t *frame_runner = runner_create(nds);

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
            if (frame_runner != NULL)
                runner_run_frame(frame_runner);
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
    runner_destroy(frame_runner);
    ppu_destroy(ppu);
    nds_destroy(nds);
    cart_free(cart);
    return 0;
}
