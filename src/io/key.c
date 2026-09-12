#include "key.h"

uint8_t key_read8(const keypad_t *k, uint32_t addr)
{
    if (addr >= IO_KEYINPUT_ADDR && addr < IO_KEYINPUT_END)
        return (uint8_t)(k->input >> ((addr - IO_KEYINPUT_ADDR) * 8));
    return 0; /* KEYCNT 等未实现：读 0 */
}

void key_write8(keypad_t *k, uint32_t addr, uint8_t val)
{
    (void)k; (void)addr; (void)val; /* KEYINPUT 只读，写忽略 */
}

void key_set_pressed(keypad_t *k, uint16_t pressed)
{
    /* 21-B9xy：按 melonDS 口径——KEYINPUT 只有低 10 位（A/B/Select/Start/
       右/左/上/下/R/L），松开=1；bit10-15 读回 0（参考核帧 1900 无按键时
       读到 0x03FF，本地此前是 0xFFFF）。 */
    k->input = (uint16_t)((~pressed) & 0x03FFu);
}

void key_reset(keypad_t *k)
{
    key_set_pressed(k, 0);   /* 无按键：全部为“松开” */
}
