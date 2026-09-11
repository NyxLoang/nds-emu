#ifndef NDS_EMU_IO_RTC_H
#define NDS_EMU_IO_RTC_H

#include <stdint.h>

/* 21-B9wx：NDS 实时时钟（ARM7 专属，0x04000134 RCnt / 0x04000138 RTC 数据）。

   真机是一颗 3 线串行 RTC 芯片（Fujitsu/Rohm 风格）：
   0x04000138 的 bit0=数据、bit1=时钟、bit2=片选、bit4=方向（1=主机写、0=主机读）。
   片选拉高开始一次传输；每来一个“时钟下降沿”，写方向锁存一位数据、
   读方向把一位输出放到 bit0。第一个字节是命令（高半字节=寄存器号，
   低半字节 6=读写时间/状态、E=DSi 扩展），随后是要读/写的字节。

   FFXII 开局后 ARM7 会大量轮询这两个寄存器（实测 2300 帧内 11 万次读）；
   本地此前把它们当未映射读 0，游戏的 RTC 事务永远得不到数据。 */

typedef struct rtc_state {
    uint8_t status1;         /* 命令 0x06：状态寄存器 1（bit1=24 小时制） */
    uint8_t status2;         /* 命令 0x46：状态寄存器 2（中断控制） */
    uint8_t datetime[7];     /* 命令 0x26：年/月/日/星期/时/分/秒（BCD） */
    uint8_t alarm1[3];       /* 命令 0x16：闹钟 1 */
    uint8_t alarm2[3];       /* 命令 0x56：闹钟 2 */
    uint8_t clock_adjust;    /* 命令 0x36：时钟微调 */
    uint8_t free_reg;        /* 命令 0x76：自由寄存器 */
} rtc_state_t;

typedef struct rtc {
    uint16_t io;             /* 0x04000138 的可见值（bit0=输出数据） */
    uint8_t  input;          /* 正在接收的一个字节 */
    uint32_t input_bit;
    uint32_t input_pos;
    uint8_t  output[8];      /* 待发送的字节 */
    uint32_t output_bit;
    uint32_t output_pos;
    uint8_t  cur_cmd;
    rtc_state_t st;
} rtc_t;

void rtc_reset(rtc_t *r);
uint16_t rtc_read16(const rtc_t *r);
void rtc_write16(rtc_t *r, uint16_t val, int byte_write);

/* 走时：按“经过的秒数”推进 RTC 时钟（BCD 进位到日；月份长度按简化的 31 天）。 */
void rtc_advance_seconds(rtc_t *r, uint32_t seconds);

#endif /* NDS_EMU_IO_RTC_H */
