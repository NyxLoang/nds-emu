#include "key1.h"

/* NDS KEY1（任天堂定制 Blowfish）实现。
   参考：melonDS NDSCart.cpp（Key1_Encrypt/Decrypt/ApplyKeycode/InitKeycode/DecryptSecureArea）
   与 ndstool encryption.cpp、nds-bootstrap decompress.c 完全一致。

   密钥表布局（0x412 个 32 位字，共 0x1048 字节）：
     [0x000..0x011] P 数组 18 字
     [0x012..0x111] S1 盒 256 字
     [0x112..0x211] S2 盒 256 字
     [0x212..0x311] S3 盒 256 字
     [0x312..0x411] S4 盒 256 字
   每个 32 位字按小端从字节表读出（低字节在前）。 */

/* 密钥表常量：由 tools/gen_key1_table.py 从 ndstool 生成（= NDS ARM7 BIOS 0x30..0x1077）。 */
static const uint8_t key1_table[0x1048] = {
#include "key1_table.inc"
};

/* 从字节表读 32 位小端整数（低地址先放低字节），避免对 uint8_t* 强转 u32* 的未对齐访问。 */
static uint32_t load_le32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* 把 32 位整数按小端写回字节缓冲。 */
static void store_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

uint32_t key1_bswap32(uint32_t v)
{
    return ((v & 0x000000FFu) << 24)
         | ((v & 0x0000FF00u) << 8)
         | ((v & 0x00FF0000u) >> 8)
         | ((v & 0xFF000000u) >> 24);
}

/* 把字节常量表装入 0x412 字的密钥表（小端拆字）。 */
static void key1_load_keybuf(uint32_t *keybuf)
{
    for (uint32_t i = 0; i < 0x412; i++)
        keybuf[i] = load_le32(&key1_table[i * 4]);
}

/* Blowfish F 函数：把 z 拆成 4 个字节，分别查 S1..S4 后按
   ((S1 + S2) ^ S3) + S4 组合。 */
static uint32_t key1_lookup(const uint32_t *keybuf, uint32_t z)
{
    uint32_t x = keybuf[0x12 + (z >> 24)];
    x += keybuf[0x112 + ((z >> 16) & 0xFF)];
    x ^= keybuf[0x212 + ((z >> 8) & 0xFF)];
    x += keybuf[0x312 + (z & 0xFF)];
    return x;
}

void key1_encrypt64(const uint32_t *keybuf, uint32_t *data)
{
    uint32_t y = data[0]; /* 低字 */
    uint32_t x = data[1]; /* 高字 */
    uint32_t z;

    for (uint32_t i = 0; i <= 0xF; i++) {
        z = keybuf[i] ^ x;
        x = key1_lookup(keybuf, z) ^ y;
        y = z;
    }
    data[0] = x ^ keybuf[0x10];
    data[1] = y ^ keybuf[0x11];
}

void key1_decrypt64(const uint32_t *keybuf, uint32_t *data)
{
    uint32_t y = data[0]; /* 低字 */
    uint32_t x = data[1]; /* 高字 */
    uint32_t z;

    for (uint32_t i = 0x11; i >= 0x2; i--) {
        z = keybuf[i] ^ x;
        x = key1_lookup(keybuf, z) ^ y;
        y = z;
    }
    data[0] = x ^ keybuf[0x1];
    data[1] = y ^ keybuf[0x0];
}

/* apply_keycode：把 gamecode 派生的 keycode 混进密钥表（Blowfish 的密钥调度）。 */
static void key1_apply_keycode(uint32_t *keybuf, uint32_t *keycode)
{
    /* 先就地加密 keycode 的后两字、再前两字（keycode 被改写） */
    key1_encrypt64(keybuf, &keycode[1]);
    key1_encrypt64(keybuf, &keycode[0]);

    /* P 数组(18 字)每个 ^= bswap32(keycode[i % 2])，只用到 keycode[0]/[1] */
    for (uint32_t i = 0; i <= 0x11; i++)
        keybuf[i] ^= key1_bswap32(keycode[i & 1u]);

    /* 用新 P 数组把整张表（P + 4 个 S 盒）重滚一遍；temp 链式传递不重置 */
    uint32_t temp[2] = {0, 0};
    for (uint32_t i = 0; i <= 0x410; i += 2) {
        key1_encrypt64(keybuf, temp);
        keybuf[i] = temp[1];       /* 高字放偶数下标 */
        keybuf[i + 1] = temp[0];   /* 低字放奇数下标 */
    }
}

