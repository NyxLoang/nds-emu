#ifndef NDS_EMU_CART_H
#define NDS_EMU_CART_H

#include <stddef.h>
#include <stdint.h>

/* 卡带数据：整份 .nds 文件读进内存后的持有者。
   data 指向堆缓冲区，size 为字节数；用 cart_free 释放。 */
typedef struct cart {
    unsigned char *data;
    size_t size;
} cart_t;

/* NDS ROM 头里解析出的关键字段（32 位小端）。 */
typedef struct cart_header {
    uint32_t arm9_offset; /* 0x020 ARM9 代码在文件里的偏移 */
    uint32_t arm9_entry;  /* 0x024 ARM9 CPU 复位后开始执行的地址 */
    uint32_t arm9_ram;    /* 0x028 ARM9 镜像装载到内存的地址 */
    uint32_t arm9_size;   /* 0x02C ARM9 镜像字节数 */
} cart_header_t;

/* 读整份文件到堆缓冲。失败返回 NULL（错误信息写入 err）。 */
cart_t *cart_load(const char *path, char *err, size_t errsz);

/* 按偏移解析 ROM 头。文件不足头长时返回 -1。 */
int cart_parse_header(const cart_t *cart, cart_header_t *hdr);

/* 释放 cart_t 及内部缓冲。 */
void cart_free(cart_t *cart);

#endif /* NDS_EMU_CART_H */
