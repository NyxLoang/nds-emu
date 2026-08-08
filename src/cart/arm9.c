#include <stddef.h>
#include <stdint.h>
#include "arm9.h"

/* 从缓冲区读 32 位小端整数（低地址先放低字节）。 */
static uint32_t read_le32(const unsigned char *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

int arm9_parse(const unsigned char *data, size_t size, arm9_header_t *hdr)
{
    /* 需要读到 0x02C+4，至少 0x30 字节 */
    if (size < 0x30)
        return -1;

    hdr->offset = read_le32(data + 0x020);
    hdr->entry  = read_le32(data + 0x024);
    hdr->ram    = read_le32(data + 0x028);
    hdr->size   = read_le32(data + 0x02C);
    return 0;
}