void key1_init_keycode(uint32_t *keybuf, uint32_t gamecode, uint32_t level)
{
    key1_load_keybuf(keybuf);

    uint32_t keycode[3] = {gamecode, gamecode >> 1, gamecode << 1};
    if (level >= 1)
        key1_apply_keycode(keybuf, keycode);
    if (level >= 2)
        key1_apply_keycode(keybuf, keycode);
    if (level >= 3) {
        keycode[1] <<= 1;
        keycode[2] >>= 1;
        key1_apply_keycode(keybuf, keycode);
    }
}

int key1_decrypt_secure_area(uint32_t gamecode, uint8_t *data)
{
    uint32_t keybuf[0x412];

    /* 1. level2 解密前 8 字节（剥最外层） */
    key1_init_keycode(keybuf, gamecode, 2);
    {
        uint32_t blk[2] = {load_le32(data), load_le32(data + 4)};
        key1_decrypt64(keybuf, blk);
        store_le32(data, blk[0]);
        store_le32(data + 4, blk[1]);
    }

    /* 2. level3 解密全部 0x800 字节（每 8 字节一块） */
    key1_init_keycode(keybuf, gamecode, 3);
    for (uint32_t off = 0; off < 0x800; off += 8) {
        uint32_t blk[2] = {load_le32(data + off), load_le32(data + off + 4)};
        key1_decrypt64(keybuf, blk);
        store_le32(data + off, blk[0]);
        store_le32(data + off + 4, blk[1]);
    }

    /* 3. 校验魔数 "encryObj"（小端下 = 0x72636E65 / 0x6A624F79） */
    return data[0] == 'e' && data[1] == 'n' && data[2] == 'c' && data[3] == 'r'
        && data[4] == 'y' && data[5] == 'O' && data[6] == 'b' && data[7] == 'j';
}

void key1_encrypt_secure_area(uint32_t gamecode, uint8_t *data)
{
    uint32_t keybuf[0x412];

    /* 1. 头 8 字节 = 明文魔数 "encryObj" */
    data[0] = 'e'; data[1] = 'n'; data[2] = 'c'; data[3] = 'r';
    data[4] = 'y'; data[5] = 'O'; data[6] = 'b'; data[7] = 'j';

    /* 2. level3 加密 body（8..0x800，只加一层） */
    key1_init_keycode(keybuf, gamecode, 3);
    for (uint32_t off = 8; off < 0x800; off += 8) {
        uint32_t blk[2] = {load_le32(data + off), load_le32(data + off + 4)};
        key1_encrypt64(keybuf, blk);
        store_le32(data + off, blk[0]);
        store_le32(data + off + 4, blk[1]);
    }

    /* 3. level3 加密头 8 字节（内层） */
    {
        uint32_t blk[2] = {load_le32(data), load_le32(data + 4)};
        key1_encrypt64(keybuf, blk);
        store_le32(data, blk[0]);
        store_le32(data + 4, blk[1]);
    }

    /* 4. level2 加密头 8 字节（最外层） */
    key1_init_keycode(keybuf, gamecode, 2);
    {
        uint32_t blk[2] = {load_le32(data), load_le32(data + 4)};
        key1_encrypt64(keybuf, blk);
        store_le32(data, blk[0]);
        store_le32(data + 4, blk[1]);
    }
}

uint32_t key1_table_checksum(void)
{
    uint32_t sum = 0;
    for (uint32_t i = 0; i < 0x1048; i++)
        sum += key1_table[i];
    return sum;
}
