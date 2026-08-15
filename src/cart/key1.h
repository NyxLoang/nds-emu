#ifndef NDS_EMU_CART_KEY1_H
#define NDS_EMU_CART_KEY1_H

#include <stdint.h>

/* KEY1（任天堂定制的 Blowfish）加解密，用于 NDS 商业卡带「安全区」。
   密钥表常量见 key1_table.inc（P 数组 18 字 + S 盒 4×256 字，共 0x1048 字节）。 */

/* 32 位字节序反转（bswap32）：0x12345678 -> 0x78563412。 */
uint32_t key1_bswap32(uint32_t v);

/* Blowfish 块加解密：就地处理 8 字节（data[0]=低字、data[1]=高字）。
   keybuf 是 0x412 字的密钥表（P 数组 + 4 个 S 盒）。 */
void key1_encrypt64(const uint32_t *keybuf, uint32_t *data);
void key1_decrypt64(const uint32_t *keybuf, uint32_t *data);

/* 按 gamecode 派生工作密钥表到 keybuf（调用方提供 0x412 字缓冲）。
   level 1/2/3 对应 GBATEK 的三段密钥调度；安全区解密用 level 2、level 3。 */
void key1_init_keycode(uint32_t *keybuf, uint32_t gamecode, uint32_t level);

/* 安全区解密：就地解密 0x800 字节安全区。
   返回 1 = 解密成功（开头 8 字节等于 "encryObj"）；0 = 未加密或校验失败（保持原样）。 */
int key1_decrypt_secure_area(uint32_t gamecode, uint8_t *data /* 0x800 in-place */);

/* 安全区加密（仅测试 / 自制 ROM 用）：就地加密 0x800 字节。
   调用后 data 的头 8 字节会被设为明文魔数 "encryObj" 再按协议加密。 */
void key1_encrypt_secure_area(uint32_t gamecode, uint8_t *data /* 0x800 in-place */);

/* 密钥表 0x1048 字节的累加和（自测/回归用，防止表被误改；期望 0x000803BA）。 */
uint32_t key1_table_checksum(void);

#endif /* NDS_EMU_CART_KEY1_H */
