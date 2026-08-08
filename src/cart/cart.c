#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cart.h"

cart_t *cart_load(const char *path, char *err, size_t errsz)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        snprintf(err, errsz, "cart_load: cannot open %s", path);
        return NULL;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        snprintf(err, errsz, "cart_load: fseek failed on %s", path);
        fclose(fp);
        return NULL;
    }
    long size = ftell(fp);
    if (size < 0) {
        snprintf(err, errsz, "cart_load: ftell failed on %s", path);
        fclose(fp);
        return NULL;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        snprintf(err, errsz, "cart_load: rewind failed on %s", path);
        fclose(fp);
        return NULL;
    }

    cart_t *cart = malloc(sizeof(cart_t));
    if (cart == NULL) {
        snprintf(err, errsz, "cart_load: out of memory");
        fclose(fp);
        return NULL;
    }
    cart->data = malloc((size_t)size);
    if (cart->data == NULL) {
        snprintf(err, errsz, "cart_load: out of memory (%ld bytes)", size);
        free(cart);
        fclose(fp);
        return NULL;
    }

    size_t got = fread(cart->data, 1, (size_t)size, fp);
    fclose(fp);
    if (got != (size_t)size) {
        snprintf(err, errsz, "cart_load: short read on %s (%zu/%ld)", path, got, size);
        free(cart->data);
        free(cart);
        return NULL;
    }

    cart->size = (size_t)size;
    return cart;
}

void cart_free(cart_t *cart)
{
    if (cart == NULL)
        return;
    free(cart->data);
    free(cart);
}

/* 从缓冲区读 32 位小端整数（低地址先放低字节）。 */
static uint32_t read_le32(const unsigned char *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

int cart_parse_header(const cart_t *cart, cart_header_t *hdr)
{
    /* 需要读到 0x02C+4，至少 0x30 字节 */
    if (cart->size < 0x30)
        return -1;

    hdr->arm9_offset = read_le32(cart->data + 0x020);
    hdr->arm9_entry  = read_le32(cart->data + 0x024);
    hdr->arm9_ram    = read_le32(cart->data + 0x028);
    hdr->arm9_size   = read_le32(cart->data + 0x02C);
    return 0;
}
