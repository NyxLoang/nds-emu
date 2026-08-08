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
