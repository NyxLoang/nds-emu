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

static const int16_t snd_adpcm_step[16] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31
};
static const int8_t snd_adpcm_index_tbl[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8
};

static void snd_adpcm_read_header(snd_channel_t *c, const struct bus *bus)
{
    uint32_t hdr = bus != NULL ? bus_read32(bus, c->sad) : 0;
    int32_t init = (int32_t)((hdr & 0x7F) << 9); /* 7 位有符号，升到 16 位 */
    if (init & 0x8000) init -= 0x10000;          /* 符号扩展（bit6 为符号位） */
    c->adpcm_sample = init;
    c->adpcm_index = (hdr >> 8) & 0xFu;
    c->adpcm_remaining = 0;
    c->adpcm_cursor = 0;
    c->adpcm_started = 1;
}

/* 解码一个 nibble，把 adpcm_sample 推进到源采样 adpcm_cursor。 */
static void snd_adpcm_decode_one(snd_channel_t *c, const struct bus *bus)
{
    if (c->adpcm_remaining == 0) {
        /* 第 1 个数据字在头字之后（+4），之后每字 +4 */
        uint32_t data_addr = c->sad + 4u + (c->adpcm_cursor / 8u) * 4u;
        c->adpcm_word = bus != NULL ? bus_read32(bus, data_addr) : 0;
        c->adpcm_remaining = 8;
    }
    int nibble = (int)((c->adpcm_word >> ((8 - c->adpcm_remaining) * 4)) & 0xFu);
    c->adpcm_remaining--;

    int d = nibble & 7;
    int sign = (nibble & 8) ? -1 : 1;
    int step = snd_adpcm_step[c->adpcm_index];
    int diff = step >> 3;
    if (d & 1) diff += step >> 2;
    if (d & 2) diff += step >> 1;
    if (d & 4) diff += step;
    c->adpcm_sample += sign * diff;
    if (c->adpcm_sample > 32767) c->adpcm_sample = 32767;
    if (c->adpcm_sample < -32768) c->adpcm_sample = -32768;
    int idx = (int)c->adpcm_index + snd_adpcm_index_tbl[nibble];
    if (idx < 0) idx = 0;
    if (idx > 15) idx = 15;
    c->adpcm_index = (uint32_t)idx;
    c->adpcm_cursor++;
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
    switch (fmt) {
    case SND_FORMAT_PCM8:
        return ((int32_t)(int8_t)(bus != NULL ? bus_read8(bus, c->sad + idx) : 0)) << 2;
    case SND_FORMAT_PCM16:
        return ((int32_t)(int16_t)(bus != NULL ? bus_read16(bus, c->sad + idx * 2u) : 0)) >> 6;
    case SND_FORMAT_ADPCM: {
        if (!c->adpcm_started)
            snd_adpcm_read_header(c, bus);
        while (c->adpcm_cursor <= idx)
            snd_adpcm_decode_one(c, bus);
        return c->adpcm_sample >> 6;
    }
    case SND_FORMAT_PSG:
    default:
        return snd_psg_sample(c, idx, ch);
    }
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

                /* 单发/手动：到达总长后停止；循环：回绕到 0（简化，见文档） */
                if (fmt != SND_FORMAT_PSG) {
                    uint32_t total;
                    if (fmt == SND_FORMAT_PCM8)       total = c->len * 4u;
                    else if (fmt == SND_FORMAT_PCM16) total = c->len * 2u;
                    else                              total = (c->len > 1) ? (c->len - 1u) * 8u : 0u;
                    int repeat = (int)((c->cnt >> SNDCNT_REPEAT_SHIFT) & 3);
                    if ((uint64_t)c->pos >= ((uint64_t)total << 16)) {
                        if (repeat == SND_REPEAT_LOOP) {
                            c->pos = 0;
                            snd_reset_channel(c); /* 简化：回绕从头重放 */
                            c->cnt |= SNDCNT_START;
                        } else {
                            snd_log("end", ch);
                            c->cnt &= ~SNDCNT_START; /* 单发/手动：停止 */
                        }
                    }
                }
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
