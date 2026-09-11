#include <string.h>
#include "rtc.h"

/* ---- BCD 工具 ---- */
static uint8_t bcd(uint8_t v)
{
    return (uint8_t)(((v / 10u) << 4) | (v % 10u));
}

static uint8_t from_bcd(uint8_t v)
{
    return (uint8_t)((v >> 4) * 10u + (v & 0x0Fu));
}

static uint8_t bcd_inc(uint8_t v)
{
    uint8_t lo = (uint8_t)(v & 0x0Fu), hi = (uint8_t)(v >> 4);
    if (++lo >= 10u) { lo = 0; hi++; }
    return (uint8_t)((hi << 4) | lo);
}

static uint8_t bcd_clamp(uint8_t v, uint8_t lo, uint8_t hi)
{
    uint8_t d = from_bcd(v);
    if (d < lo) d = lo;
    if (d > hi) d = hi;
    return bcd(d);
}

void rtc_reset(rtc_t *r)
{
    memset(r, 0, sizeof(*r));
    /* 固定一个合法的初始时刻：2026-09-12（星期六）12:00:00，24 小时制。 */
    r->st.status1 = 0x02;
    r->st.datetime[0] = bcd(26);
    r->st.datetime[1] = bcd(9);
    r->st.datetime[2] = bcd(12);
    r->st.datetime[3] = 6;
    r->st.datetime[4] = bcd(12);
    r->st.datetime[5] = 0;
    r->st.datetime[6] = 0;
}

static void rtc_count_second(rtc_t *r)
{
    r->st.datetime[6] = bcd_inc(r->st.datetime[6]);
    if (r->st.datetime[6] < 0x60)
        return;
    r->st.datetime[6] = 0;
    r->st.datetime[5] = bcd_inc(r->st.datetime[5]);
    if (r->st.datetime[5] < 0x60)
        return;
    r->st.datetime[5] = 0;
    /* 小时：24 小时制按 23→0 进位；12 小时制按 11→12 翻 AM/PM */
    uint8_t hour = (uint8_t)(r->st.datetime[4] & 0x3Fu);
    uint8_t pm = (uint8_t)(r->st.datetime[4] & 0x40u);
    hour = bcd_inc(hour);
    if (r->st.status1 & 0x02u) {
        if (hour >= 0x24u) { hour = 0; r->st.datetime[3] = (uint8_t)((r->st.datetime[3] + 1u) % 7u);
                             r->st.datetime[2] = bcd_inc(r->st.datetime[2]); }
    } else {
        if (hour >= 0x12u) { hour = 0; pm ^= 0x40u; }
    }
    r->st.datetime[4] = (uint8_t)(hour | pm);
}

void rtc_advance_seconds(rtc_t *r, uint32_t seconds)
{
    while (seconds--)
        rtc_count_second(r);
}

/* ---- 命令：读（CurCmd bit7=1）---- */
static void rtc_cmd_read(rtc_t *r)
{
    if ((r->cur_cmd & 0x0F) != 0x06)
        return;
    switch (r->cur_cmd & 0x70) {
    case 0x00:
        r->output[0] = r->st.status1;
        r->st.status1 &= 0x0F;      /* 自动清零 bit4-7 */
        break;
    case 0x40: r->output[0] = r->st.status2; break;
    case 0x20: memcpy(r->output, r->st.datetime, 7); break;
    case 0x60: memcpy(r->output, &r->st.datetime[4], 3); break;
    case 0x10:
        if (r->st.status2 & 0x04) memcpy(r->output, r->st.alarm1, 3);
        else                      r->output[0] = r->st.alarm1[2];
        break;
    case 0x50: memcpy(r->output, r->st.alarm2, 3); break;
    case 0x30: r->output[0] = r->st.clock_adjust; break;
    case 0x70: r->output[0] = r->st.free_reg; break;
    default: break;
    }
}

/* ---- 命令：写 ---- */
static void rtc_write_datetime(rtc_t *r, uint32_t num, uint8_t val)
{
    switch (num) {
    case 1: r->st.datetime[0] = bcd_clamp(val, 0, 99); break;   /* 年 */
    case 2: r->st.datetime[1] = bcd_clamp((uint8_t)(val & 0x1Fu), 1, 12); break;
    case 3: r->st.datetime[2] = bcd_clamp((uint8_t)(val & 0x3Fu), 1, 31); break;
    case 4: r->st.datetime[3] = (uint8_t)(from_bcd((uint8_t)(val & 0x07u)) % 7u); break;
    case 5: {
        uint8_t hour = (uint8_t)(val & 0x3Fu);
        uint8_t pm = (uint8_t)(val & 0x40u);
        if (r->st.status1 & 0x02u) {
            hour = bcd_clamp(hour, 0, 23);
            pm = (hour >= 0x12u) ? 0x40u : 0u;
        } else {
            hour = bcd_clamp(hour, 0, 11);
        }
        r->st.datetime[4] = (uint8_t)(hour | pm);
        break;
    }
    case 6: r->st.datetime[5] = bcd_clamp((uint8_t)(val & 0x7Fu), 0, 59); break;
    case 7: r->st.datetime[6] = bcd_clamp((uint8_t)(val & 0x7Fu), 0, 59); break;
    default: break;
    }
}

