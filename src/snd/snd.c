#include "snd.h"
#include "bus/bus.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>   /* 21-B9yi(续60)：getenv（NDS_SNDSTAT） */

/* 21-B9xl 诊断：通道启动/结束打点（带帧号），用于与参考核对照“通道生命周期”。 */
extern unsigned long long g_dbg_frame;
static int g_snd_evt_log = 0;
/* 21-B9yi(续88)：每通道启动/结束计数（覆盖整轮，不受下面打印截断影响） */
static uint32_t s_ch_starts[SND_CHANNEL_COUNT];
static uint32_t s_ch_ends[SND_CHANNEL_COUNT];
/* 21-B9yi(续90)：启动时的「重复模式」与「循环点 PNT 是否为 0」普查 ——
   用来判断「本地循环时回绕到 0、忽略 PNT」到底是不是真缺陷。
   s_start_rep[0]=手动、[1]=循环、[2]=单发、[3]=保留；s_start_pnt_nz = PNT≠0 的启动次数。 */
static uint32_t s_start_rep[4];
static uint32_t s_start_pnt_nz;
static uint32_t s_start_pnt_ch[SND_CHANNEL_COUNT];
/* 21-B9yi(续90)：真正发生「回绕（循环）」的次数，以及其中 PNT≠0 的次数 ——
   决定「忽略 PNT 的简化循环」对本作有没有实际影响。 */
static uint32_t s_loop_events;
static uint32_t s_loop_events_pnt_nz;
/* 21-B9yi(续88)：按格式统计「混进来的样本数」（0=PCM8/1=PCM16/2=ADPCM/3=PSG/4=噪声通道）。
   只在 NDS_SNDSTAT 打开时累加（那一层本来就在热路径里判过，不额外增加常态开销）。 */
static uint64_t s_fmt_samples[5];

static void snd_log(const char *what, int ch)
{
    /* 21-B9yi(续88)：计数放在「打印截断」之前，保证统计覆盖整轮运行 */
    if (ch >= 0 && ch < SND_CHANNEL_COUNT) {
        if (what[0] == 's')
            s_ch_starts[ch]++;
        else if (what[0] == 'e')
            s_ch_ends[ch]++;
    }
    if (g_snd_evt_log >= 240)
        return;
    g_snd_evt_log++;
    printf("snd: ch=%d %s f=%llu\n", ch, what, g_dbg_frame);
}

void snd_channel_report(void)
{
    printf("sndch: starts/ends");
    for (int i = 0; i < SND_CHANNEL_COUNT; i++)
        if (s_ch_starts[i] || s_ch_ends[i])
            printf(" %d=%u/%u", i, s_ch_starts[i], s_ch_ends[i]);
    printf("\n");
    if (s_fmt_samples[0] || s_fmt_samples[1] || s_fmt_samples[2] ||
        s_fmt_samples[3] || s_fmt_samples[4])
        printf("sndfmt: pcm8=%llu pcm16=%llu adpcm=%llu psg=%llu noise=%llu\n",
               (unsigned long long)s_fmt_samples[0],
               (unsigned long long)s_fmt_samples[1],
               (unsigned long long)s_fmt_samples[2],
               (unsigned long long)s_fmt_samples[3],
               (unsigned long long)s_fmt_samples[4]);
    if (s_start_rep[0] || s_start_rep[1] || s_start_rep[2] || s_start_rep[3] ||
        s_start_pnt_nz)
        printf("sndrep: manual=%u loop=%u oneshot=%u rsv=%u | PNT!=0 的启动=%u\n",
               s_start_rep[0], s_start_rep[1], s_start_rep[2], s_start_rep[3],
               s_start_pnt_nz);
    printf("sndloop: 回绕=%u（其中 PNT!=0 时回绕=%u）\n",
           s_loop_events, s_loop_events_pnt_nz);
    fflush(stdout);
}

/* ---- 寄存器地址换算 ---- */

static int snd_channel_of(uint32_t addr)
{
    if (addr >= SND_BASE && addr < SND_BASE + SND_CHANNEL_COUNT * SND_CH_STRIDE)
        return (int)((addr - SND_BASE) / SND_CH_STRIDE);
    return -1;
}

