#include "snd.h"
#include "bus/bus.h"

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

uint8_t snd_read8(const snd_t *s, uint32_t addr)
{
    if (addr == SND_SOUNDCNT)     return (uint8_t)(s->soundcnt & 0xFF);
    if (addr == SND_SOUNDCNT + 1) return (uint8_t)(s->soundcnt >> 8);
    if (addr == SND_SOUNDBIAS)    return (uint8_t)(s->soundbias & 0xFF);
    if (addr == SND_SOUNDBIAS + 1) return (uint8_t)(s->soundbias >> 8);

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
        s->soundbias = (uint16_t)((s->soundbias & 0xFF00u) | (val & 0x03u));
        return;
    }
    if (addr == SND_SOUNDBIAS + 1) {
        s->soundbias = (uint16_t)((s->soundbias & 0x0003u) | ((uint16_t)(val & 0x03u) << 8));
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
            snd_reset_channel(c);
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
static int32_t snd_psg_sample(const snd_channel_t *c, uint32_t idx, int ch)
{
    if (ch >= 14) {
        /* 白噪声：确定性伪随机位（近似 LFSR），随源采样变化 */
        uint32_t h = idx * 0x9E3779B9u;
        return ((h >> 16) & 1) ? 0x200 : -0x200;
    }
    /* 方波：8 步一周期，占空比 (duty+1)/8 */
    int duty = (int)((c->cnt >> SNDCNT_DUTY_SHIFT) & 7);
    int step = (int)(idx & 7);
    return (step < duty + 1) ? 0x200 : -0x200;
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

void snd_render(snd_t *s, const struct bus *bus, int16_t *out_l, int16_t *out_r, int n)
{
    int master = (int)(s->soundcnt & 0x7F);
    int master_enable = (s->soundcnt & 0x8000u) != 0;
    int bias = (int)(s->soundbias & 0x3FFu);

    for (int i = 0; i < n; i++) {
        int32_t mixL = 0, mixR = 0;
        if (master_enable) {
            for (int ch = 0; ch < SND_CHANNEL_COUNT; ch++) {
                snd_channel_t *c = &s->ch[ch];
                if (!(c->cnt & SNDCNT_START))
                    continue; /* 未启动（bit31=0） */
                int fmt = (int)((c->cnt >> SNDCNT_FORMAT_SHIFT) & 3);
                int32_t raw = snd_channel_raw(c, bus, ch);

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
    }
}
