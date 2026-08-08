#ifndef NDS_EMU_CART_H
#define NDS_EMU_CART_H

#include <stddef.h>

/* 卡带数据：整份 .nds 文件读进内存后的持有者。
   data 指向堆缓冲区，size 为字节数；用 cart_free 释放。 */
typedef struct cart {
    unsigned char *data;
    size_t size;
} cart_t;

/* 读整份文件到堆缓冲。失败返回 NULL（错误信息写入 err）。 */
cart_t *cart_load(const char *path, char *err, size_t errsz);

/* 释放 cart_t 及内部缓冲。 */
void cart_free(cart_t *cart);

#endif /* NDS_EMU_CART_H */