int snd_is_addr(uint32_t addr)
{
    return addr >= SND_BASE && addr < SND_END;
}

/* 21-B9xi：复位到真机缺省值。SOUNDBIAS=0x200 是静音中值
   （混音公式 (bias<<6)-0x8000 正好抵消为 0），也是真实硬件上电值。 */
void snd_reset(snd_t *s)
{
    memset(s, 0, sizeof(*s));
    s->soundbias = 0x200u;
}

uint8_t snd_read8(const snd_t *s, uint32_t addr)
{
    if (addr == SND_SOUNDCNT)     return (uint8_t)(s->soundcnt & 0xFF);
    if (addr == SND_SOUNDCNT + 1) return (uint8_t)(s->soundcnt >> 8);
    /* SOUNDBIAS 只有 bit0-9（melonDS：Bias = val & 0x3FF） */
    if (addr == SND_SOUNDBIAS)    return (uint8_t)(s->soundbias & 0xFFu);
    if (addr == SND_SOUNDBIAS + 1) return (uint8_t)((s->soundbias >> 8) & 0x03u);
    /* 21-B9yi(续87)：捕获单元。参考核能读回的只有 Cnt（0x508/0x509）与
       DstAddr（0x510-0x513 / 0x518-0x51B）；Length 参考核也读不到（返回 0）。 */
    if (addr == 0x04000508u) return s->cap_cnt[0];
    if (addr == 0x04000509u) return s->cap_cnt[1];
    if (addr >= 0x04000510u && addr < 0x04000514u)
        return (uint8_t)(s->cap_dst[0] >> ((addr - 0x04000510u) * 8));
    if (addr >= 0x04000518u && addr < 0x0400051Cu)
        return (uint8_t)(s->cap_dst[1] >> ((addr - 0x04000518u) * 8));

    int ch = snd_channel_of(addr);
    if (ch < 0)
        return 0;
    const snd_channel_t *c = &s->ch[ch];
    uint32_t off = addr - (SND_BASE + (uint32_t)ch * SND_CH_STRIDE);
    switch (off) {
    case 0: case 1: case 2: case 3:
        return (uint8_t)(c->cnt >> (off * 8));
    case 4: case 5: case 6: case 7:
        return (uint8_t)(c->sad >> ((off - 4) * 8));
    case 8: case 9:
        return (uint8_t)(c->tmr >> ((off - 8) * 8));
    case 0xA: case 0xB:
        return (uint8_t)(c->pnt >> ((off - 0xA) * 8));
    case 0xC: case 0xD: case 0xE: case 0xF:
        return (uint8_t)(c->len >> ((off - 0xC) * 8));
    }
    return 0;
}

static void snd_reset_channel(snd_channel_t *c)
{
    c->pos = 0;
    c->adpcm_sample = 0;
    c->adpcm_index = 0;
    c->adpcm_remaining = 0;
    c->adpcm_word = 0;
    c->adpcm_cursor = 0;
    c->adpcm_started = 0;
    c->noise = 0x7FFFu;   /* 21-B9yi(续88)：噪声 LFSR 初值（参考核 Start() 同值） */
    c->adpcm_loop_sample = 0;   /* 21-B9yi(续90)：循环现场（参考核 Reset/Start 同口径） */
    c->adpcm_loop_index = 0;
    c->adpcm_loop_valid = 0;
}

