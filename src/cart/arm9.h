#ifndef NDS_EMU_CART_ARM9_H
#define NDS_EMU_CART_ARM9_H

#include <stddef.h>
#include <stdint.h>

/* ARM9 头关键字段（32 位小端）。 */
typedef struct arm9_header {
    uint32_t offset; /* 0x020 ARM9 代码在文件里的偏移 */
    uint32_t entry;  /* 0x024 ARM9 CPU 复位后开始执行的地址 */
    uint32_t ram;    /* 0x028 ARM9 镜像装载到内存的地址 */
    uint32_t size;   /* 0x02C ARM9 镜像字节数 */
} arm9_header_t;

/* 从 ROM 缓冲解析 ARM9 头四字段。文件不足返回 -1。 */
int arm9_parse(const unsigned char *data, size_t size, arm9_header_t *hdr);

#endif /* NDS_EMU_CART_ARM9_H */