static void rtc_cmd_write(rtc_t *r, uint8_t val)
{
    if ((r->cur_cmd & 0x0F) != 0x06)
        return;
    switch (r->cur_cmd & 0x70) {
    case 0x00:
        if (r->input_pos == 1) {
            uint8_t old = r->st.status1;
            r->st.status1 = val;
            if ((old ^ val) & 0x02u) {
                /* 12/24 小时制切换：换算当前小时 */
                uint8_t hour = (uint8_t)(r->st.datetime[4] & 0x3Fu);
                uint8_t pm = (uint8_t)(r->st.datetime[4] & 0x40u);
                if (r->st.status1 & 0x02u) {
                    if (pm) {
                        hour = (uint8_t)(hour + 0x12u);
                        if ((hour & 0x0Fu) >= 0x0Au) hour = (uint8_t)(hour + 0x06u);
                    }
                    hour = bcd_clamp(hour, 0, 23);
                } else {
                    if (hour >= 0x12u) {
                        pm = 0x40u;
                        hour = (uint8_t)(hour - 0x12u);
                        if ((hour & 0x0Fu) >= 0x0Au) hour = (uint8_t)(hour - 0x06u);
                    } else {
                        pm = 0;
                    }
                    hour = bcd_clamp(hour, 0, 11);
                }
                r->st.datetime[4] = (uint8_t)(hour | pm);
            }
        }
        break;
    case 0x40:
        if (r->input_pos == 1) r->st.status2 = val;
        break;
    case 0x20:
        if (r->input_pos <= 7) rtc_write_datetime(r, r->input_pos, val);
        break;
    case 0x60:
        if (r->input_pos <= 3) rtc_write_datetime(r, r->input_pos + 4u, val);
        break;
    case 0x10:
        if (r->st.status2 & 0x04) {
            if (r->input_pos >= 1 && r->input_pos <= 3) r->st.alarm1[r->input_pos - 1] = val;
        } else if (r->input_pos == 1) {
            r->st.alarm1[2] = val;
        }
        break;
    case 0x50:
        if (r->input_pos >= 1 && r->input_pos <= 3) r->st.alarm2[r->input_pos - 1] = val;
        break;
    case 0x30:
        if (r->input_pos == 1) r->st.clock_adjust = val;
        break;
    case 0x70:
        if (r->input_pos == 1) r->st.free_reg = val;
        break;
    default: break;
    }
}

/* 收到一个完整字节：第一个字节是命令，其余按命令分发 */
static void rtc_byte_in(rtc_t *r, uint8_t val)
{
    if (r->input_pos == 0) {
        if ((val & 0xF0u) == 0x60u) {
            /* 真机命令的高半字节按位反转 */
            static const uint8_t rev[16] = {
                0x06, 0x86, 0x46, 0xC6, 0x26, 0xA6, 0x66, 0xE6,
                0x16, 0x96, 0x56, 0xD6, 0x36, 0xB6, 0x76, 0xF6
            };
            r->cur_cmd = rev[val & 0x0Fu];
        } else {
            r->cur_cmd = val;
        }
        if (r->cur_cmd & 0x80u)
            rtc_cmd_read(r);
        return;
    }
    rtc_cmd_write(r, val);
}

uint16_t rtc_read16(const rtc_t *r)
{
    return r->io;
}

void rtc_write16(rtc_t *r, uint16_t val, int byte_write)
{
    if (byte_write)
        val = (uint16_t)((r->io & 0xFF00u) | (val & 0x00FFu));

    if (val & 0x0004u) {
        if (!(r->io & 0x0004u)) {
            /* 片选上升：开始一次传输 */
            r->input = 0;
            r->input_bit = 0;
            r->input_pos = 0;
            memset(r->output, 0, sizeof r->output);
            r->output_bit = 0;
            r->output_pos = 0;
        } else if (!(val & 0x0002u)) {
            /* 时钟下降沿 */
            if (val & 0x0010u) {
                /* 主机写：锁存数据位 */
                if (val & 0x0001u)
                    r->input |= (uint8_t)(1u << r->input_bit);
                r->input_bit++;
                if (r->input_bit >= 8) {
                    r->input_bit = 0;
                    rtc_byte_in(r, r->input);
                    r->input = 0;
                    r->input_pos++;
                }
            } else {
                /* 主机读：把输出位放到 bit0 */
                if (r->output[r->output_pos] & (1u << r->output_bit))
                    r->io |= 0x0001u;
                else
                    r->io &= (uint16_t)~0x0001u;
                r->output_bit++;
                if (r->output_bit >= 8) {
                    r->output_bit = 0;
                    if (r->output_pos < 7)
                        r->output_pos++;
                }
            }
        }
    }

    if (val & 0x0010u)
        r->io = val;                                  /* 写方向：整字写入 */
    else
        r->io = (uint16_t)((r->io & 0x0001u) | (val & 0xFFFEu));
}
