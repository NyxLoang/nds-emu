#include <stdio.h>
#include <SDL.h>
#include "nds/nds.h"   /* nds_t（经 bus.h 得到 bus_t） */
#include "io/io.h"     /* io_t 完整定义（snd 字段在 io 里） */
#include "snd/snd.h"   /* SND_MIX_RATE / snd_render */
#include "audio.h"

/* 阶段 18.4：SDL 音频回调，把 snd_render 合成的样本送进声卡。
   snd/ 只做纯混音（无 SDL），本模块是「主机侧」设备：打开设备 + 回调取样本。 */
static nds_t *g_audio_nds = NULL;
static SDL_AudioDeviceID g_audio_dev = 0;
static int g_audio_owns_subsystem;   /* 21-B9yi(续42)：音频子系统是否由本模块初始化 */
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
    /* 21-B9yi(续92)：**宿主渲染关闭时本回调只吐静音**。
       为什么：快进（按住 Tab）改由 runner 按**模拟时间**推进 SPU，此时声卡若还按
       墙钟去调 snd_render，既与 4 倍速的游戏时间脱节，又会把通道状态**多推进一次**。 */
    if (nds != NULL && snd_host_render_active()) {
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

int audio_init(nds_t *nds)
{
    g_audio_nds = nds;
    /* 21-B9yi(续42)：**SDL 音频子系统必须先初始化**。窗口模式只 `SDL_Init(VIDEO)`，
       此前直接 `SDL_OpenAudioDevice` 会失败（"Audio subsystem is not initialized"）
       ⇒ 窗口模式全程没有声音。这里按需初始化音频子系统（已初始化则跳过），
       关机时只退出我们自己初始化的那个。 */
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            fprintf(stderr, "audio: SDL_InitSubSystem(AUDIO) failed: %s\n",
                    SDL_GetError());
            return -1;
        }
        g_audio_owns_subsystem = 1;
    }
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
    snd_set_host_render_active(1);        /* 21-B9wu：回调负责推进 SPU */
    printf("audio: device opened %d Hz, %d ch, format=%d\n",
           have.freq, have.channels, have.format);
    fflush(stdout);
    return 0;
}

void audio_shutdown(void)
{
    if (g_audio_dev != 0) {
        SDL_CloseAudioDevice(g_audio_dev);
        g_audio_dev = 0;
    }
    snd_set_host_render_active(0);
    g_audio_nds = NULL;
    if (g_audio_owns_subsystem) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        g_audio_owns_subsystem = 0;
    }
}
