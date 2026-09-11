/*
   FreeBIOS（NDS ARM7/ARM9 BIOS 替代品）
   Copyright (c) 2013, Gilead Kutnick — BSD 2-Clause License
   （完整许可文本见 melonDS 的 src/FreeBIOS.cpp / FreeBIOS.h）

   本文件只复制供读取的少量数据字节（向量表 0x00-0x1F、SWI 函数表
   0x10B0-0x112B），用于让「游戏恢复 BIOS 内部现场」这条路径与参考核一致。 */

#include <stdint.h>
#include "bios7_image.h"

/* 0x0000-0x001F：ARM7 异常向量表 8 个字
   （b boot_handler / b unhandled / b swi_handler / ... / b interrupt_handler）。 */
static const uint32_t s_vec[8] = {
    0xEA00041Cu, 0xEA00041Cu, 0xEA00041Cu, 0xEA00041Au,
    0xEA000419u, 0xEA000418u, 0xEA0007E4u, 0xEA000416u,
};

/* 0x10B0-0x112B：SWI 0x00-0x1E 的函数表（0x1F 在 FreeBIOS 里落到表外）。 */
static const uint32_t s_swi_table[31] = {
    0x00001FD8u, 0x0000112Cu, 0x0000112Cu, 0x0000115Cu, 0x00001190u,
    0x00001188u, 0x0000114Cu, 0x000011B8u, 0x000011C8u, 0x000011E4u,
    0x0000112Cu, 0x00001240u, 0x000012C0u, 0x00001300u, 0x0000134Cu,
    0x000013ECu, 0x000013F4u, 0x00001488u, 0x00001504u, 0x000015B0u,
    0x000015B4u, 0x000015B4u, 0x0000112Cu, 0x0000112Cu, 0x0000112Cu,
    0x0000112Cu, 0x0000168Cu, 0x00001CA0u, 0x00001FC8u, 0x00001F88u,
    0x00001FA4u,
};

#define BIOS7_SWI_TABLE_BASE 0x000010B0u
#define BIOS7_SWI_TABLE_LEN  (31u * 4u)

int bios7_image_read8(uint32_t addr, uint8_t *out)
{
    if (addr < 0x20u) {
        *out = (uint8_t)(s_vec[addr >> 2] >> ((addr & 3u) * 8u));
        return 1;
    }
    if (addr >= BIOS7_SWI_TABLE_BASE &&
        addr - BIOS7_SWI_TABLE_BASE < BIOS7_SWI_TABLE_LEN) {
        uint32_t rel = addr - BIOS7_SWI_TABLE_BASE;
        *out = (uint8_t)(s_swi_table[rel >> 2] >> ((rel & 3u) * 8u));
        return 1;
    }
    return 0;
}