void snd_write8(snd_t *s, uint32_t addr, uint8_t val)
{
    if (addr == SND_SOUNDCNT) {
        s->soundcnt = (uint16_t)((s->soundcnt & 0xFF00u) | val);
        return;
    }
    if (addr == SND_SOUNDCNT + 1) {
        s->soundcnt = (uint16_t)((s->soundcnt & 0x00FFu) | ((uint16_t)val << 8));
        return;
    }
    if (addr == SND_SOUNDBIAS) {
        s->soundbias = (uint16_t)(((s->soundbias & 0x0300u) | val) & 0x03FFu);
        return;
    }
    if (addr == SND_SOUNDBIAS + 1) {
        s->soundbias = (uint16_t)((s->soundbias & 0x00FFu) | ((uint16_t)(val & 0x03u) << 8));
        return;
    }
    /* 21-B9yi(续87)：捕获单元寄存器（0x04000508-0x0400051F）。
       参考核语义：0x508/509 写 Cnt；0x510/518 写 DstAddr；0x514/51C 写 Length（低 16 位）。
       真正的「把混音写进内存」不在本轮范围（游戏只读了 Cnt，没有启动捕获）。 */
    if (addr == 0x04000508u) { s->cap_cnt[0] = val; return; }
    if (addr == 0x04000509u) { s->cap_cnt[1] = val; return; }
    if (addr >= 0x04000510u && addr < 0x04000514u) {
        unsigned sh = (unsigned)(addr - 0x04000510u) * 8;
        s->cap_dst[0] = (s->cap_dst[0] & ~(0xFFu << sh)) | ((uint32_t)val << sh);
        return;
    }
    if (addr >= 0x04000518u && addr < 0x0400051Cu) {
        unsigned sh = (unsigned)(addr - 0x04000518u) * 8;
        s->cap_dst[1] = (s->cap_dst[1] & ~(0xFFu << sh)) | ((uint32_t)val << sh);
        return;
    }
    if (addr == 0x04000514u || addr == 0x04000515u) {
        unsigned sh = (unsigned)(addr - 0x04000514u) * 8;
        s->cap_len[0] = (uint16_t)((s->cap_len[0] & ~(0xFFu << sh)) | ((uint16_t)val << sh));
        return;
    }
    if (addr == 0x0400051Cu || addr == 0x0400051Du) {
        unsigned sh = (unsigned)(addr - 0x0400051Cu) * 8;
        s->cap_len[1] = (uint16_t)((s->cap_len[1] & ~(0xFFu << sh)) | ((uint16_t)val << sh));
        return;
    }

    int ch = snd_channel_of(addr);
    if (ch < 0)
        return;
    snd_channel_t *c = &s->ch[ch];
    uint32_t off = addr - (SND_BASE + (uint32_t)ch * SND_CH_STRIDE);

    switch (off) {
    case 0: case 1: case 2: case 3: {
        uint32_t old = c->cnt;
        uint32_t mask = (uint32_t)0xFFu << (off * 8);
        c->cnt = (c->cnt & ~mask) | (((uint32_t)val << (off * 8)) & mask);
        /* bit31 0→1：启动并复位游标；bit31 1→0：停止 */
        if (!(old & SNDCNT_START) && (c->cnt & SNDCNT_START))
        {
            snd_log("start", ch);
            snd_reset_channel(c);
            /* 21-B9yi(续90)：启动时刻的重复模式 / 循环点普查 */
            {
                uint32_t rep = (c->cnt >> SNDCNT_REPEAT_SHIFT) & 3u;
                s_start_rep[rep]++;
                if (c->pnt != 0) {
                    s_start_pnt_nz++;
                    if (ch >= 0 && ch < SND_CHANNEL_COUNT)
                        s_start_pnt_ch[ch]++;
                }
            }
        }
        break;
    }
    case 4: case 5: case 6: case 7: {
        uint32_t mask = (uint32_t)0xFFu << ((off - 4) * 8);
        c->sad = (c->sad & ~mask) | (((uint32_t)val << ((off - 4) * 8)) & mask);
        break;
    }
    case 8: case 9: {
        uint32_t mask = (uint32_t)0xFFu << ((off - 8) * 8);
        c->tmr = (uint16_t)((c->tmr & (uint16_t)~mask) | (uint16_t)(((uint16_t)val << ((off - 8) * 8)) & mask));
        break;
    }
    case 0xA: case 0xB: {
        uint32_t mask = (uint32_t)0xFFu << ((off - 0xA) * 8);
        c->pnt = (uint16_t)((c->pnt & (uint16_t)~mask) | (uint16_t)(((uint16_t)val << ((off - 0xA) * 8)) & mask));
        break;
    }
    case 0xC: case 0xD: case 0xE: case 0xF: {
        uint32_t mask = (uint32_t)0xFFu << ((off - 0xC) * 8);
        c->len = (c->len & ~mask) | (((uint32_t)val << ((off - 0xC) * 8)) & mask);
        break;
    }
    }
}

/* ---- ADPCM ---- */

