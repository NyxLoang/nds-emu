#include "touch.h"

/* 触摸屏是否被选中：SPI 使能 且 设备选择 = 触摸(2)。 */
static int touch_active(const touch_t *t)
{
    return (t->spicnt & SPICNT_ENABLE) &&
           ((t->spicnt & SPICNT_DEVICE_MASK) == SPICNT_DEVICE_TOUCH);
}

/* 按通道算出 12 位 ADC 结果。
   X/Y 未按下时按协议回 X=000h、Y=FFFh；电池在 NDS 接 GND 恒 0。 */
static uint16_t touch_convert(const touch_t *t, int channel)
{
    switch (channel) {
    case TSC_CH_X:   return t->down ? t->adc_x : 0x000u;
    case TSC_CH_Y:   return t->down ? t->adc_y : 0xFFFu;
    case TSC_CH_Z1:  return t->down ? 0x300u : 0xFFFu;
    case TSC_CH_Z2:  return t->down ? 0xB00u : 0x000u;
    case TSC_CH_BAT: return 0x000u;               /* 接 GND */
    case TSC_CH_AUX: return 0x000u;               /* 麦克风关闭 */
    case TSC_CH_TEMP0:
    case TSC_CH_TEMP1:
    default:         return 0x000u;               /* 温度：本阶段返回 0 */
    }
}

/* 回传下一个回复字节。
   12 位：首字节 = 哑元位(bit7=0) + 结果 bit11..5；次字节 = 结果 bit4..0 + 3 位填充。
   8 位：取结果高 8 位（12 位右移 4）；首字节 = 哑元位 + 结果 bit7..1；
        次字节 = 结果 bit0 + 7 位填充。 */
static uint8_t touch_reply(const touch_t *t)
{
    uint8_t r8;
    if (t->reply_idx == 0) {
        if (t->mode8) {
            r8 = (uint8_t)((t->result >> 4) & 0xFF);
            return (uint8_t)(r8 >> 1);
        }
        return (uint8_t)((t->result >> 5) & 0x7F);
    }
    if (t->reply_idx == 1) {
        if (t->mode8) {
            r8 = (uint8_t)((t->result >> 4) & 0xFF);
            return (uint8_t)((r8 & 1) << 7);
        }
        return (uint8_t)((t->result & 0x1F) << 3);
    }
    return 0; /* 无限填充 */
}

/* 写 SPIDATA 触发一次传输：写出的 out 若带起始位(bit7)则为新命令，
   解码通道/分辨率并算出结果；随后回传下一个回复字节存进 SPIDATA。 */
static void touch_transfer(touch_t *t, uint8_t out)
{
    if (!touch_active(t))
    {
        /* 21-B9j：设备 1（固件 Flash）最小 HLE——FFXII 启动会经 ARM7 FIFO
           service6 读固件；本模拟器暂无固件镜像，按“空数据 0xFF”回读，
           让命令序列能继续走完。 */
        if ((t->spicnt & SPICNT_DEVICE_MASK) == SPICNT_DEVICE_FW)
            t->spidata = 0xFFu;
        return;
    }
    if (out & 0x80) {
        t->cmd = out;
        t->channel = (out >> 4) & 7;
        t->mode8 = (out >> 3) & 1;
        t->result = touch_convert(t, t->channel);
        t->reply_idx = 0;
    }
    t->spidata = touch_reply(t);
    t->reply_idx++;
    /* 未保持片选：传输后自动撤片选，下一次需重新发命令。 */
    if (!(t->spicnt & SPICNT_HOLD))
        t->reply_idx = 2;
}

int touch_is_addr(uint32_t addr)
{
    return addr >= IO_SPICNT && addr < IO_SPI_END;
}

uint8_t touch_read8(touch_t *t, uint32_t addr)
{
    if (addr == IO_SPIDATA)
        return t->spidata;
    if (addr == IO_SPICNT)
        return (uint8_t)(t->spicnt & 0x7F);      /* 低字节，busy(bit7) 恒 0 */
    if (addr == IO_SPICNT + 1)
        return (uint8_t)(t->spicnt >> 8);        /* 高字节 */
    return 0;
}

void touch_write8(touch_t *t, uint32_t addr, uint8_t val)
{
    if (addr == IO_SPIDATA) {
        touch_transfer(t, val);
        return;
    }
    if (addr == IO_SPICNT) {
        t->spicnt = (uint16_t)((t->spicnt & 0xFF00u) | val);
    } else if (addr == IO_SPICNT + 1) {
        t->spicnt = (uint16_t)((t->spicnt & 0x00FFu) | ((uint16_t)val << 8));
    }
    /* 设备被撤下（禁用或改选其它设备）时复位命令状态。 */
    if (!touch_active(t))
        t->reply_idx = 2;
}

void touch_set_pos(touch_t *t, uint16_t adc_x, uint16_t adc_y, int down)
{
    t->adc_x = adc_x;
    t->adc_y = adc_y;
    t->down = down;
}
