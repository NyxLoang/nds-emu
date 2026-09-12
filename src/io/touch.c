#include "touch.h"
#include <stdio.h>

/* 触摸屏是否被选中：SPI 使能 且 设备选择 = 触摸(2)。 */
static int touch_active(const touch_t *t)
{
    return (t->spicnt & SPICNT_ENABLE) &&
           ((t->spicnt & SPICNT_DEVICE_MASK) == SPICNT_DEVICE_TOUCH);
}

/* 按通道算出 12 位 ADC 结果。

   21-B9yi(续73)：**按参考核口径对齐**。melonDS 的 `TSC::Write()` 只处理三种通道：
     X(0x50) → TouchX、Y(0x10) → TouchY、AUX(0x60) → 麦克风采样；
     其余通道（含 Z1/Z2 压力、BAT、TEMP0/TEMP1）**一律 0xFFF**。
   本地此前自己编了一套 Z1=0x300/Z2=0xB00/BAT=0/TEMP=0 —— 与参考核不一致。
   实测里游戏 ARM7 的触摸驱动轮询的正是**通道 0（TEMP0，命令字节 0x84）**，
   因此这条差异很可能影响游戏是否认可“这是有效触摸”（战斗相位比早期画面更严格）。
   X/Y 与“未按下”口径保持不变（未按下 X=000h、Y=FFFh，与参考核一致）。 */
static uint16_t touch_convert(const touch_t *t, int channel)
{
    switch (channel) {
    case TSC_CH_X:   return t->down ? t->adc_x : 0x000u;
    case TSC_CH_Y:   return t->down ? t->adc_y : 0xFFFu;
    case TSC_CH_AUX: return 0x800u;   /* 麦克风静音：参考核 sample^0x8000 后 >>4 = 0x800 */
    default:         return 0xFFFu;   /* 其余通道（Z1/Z2/BAT/TEMP）：与参考核一致 */
    }
}

/* 回传下一个回复字节。
   12 位：首字节 = 哑元位(bit7=0) + 结果 bit11..5；次字节 = 结果 bit4..0 + 3 位填充。
   8 位：取结果高 8 位（12 位右移 4）；首字节 = 哑元位 + 结果 bit7..1；
        次字节 = 结果 bit0 + 7 位填充。 */
static uint8_t touch_reply(const touch_t *t)
{
    /* 21-B9yi(续73)：8 位模式按参考核口径 —— 把结果掩到 0xFF0（`ConvResult &= 0x0FF0`），
       字节拆分仍走 12 位那套；此前本地另写了一套 8 位拆分，与参考核不一致。 */
    uint16_t result = (t->mode8) ? (uint16_t)(t->result & 0x0FF0u) : t->result;
    if (t->reply_idx == 0) {
        return (uint8_t)((result >> 5) & 0x7F);
    }
    if (t->reply_idx == 1) {
        return (uint8_t)((result & 0x1F) << 3);
    }
    return 0; /* 无限填充 */
}

/* ---------- device1：固件 Flash（21-B9j+） ----------
   FFXII 的 ARM7 service6 会读：固件头 0x20 的用户设置偏移、0x3FE00/0x3FF00
   两个 256 字节用户设置镜像，以及 Wi-Fi 信息头里的机型/MAC/长度/频道等。
   模拟器没有 firmware.bin，这里按真实 NDS 出厂默认的最小集合回读：
   关键头字节 + CRC 合法的用户设置镜像，其余按擦除态 0xFF。 */