/* 21-B9yi(续89)：**IMA-ADPCM 按参考核（melonDS SPU.cpp）重写**。
   为什么重要：本作音频样本里 **99.5% 是 ADPCM**（见续88 的格式普查），
   而本地此前是「16 项小步进表 + 自定头部位布局 + 32 位字内 MSB-first 取 nibble」，
   三处都与硬件不符 ⇒ 解码动态范围被压到很小、且有相位/位序错位。
   参考核口径：
     * 头部 4 字节：bits0-15 = 16 位初始采样（有符号）；bits16-22 = 初始 index(0..88)
     * 数据 nibble 从 SAD+4 起，**每字节低 nibble 在前**（2 nibble/字节）
     * 前 8 个源采样输出 0（4 字节头部当热身 nibble 消耗掉，参考核 Pos 0..7 直接返回）
     * 步进表 89 项（IMA），index 按 nibble 低 3 位查 {-1,-1,-1,-1,2,4,6,8} 后夹在 0..88
     * 采样夹在 ±0x7FFF（不是 -0x8000） */
static const uint16_t s_adpcm_table[89] = {
    0x0007, 0x0008, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E,
    0x0010, 0x0011, 0x0013, 0x0015, 0x0017, 0x0019, 0x001C, 0x001F,
    0x0022, 0x0025, 0x0029, 0x002D, 0x0032, 0x0037, 0x003C, 0x0042,
    0x0049, 0x0050, 0x0058, 0x0061, 0x006B, 0x0076, 0x0082, 0x008F,
    0x009D, 0x00AD, 0x00BE, 0x00D1, 0x00E6, 0x00FD, 0x0117, 0x0133,
    0x0151, 0x0173, 0x0198, 0x01C1, 0x01EE, 0x0220, 0x0256, 0x0292,
    0x02D4, 0x031C, 0x036C, 0x03C3, 0x0424, 0x048E, 0x0502, 0x0583,
    0x0610, 0x06AB, 0x0756, 0x0812, 0x08E0, 0x09C3, 0x0ABD, 0x0BD0,
    0x0CFF, 0x0E4C, 0x0FBA, 0x114C, 0x1307, 0x14EE, 0x1706, 0x1954,
    0x1BDC, 0x1EA5, 0x21B6, 0x2515, 0x28CA, 0x2CDF, 0x315B, 0x364B,
    0x3BB9, 0x41B2, 0x4844, 0x4F7E, 0x5771, 0x602F, 0x69CE, 0x7462,
    0x7FFF
};
static const int8_t s_adpcm_index_tbl[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

static void snd_adpcm_read_header(snd_channel_t *c, const struct bus *bus)
{
    uint32_t hdr = bus != NULL ? bus_read32(bus, c->sad) : 0;
    c->adpcm_sample = (int32_t)(int16_t)(hdr & 0xFFFFu);   /* bits0-15：16 位初始采样 */
    uint32_t idx = (hdr >> 16) & 0x7Fu;                    /* bits16-22：初始 index */
    if (idx > 88u)
        idx = 88u;
    c->adpcm_index = idx;
    c->adpcm_remaining = 0;
    c->adpcm_cursor = 0;
    c->adpcm_started = 1;
}

/* 解码「源采样序号 adpcm_cursor」这一个 nibble，并把游标 +1。
   前 8 个（头部热身）输出 0 且不改动解码状态。 */
static void snd_adpcm_decode_one(snd_channel_t *c, const struct bus *bus)
{
    uint32_t n = c->adpcm_cursor;
    if (n < 8u) {
        c->adpcm_cursor = n + 1u;
        return;
    }
    uint32_t k = n - 8u;                       /* 数据区里的第几个 nibble */
    uint8_t byte = bus != NULL ? bus_read8(bus, c->sad + 4u + (k >> 1)) : 0;
    uint32_t nib = (k & 1u) ? (uint32_t)(byte >> 4) : (uint32_t)(byte & 0xFu);

    uint32_t step = s_adpcm_table[c->adpcm_index];
    uint32_t diff = step >> 3;
    if (nib & 1u) diff += step >> 2;
    if (nib & 2u) diff += step >> 1;
    if (nib & 4u) diff += step;

    if (nib & 8u) {
        c->adpcm_sample -= (int32_t)diff;
        if (c->adpcm_sample < -0x7FFF) c->adpcm_sample = -0x7FFF;
    } else {
        c->adpcm_sample += (int32_t)diff;
        if (c->adpcm_sample > 0x7FFF) c->adpcm_sample = 0x7FFF;
    }

    int idx = (int)c->adpcm_index + (int)s_adpcm_index_tbl[nib & 7u];
    if (idx < 0) idx = 0;
    else if (idx > 88) idx = 88;
    c->adpcm_index = (uint32_t)idx;
    c->adpcm_cursor = n + 1u;

    /* 21-B9yi(续90)：到达循环点时保存解码现场（参考核在同一位置保存
       ADPCMValLoop/ADPCMIndexLoop），回绕时用它恢复。 */
    {
        uint32_t loop_bytes = (uint32_t)(c->pnt & 0xFFFFu) << 2;
        uint32_t loop_samples = loop_bytes << 1;   /* ADPCM：2 个 nibble 采样/字节 */
        if (n == loop_samples) {
            c->adpcm_loop_sample = c->adpcm_sample;
            c->adpcm_loop_index = c->adpcm_index;
            c->adpcm_loop_valid = 1;
        }
    }
}

/* ---- PSG ---- */

/* 方波（通道 8-13）与噪声（14-15）都返回 ±满幅（10 位域 ±0x200）。 */
/* 21-B9yi(续88)：PSG 方波表 + 噪声 LFSR —— 直接抄参考核（melonDS SPU.cpp/h）。
   此前本地是「自定相位 + 自定伪随机」：duty=0 的首样本为正、占空比按 (duty+1)/8；
   参考核的表是「duty=0 首样本为负、每行有 N 个 +0x7FFF」。
   两者听起来都像方波，但**相位与占空比摆放不同** ⇒ 换成参考核口径。
   注意量纲：参考核是 16 位（±0x7FFF），本地混音管线是 10 位域（±0x200），故 >>6。 */
static const int16_t s_psg_table[8][8] = {
    { -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF,  0x7FFF },
    { -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF,  0x7FFF,  0x7FFF },
    { -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF },
    { -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF },
    { -0x7FFF, -0x7FFF, -0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF },
    { -0x7FFF, -0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF },
    { -0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF },
    { -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF }
};

static int32_t snd_psg_sample(snd_channel_t *c, uint32_t idx, int ch)
{
    if (ch >= 14) {
        /* 通道 14/15 的 PSG 格式 = 噪声（melonDS `DoRun`：`Num >= 14 → Run<4>`）。
           melonDS 的 LFSR：**先看当前最低位决定输出**，再移位/异或 0x6000；初值 0x7FFF。 */
        int32_t out;
        if (c->noise & 1u) {
            c->noise = (c->noise >> 1) ^ 0x6000u;
            out = -0x7FFF;
        } else {
            c->noise >>= 1;
            out = 0x7FFF;
        }
        return out >> 6;
    }
    int duty = (int)((c->cnt >> SNDCNT_DUTY_SHIFT) & 7);
    return (int32_t)s_psg_table[duty][idx & 7u] >> 6;
}

/* ---- 混音 ---- */

/* 归一化：各格式都折到 10 位有符号域（±0x200），满音量不削波。 */
static int32_t snd_channel_raw(snd_channel_t *c, const struct bus *bus, int ch)
{
    int fmt = (int)((c->cnt >> SNDCNT_FORMAT_SHIFT) & 3);
    uint32_t idx = c->pos >> 16;

    /* 21-B9yi(续90)：**循环点 PNT 语义**（按参考核 melonDS 口径）。
       参考核把 PNT/LEN 都换算成字节：LoopPos=(pnt&0xFFFF)<<2、Length=(len&0x1FFFFF)<<2，
       播放在 [0, LoopPos+Length) 内进行、**循环时跳回 LoopPos**（并恢复 ADPCM 现场）。
       本地此前「回绕到 0」——实测本作 20000 帧里 **999 次回绕全部带非零 PNT**，
       所以那是真缺陷（音频块循环会跳回样本开头而不是循环点）。 */
    if (fmt != SND_FORMAT_PSG) {
        uint32_t total_bytes = ((uint32_t)(c->pnt & 0xFFFFu) << 2) +
                               ((c->len & 0x1FFFFFu) << 2);
        uint64_t pos_samples = c->pos >> 16;
        uint64_t used_bytes = (fmt == SND_FORMAT_PCM16) ? pos_samples * 2u
                            : (fmt == SND_FORMAT_ADPCM) ? (pos_samples >> 1)
                            : pos_samples;
        if (total_bytes != 0 && used_bytes >= (uint64_t)total_bytes) {
            int repeat = (int)((c->cnt >> SNDCNT_REPEAT_SHIFT) & 3);
            if (repeat == SND_REPEAT_LOOP) {
                uint32_t loop_bytes = (uint32_t)(c->pnt & 0xFFFFu) << 2;
                uint64_t back = (fmt == SND_FORMAT_PCM16) ? ((uint64_t)loop_bytes >> 1)
                              : (fmt == SND_FORMAT_ADPCM) ? ((uint64_t)loop_bytes << 1)
                              : (uint64_t)loop_bytes;
                s_loop_events++;
                if (c->pnt != 0)
                    s_loop_events_pnt_nz++;
                c->pos = back << 16;
                idx = (uint32_t)(c->pos >> 16);
                if (fmt == SND_FORMAT_ADPCM) {
                    /* 恢复循环现场；游标指向「循环点样本已算好」的位置 */
                    c->adpcm_sample = c->adpcm_loop_sample;
                    c->adpcm_index = c->adpcm_loop_index;
                    c->adpcm_cursor = (uint32_t)back + 1u;
                }
            } else {
                if (repeat == SND_REPEAT_ONESHOT) {
                    snd_log("end", ch);
                    c->cnt &= ~SNDCNT_START;   /* 单发：停止（参考核同时把 CurSample 置 0） */
                    return 0;
                }
                /* 手动模式（repeat=0）：参考核越过末尾后**不再解码**、也不停，
                   而是保持最后的采样值（等软件清 start 位）。 */
                return c->last_raw;
            }
        }
    }

    int32_t raw;
    switch (fmt) {
    case SND_FORMAT_PCM8:
        raw = ((int32_t)(int8_t)(bus != NULL ? bus_read8(bus, c->sad + idx) : 0)) << 2;
        break;
    case SND_FORMAT_PCM16:
        raw = ((int32_t)(int16_t)(bus != NULL ? bus_read16(bus, c->sad + idx * 2u) : 0)) >> 6;
        break;
    case SND_FORMAT_ADPCM: {
        if (!c->adpcm_started)
            snd_adpcm_read_header(c, bus);
        while (c->adpcm_cursor <= idx)
            snd_adpcm_decode_one(c, bus);
        raw = c->adpcm_sample >> 6;
        break;
    }
    case SND_FORMAT_PSG:
    default:
        raw = snd_psg_sample(c, idx, ch);
        break;
    }
    c->last_raw = raw;   /* 21-B9yi(续90)：手动模式越过末尾时保持它 */
    return raw;
}

static int32_t snd_arith_div(int32_t v, int shift)
{
    /* 算术右移（向 -inf），等价整数实现避免负数的实现定义行为 */
    if (v >= 0)
        return v >> shift;
    return -((-v) >> shift);
}

/* 宿主侧渲染标志（audio.c 打开 SDL 设备时置 1）。 */
static int s_host_render_active;

void snd_set_host_render_active(int active)
{
    s_host_render_active = active;
}

int snd_host_render_active(void)
{
    return s_host_render_active;
}

/* 无头/无声卡时按经过的样本数推进混音器状态：结果丢弃，只保留通道游标与
   结束标志（游戏轮询 SOUNDxCNT bit31/bit15 时会用到）。 */
void snd_advance(snd_t *s, const struct bus *bus, uint32_t samples)
{
    int16_t scratch[SND_MIX_RATE / 8];      /* 4096 样本/批：一帧约 548 */
    const uint32_t cap = (uint32_t)(sizeof scratch / sizeof scratch[0]);
    while (samples > 0) {
        uint32_t n = (samples > cap) ? cap : samples;
        snd_render(s, bus, scratch, scratch, (int)n);
        samples -= n;
    }
}

void snd_render(snd_t *s, const struct bus *bus, int16_t *out_l, int16_t *out_r, int n)
{
    int master = (int)(s->soundcnt & 0x7F);
    int master_enable = (s->soundcnt & 0x8000u) != 0;
    int bias = (int)(s->soundbias & 0x3FFu);

    /* 21-B9yi(续60)：音频统计诊断（`NDS_SNDSTAT=1`）。
       背景：「窗口模式有声音」此前只验证到「SDL 设备能打开」，从没验证过
       **混音结果不是静音**。这里直接盯 `snd_render()` 的输出：每秒（32768 样本）
       打一行「非静音样本数 / 峰值」，无头模式也能跑（不依赖声卡）。 */
    static int stat_state;
    static unsigned long long stat_n, stat_nz, stat_total;
    static int stat_peak;
    if (stat_state == 0)
        stat_state = (getenv("NDS_SNDSTAT") != NULL) ? 1 : -1;

    for (int i = 0; i < n; i++) {
        int32_t mixL = 0, mixR = 0;
        if (master_enable) {
            for (int ch = 0; ch < SND_CHANNEL_COUNT; ch++) {
                snd_channel_t *c = &s->ch[ch];
                if (!(c->cnt & SNDCNT_START))
                    continue; /* 未启动（bit31=0） */
                int fmt = (int)((c->cnt >> SNDCNT_FORMAT_SHIFT) & 3);
                int32_t raw = snd_channel_raw(c, bus, ch);
                if (stat_state == 1) {
                    int f = fmt;
                    if (fmt == SND_FORMAT_PSG && ch >= 14)
                        f = 4;   /* 通道 14/15 的 PSG = 噪声 */
                    s_fmt_samples[f]++;
                }

                int mul = (int)(c->cnt & SNDCNT_VOL_MUL_MASK);
                int div = (int)((c->cnt >> SNDCNT_VOL_DIV_SHIFT) & 3);
                static const int div_shift[4] = { 0, 1, 2, 4 };
                int32_t v = snd_arith_div((int32_t)(((int64_t)raw * mul) / 127), div_shift[div]);

                int pan = (int)((c->cnt >> SNDCNT_PAN_SHIFT) & 0x7F);
                int32_t l = (int32_t)(((int64_t)v * (127 - pan)) / 127);
                int32_t r = (int32_t)(((int64_t)v * pan) / 127);
                mixL += l;
                mixR += r;

                /* 推进游标（定点 16.16）。tmr=0 视作 65536。 */
                uint32_t tmr = c->tmr ? c->tmr : 65536u;
                uint32_t inc = (uint32_t)(((uint64_t)SND_MASTER_CLOCK << 16) / tmr / SND_MIX_RATE);
                c->pos += inc;

                /* 21-B9yi(续90)：结束/循环判定已移到 `snd_channel_raw()` 顶部
                   （按参考核口径用 PNT+LEN 的字节数和循环点跳转）。 */
            }
        }
        mixL = (int32_t)(((int64_t)mixL * master) / 127);
        mixR = (int32_t)(((int64_t)mixR * master) / 127);

        int32_t uL = mixL + bias, uR = mixR + bias;
        if (uL < 0) uL = 0; else if (uL > 0x3FF) uL = 0x3FF;
        if (uR < 0) uR = 0; else if (uR > 0x3FF) uR = 0x3FF;
        out_l[i] = (int16_t)((uL - 0x200) << 6);
        out_r[i] = (int16_t)((uR - 0x200) << 6);

        /* 21-B9yi(续60)：统计（见函数开头说明）。以左右声道绝对值的较大者计。 */
        if (stat_state == 1) {
            int aL = out_l[i] < 0 ? -(int)out_l[i] : (int)out_l[i];
            int aR = out_r[i] < 0 ? -(int)out_r[i] : (int)out_r[i];
            int a = aL > aR ? aL : aR;
            if (a != 0)
                stat_nz++;
            if (a > stat_peak)
                stat_peak = a;
            if (++stat_n >= SND_MIX_RATE) {
                stat_total += stat_n;
                printf("sndstat: t=%llus nz=%llu/%llu peak=%d master=%d on=%d\n",
                       stat_total / SND_MIX_RATE, stat_nz, stat_n,
                       stat_peak, master, master_enable);
                fflush(stdout);
                stat_n = 0; stat_nz = 0; stat_peak = 0;
            }
        }
    }
}
