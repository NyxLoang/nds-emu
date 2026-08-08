#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <wchar.h>
#endif
#include "cart.h"

/* 从已打开的流读完整份文件到堆缓冲。失败返回 NULL（错误信息写入 err）。
   fname 仅用于错误信息。 */
static cart_t *cart_load_fp(FILE *fp, const char *fname, char *err, size_t errsz)
{
    if (fseek(fp, 0, SEEK_END) != 0) {
        snprintf(err, errsz, "cart_load: fseek failed on %s", fname);
        fclose(fp);
        return NULL;
    }
    long size = ftell(fp);
    if (size < 0) {
        snprintf(err, errsz, "cart_load: ftell failed on %s", fname);
        fclose(fp);
        return NULL;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        snprintf(err, errsz, "cart_load: rewind failed on %s", fname);
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
        snprintf(err, errsz, "cart_load: short read on %s (%zu/%ld)", fname, got, size);
        free(cart->data);
        free(cart);
        return NULL;
    }

    cart->size = (size_t)size;
    return cart;
}

cart_t *cart_load(const char *path, char *err, size_t errsz)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        snprintf(err, errsz, "cart_load: cannot open %s", path);
        return NULL;
    }
    return cart_load_fp(fp, path, err, errsz);
}

#ifdef _WIN32
cart_t *cart_load_w(const wchar_t *path, char *err, size_t errsz)
{
    FILE *fp = _wfopen(path, L"rb");
    if (fp == NULL) {
        snprintf(err, errsz, "cart_load: cannot open (wide path)");
        return NULL;
    }
    return cart_load_fp(fp, "(wide path)", err, errsz);
}
#endif

void cart_free(cart_t *cart)
{
    if (cart == NULL)
        return;
    free(cart->data);
    free(cart);
}

int cart_parse_header(const cart_t *cart, cart_header_t *hdr)
{
    if (arm9_parse(cart->data, cart->size, &hdr->arm9) != 0)
        return -1;
    if (arm7_parse(cart->data, cart->size, &hdr->arm7) != 0)
        return -1;
    return 0;
}