/* CRC-16/IBM（0xA001 反射实现），与 SWI 0x0E、真固件用户区校验同算法。 */
static uint16_t fw_crc16(const uint8_t *p, int len)
{
    uint16_t crc = 0xFFFFu;
    for (int i = 0; i < len; i++) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 1u)
                crc = (uint16_t)((crc >> 1) ^ 0xA001u);
            else
                crc = (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

/* 用户设置镜像前 0x70 字节的默认内容（两个镜像共用；Update Counter 单独处理）。
   布局参考 DS 固件文档/开源模拟器默认设置：version=5、语言标志、触摸校准、
   生日/昵称等字段。 */
static uint8_t fw_user_body(uint32_t off)
{
    switch (off) {
    case 0x00: return 0x05;                /* version（小端） */
    case 0x02: return 0x07;                /* favoriteColor */
    case 0x03: return 0x06;                /* 生日月 */
    case 0x04: return 0x17;                /* 生日日 */
    case 0x58: return 0x00;                /* 触摸校准：ADC x1=0x200 */
    case 0x59: return 0x02;
    case 0x5A: return 0x00;                /* ADC y1=0x200 */
    case 0x5B: return 0x02;
    case 0x5C: return 0x21;                /* 像素 x1=0x21 */
    case 0x5D: return 0x21;                /* 像素 y1=0x21 */
    case 0x5E: return 0x00;                /* ADC x2=0xE00 */
    case 0x5F: return 0x0E;
    case 0x60: return 0x00;                /* ADC y2=0x800 */
    case 0x61: return 0x08;
    case 0x62: return 0xE1;                /* 像素 x2=0xE1 */
    case 0x63: return 0x81;                /* 像素 y2=0x81 */
    case 0x64: return 0x31;                /* 语言标志（小端） */
    case 0x65: return 0xFC;
    default:   return 0x00;
    }
}

/* 用户设置镜像区（0x3FE00 / 0x3FF00）的字节：0x70 处是 Update Counter，
   0x72 处是前 0x70 字节的 CRC16（校验通过才算有效设置）。 */
static uint8_t fw_user_byte(uint32_t addr)
{
    uint32_t mirror = (addr >> 8) & 1u;
    uint32_t off = addr & 0xFFu;

    if (off < 0x70u)
        return fw_user_body(off);
    if (off == 0x70u)
        return (uint8_t)mirror;
    if (off == 0x71u)
        return 0x00;
    if (off == 0x72u || off == 0x73u) {
        uint8_t buf[0x70];
        for (uint32_t i = 0; i < 0x70u; i++)
            buf[i] = fw_user_body(i);
        uint16_t crc = fw_crc16(buf, 0x70);
        return (uint8_t)(crc >> ((off == 0x72u) ? 0 : 8));
    }
    return 0xFF; /* 0x74..0xFF：DS 出厂用户区填充 0xFF */
}

/* 固件按物理地址回读一个字节。地址 0x00..0x3F 是固件头+Wi-Fi 信息头：
   0x1D=主机型号(DS=0xFF)、0x20/21=用户设置偏移(0x7FC0)、0x2C/2D=Wi-Fi 信息
   长度(0x138)、0x36..3B=MAC、0x3C/3D=频道；其余区域按擦除态 0xFF。 */
static uint8_t fw_read8(uint32_t addr)
{
    static const uint8_t mac[6] = { 0x00, 0x09, 0xBF, 0x12, 0x34, 0x56 };

    if (addr >= 0x3FE00u && addr < 0x40000u)
        return fw_user_byte(addr);
    if (addr < 0x40u) {
        if (addr == 0x1Du || addr == 0x1Eu || addr == 0x1Fu)
            return 0xFFu;
        if (addr == 0x20u) return 0xC0u;
        if (addr == 0x21u) return 0x7Fu;
        if (addr == 0x2Cu) return 0x38u;
        if (addr == 0x2Du) return 0x01u;
        if (addr >= 0x36u && addr < 0x3Cu)
            return mac[addr - 0x36u];
        if (addr == 0x3Cu) return 0xFEu;
        if (addr == 0x3Du) return 0x3Fu;
    }
    return 0xFFu;
}

/* device1 一次 8 位 SPI 传输：MISO 回读一字节并推进固件状态机。
   真机每次写 SPIDATA 都开始一次传输；HOLD=1 片选保持（多字节命令续接），
   HOLD=0 本字节传完即撤片选（事务结束）。READ=0x03 后跟 24 位大端地址，
   地址收满后每个后续字节都回读并自增地址。 */
static uint8_t fw_transfer(touch_t *t, uint8_t out)
{
    uint8_t in = 0xFFu;

    /* 上一事务已撤片选：新事务从命令字节开始。 */
    if (!t->fw_cs) {
        t->fw_cmd = 0;
        t->fw_addr = 0;
        t->fw_addr_n = 0;
    }

    if (t->fw_cmd == 0x05u) {
        in = 0x00u;                    /* RDSR：未忙 */
    } else if (t->fw_cmd == 0x03u && t->fw_addr_n >= 3) {
        in = fw_read8(t->fw_addr);     /* READ 数据阶段 */
        t->fw_addr++;
    } else if (t->fw_cmd == 0x03u) {
        t->fw_addr = (t->fw_addr << 8) | out;  /* 3 字节大端地址 */
        t->fw_addr_n++;
    } else if (out == 0x03u) {
        t->fw_cmd = 0x03u;             /* READ 命令 */
        t->fw_addr = 0;
        t->fw_addr_n = 0;
    } else if (out == 0x05u) {
        t->fw_cmd = 0x05u;             /* RDSR 命令 */
    } else {
        t->fw_cmd = 0;                 /* 其它命令暂不建模 */
    }
    return in;
}

/* 写 SPIDATA 触发一次传输：写出的 out 若带起始位(bit7)则为新命令，
   解码通道/分辨率并算出结果；随后回传下一个回复字节存进 SPIDATA。 */
static void touch_transfer(touch_t *t, uint8_t out)
{
    if (!touch_active(t))
    {
        if ((t->spicnt & SPICNT_ENABLE) &&
            ((t->spicnt & SPICNT_DEVICE_MASK) == SPICNT_DEVICE_FW)) {
            t->spidata = fw_transfer(t, out);
            /* 本字节传完后按 HOLD 决定片选是否继续拉低。 */
            t->fw_cs = (t->spicnt & SPICNT_HOLD) ? 1 : 0;
        }
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
    if (!touch_active(t)) {
        t->reply_idx = 2;
        /* 离开固件 Flash 或关掉 SPI：当前固件事务作废。 */
        if (!(t->spicnt & SPICNT_ENABLE) ||
            ((t->spicnt & SPICNT_DEVICE_MASK) != SPICNT_DEVICE_FW))
            t->fw_cs = 0;
    }
}

void touch_set_pos(touch_t *t, uint16_t adc_x, uint16_t adc_y, int down)
{
    t->adc_x = adc_x;
    t->adc_y = adc_y;
    t->down = down;
}
