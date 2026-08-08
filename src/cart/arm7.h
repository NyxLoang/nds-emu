#ifndef NDS_EMU_CART_ARM7_H
#define NDS_EMU_CART_ARM7_H

#include <stddef.h>
#include <stdint.h>

/* ARM7 头关键字段（32 位小端）。 */
typedef struct arm7_header {
    uint32_t offset; /* 0x030 ARM7 代码在文件里的偏移 */
    uint32_t entry;  /* 0x034 ARM7 CPU 复位后开始执行的地址 */
    uint32_t ram;    /* 0x038 ARM7 镜像装载到内存的地址 */
    uint32_t size;   /* 0x03C ARM7 镜像字节数 */
} arm7_header_t;

/* 从 ROM 缓冲解析 ARM7 头四字段。文件不足返回 -1。 */
int arm7_parse(const unsigned char *data, size_t size, arm7_header_t *hdr);

#endif /* NDS_EMU_CART_ARM7_H */
