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
    /* NDS 约定：按下=0。低 12 位取反；高 4 位（bit12-15）恒为 1。 */
    k->input = (uint16_t)((~pressed) & 0x0FFFu) | 0xF000u;
}
