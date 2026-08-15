#ifndef NDS_EMU_SND_H
#define NDS_EMU_SND_H

#include <stdint.h>

/* 阶段 18：NDS 音频（16 通道 + 混音）。
   16 个音频通道各占 16 字节（基址 0x04000400），支持 PCM8/PCM16/IMA-ADPCM/PSG。
   主控 SOUNDCNT(0x04000500)/SOUNDBIAS(0x04000504)。混音器输出 32768Hz 立体声。
   核心不依赖 SDL，`snd_render` 合成样本供 SDL 回调（或测试直接断言）。 */

#define SND_CHANNEL_COUNT 16
#define SND_BASE       0x04000400u
#define SND_CH_STRIDE  0x10u
#define SND_SOUNDCNT   0x04000500u
#define SND_SOUNDBIAS  0x04000504u
#define SND_END        0x04000506u   /* 主控区上界（不含）；capture 0x508 起本阶段不实现 */

/* SOUNDxCNT 位定义 */
#define SNDCNT_VOL_MUL_MASK  0x7Fu
#define SNDCNT_VOL_DIV_SHIFT 8
#define SNDCNT_VOL_DIV_MASK  (3u << SNDCNT_VOL_DIV_SHIFT)
#define SNDCNT_HOLD          (1u << 15)
#define SNDCNT_PAN_SHIFT     16
#define SNDCNT_PAN_MASK      (0x7Fu << SNDCNT_PAN_SHIFT)
#define SNDCNT_DUTY_SHIFT    24
#define SNDCNT_DUTY_MASK     (7u << SNDCNT_DUTY_SHIFT)
#define SNDCNT_REPEAT_SHIFT  27
#define SNDCNT_REPEAT_MASK   (3u << SNDCNT_REPEAT_SHIFT)
#define SNDCNT_FORMAT_SHIFT  29
#define SNDCNT_FORMAT_MASK   (3u << SNDCNT_FORMAT_SHIFT)
#define SNDCNT_START         (1u << 31)

/* 格式（bit29-30） */
#define SND_FORMAT_PCM8   0
#define SND_FORMAT_PCM16  1
#define SND_FORMAT_ADPCM  2
#define SND_FORMAT_PSG    3

/* 循环模式（bit27-28） */
#define SND_REPEAT_MANUAL   0
#define SND_REPEAT_LOOP     1
#define SND_REPEAT_ONESHOT  2

#define SND_MIX_RATE      32768u      /* 混音输出采样率（Hz） */
#define SND_MASTER_CLOCK  33513982u   /* 通道采样时钟（Hz） */

/* 前向声明：snd 混音时按源地址从总线读波形数据（PCM/ADPCM 在内存里）。 */
struct bus;

/* 通道状态：寄存器 + 运行时状态（播放游标 / ADPCM 解码器 / PSG 相位）。 */
typedef struct snd_channel {
    uint32_t cnt;          /* SOUNDxCNT */
    uint32_t sad;          /* SOUNDxSAD */
    uint16_t tmr;          /* SOUNDxTMR */
    uint16_t pnt;          /* SOUNDxPNT */
    uint32_t len;          /* SOUNDxLEN */
    /* 运行时 */
    uint32_t pos;          /* 源采样游标（16.16 定点，整数 = 源采样序号） */
    /* ADPCM 解码器状态 */
    int32_t  adpcm_sample;    /* 当前解码采样 */
    uint32_t adpcm_index;     /* 步进索引 0..15 */
    uint32_t adpcm_remaining; /* 当前数据字里剩余 nibble 数 */
    uint32_t adpcm_word;      /* 当前数据字 */
    uint32_t adpcm_cursor;    /* 已解码到的源采样序号（追赶游标） */
    int      adpcm_started;   /* 是否已读头并开始解码 */
} snd_channel_t;

typedef struct snd {
    snd_channel_t ch[SND_CHANNEL_COUNT];
    uint16_t soundcnt;    /* SOUNDCNT */
    uint16_t soundbias;   /* SOUNDBIAS（bit0-9） */
} snd_t;

int snd_is_addr(uint32_t addr);
uint8_t snd_read8(const snd_t *s, uint32_t addr);
void snd_write8(snd_t *s, uint32_t addr, uint8_t val);

/* 合成 n 个立体声采样（32768Hz）到 out_l/out_r（各 n 个 int16）。
   bus 用于按 SAD 读波形数据；可为 NULL（此时 PCM/ADPCM 读 0）。 */
void snd_render(snd_t *s, const struct bus *bus, int16_t *out_l, int16_t *out_r, int n);

#endif /* NDS_EMU_SND_H */
