/* ARM7 低地址 BIOS 可读字节（完整 FreeBIOS 镜像）。
   BSD-2 许可与来源说明见 bios7_rom.h 头部；21-B9yd 之前本文件只影子化
   向量表与 SWI 函数表，其余低地址读 0。 */

#include <stdint.h>
#include "bios7_image.h"
#include "bios7_rom.h"

const unsigned char *bios7_image_data(void)
{
    return bios7_rom;
}

uint32_t bios7_image_size(void)
{
    return BIOS7_ROM_SIZE;
}

uint32_t bios7_image_valid_size(void)
{
    return BIOS7_ROM_VALID;
}

int bios7_image_read8(uint32_t addr, uint8_t *out)
{
    if (addr >= BIOS7_ROM_SIZE)
        return 0;
    *out = bios7_rom[addr];
    return 1;
}
