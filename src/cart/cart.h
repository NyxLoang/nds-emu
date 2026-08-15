#ifndef NDS_EMU_CART_H
#define NDS_EMU_CART_H

#include <stddef.h>
#include <stdint.h>

#include "arm9.h"
#include "arm7.h"

/* 卡带数据：整份 .nds 文件读进内存后的持有者。
   data 指向堆缓冲区，size 为字节数；用 cart_free 释放。 */
typedef struct cart {
    unsigned char *data;
    size_t size;
} cart_t;

/* NDS ROM 头解析结果：ARM9 + ARM7 两组字段。 */
typedef struct cart_header {
    arm9_header_t arm9;
    arm7_header_t arm7;
} cart_header_t;

/* 读整份文件到堆缓冲。失败返回 NULL（错误信息写入 err）。 */
cart_t *cart_load(const char *path, char *err, size_t errsz);

#ifdef _WIN32
#include <wchar.h>
/* 宽字符版：支持含中文/Unicode 的路径（Windows 用 _wfopen）。 */
cart_t *cart_load_w(const wchar_t *path, char *err, size_t errsz);
#endif

/* 按偏移解析 ROM 头（ARM9 + ARM7）。文件不足头长时返回 -1。 */
int cart_parse_header(const cart_t *cart, cart_header_t *hdr);

/* 阶段 14：若卡带安全区被 KEY1 加密，就地解密 ROM 缓冲里 ARM9 镜像开头的
   0x800 字节安全区。gamecode 由本函数从头 0x00C 处读出。
   返回 1 = 已解密（"encryObj" 校验通过）；0 = 未加密（homebrew）或解密失败（缓冲保持原样）。 */
int cart_decrypt_secure_area(cart_t *cart);

/* 释放 cart_t 及内部缓冲。 */
void cart_free(cart_t *cart);

#endif /* NDS_EMU_CART_H */
