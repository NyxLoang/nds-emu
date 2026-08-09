#ifndef NDS_EMU_IO_KEY_H
#define NDS_EMU_IO_KEY_H

#include <stdint.h>

/* KEYINPUT 寄存器地址（16 位只读；高 4 位恒读 1） */
#define IO_KEYINPUT_ADDR 0x04000130u
#define IO_KEYINPUT_END  0x04000134u   /* KEYCNT 等后续阶段，上界（不含） */

/* NDS 按键位（KEYINPUT 低 12 位，按下=0） */
#define KEY_A       (1u << 0)
#define KEY_B       (1u << 1)
#define KEY_SELECT  (1u << 2)
#define KEY_START   (1u << 3)
#define KEY_RIGHT   (1u << 4)
#define KEY_LEFT    (1u << 5)
#define KEY_UP      (1u << 6)
#define KEY_DOWN    (1u << 7)
#define KEY_R       (1u << 8)
#define KEY_L       (1u << 9)
#define KEY_X       (1u << 10)
#define KEY_Y       (1u << 11)

/* 按键状态：input 就是 KEYINPUT 的读值（按下=0，未按=1，高 4 位恒 1） */
typedef struct keypad {
    uint16_t input;
} keypad_t;

uint8_t key_read8(const keypad_t *k, uint32_t addr);
void key_write8(keypad_t *k, uint32_t addr, uint8_t val);

/* 外部信号：pressed 的某位=1 表示该键按下；转成 NDS 读值（按下=0） */
void key_set_pressed(keypad_t *k, uint16_t pressed);

#endif /* NDS_EMU_IO_KEY_H */
