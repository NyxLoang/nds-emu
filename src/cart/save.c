#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "save.h"

/* 各存档类型的参数表：大小 / 地址字节数 / 是否 512B / 是否 Flash / 页大小 / JEDEC ID。 */
typedef struct {
    size_t       size;
    int          addr_bytes;
    int          is_512b;
    int          is_flash;
    int          page_size;
    const uint8_t *jedec_id;
} save_params_t;

/* ST（0x20）M45PE 系列与 Macronix MX25L64 的 JEDEC ID（厂商+型号，游戏经 RDID 识别芯片）。 */
static const uint8_t jedec_flash_256k[3] = { 0x20, 0x20, 0x12 }; /* M45PE20 */
static const uint8_t jedec_flash_512k[3] = { 0x20, 0x20, 0x14 }; /* M45PE40 */
static const uint8_t jedec_flash_1m[3]   = { 0x20, 0x20, 0x15 }; /* M45PE80 */
static const uint8_t jedec_flash_8m[3]   = { 0xC2, 0x20, 0x17 }; /* MX25L6445 */

static void save_params(save_type_t type, save_params_t *p)
{
    static const uint8_t no_id[3] = { 0, 0, 0 };
    switch (type) {
    case SAVE_EEPROM_512B: *p = (save_params_t){ 512, 1, 1, 0, 16, no_id }; break;
    case SAVE_EEPROM_8K:   *p = (save_params_t){ 8192, 2, 0, 0, 32, no_id }; break;
    case SAVE_EEPROM_64K:  *p = (save_params_t){ 65536, 3, 0, 0, 128, no_id }; break;
    case SAVE_EEPROM_128K: *p = (save_params_t){ 131072, 3, 0, 0, 128, no_id }; break;
    case SAVE_FLASH_256K:  *p = (save_params_t){ 262144, 3, 0, 1, 256, jedec_flash_256k }; break;
    case SAVE_FLASH_512K:  *p = (save_params_t){ 524288, 3, 0, 1, 256, jedec_flash_512k }; break;
    case SAVE_FLASH_1M:    *p = (save_params_t){ 1048576, 3, 0, 1, 256, jedec_flash_1m }; break;
    case SAVE_FLASH_8M:    *p = (save_params_t){ 8388608, 3, 0, 1, 256, jedec_flash_8m }; break;
    case SAVE_FRAM_32K:    *p = (save_params_t){ 32768, 2, 0, 0, 0, no_id }; break;
    case SAVE_NONE:
    default:               *p = (save_params_t){ 0, 0, 0, 0, 0, no_id }; break;
    }
}

void save_init(save_t *s)
{
    memset(s, 0, sizeof(*s));
    s->type = SAVE_NONE;
    s->status = 0;
}

void save_free(save_t *s)
{
    free(s->data);
    s->data = NULL;
    s->size = 0;
    s->type = SAVE_NONE;
    save_reset_cmd(s);
}

int save_configure(save_t *s, save_type_t type)
{
    save_params_t p;
    save_params(type, &p);
    if (p.size == 0)
        return -1;

    free(s->data);
    s->data = (uint8_t *)malloc(p.size);
    if (s->data == NULL)
        return -1;
    memset(s->data, 0xFF, p.size); /* 擦除态 = 0xFF（Flash/EEPROM 出厂态） */

    s->size = p.size;
    s->type = type;
    s->addr_bytes = p.addr_bytes;
    s->is_512b = p.is_512b;
    s->is_flash = p.is_flash;
    s->page_size = p.page_size;
    memcpy(s->jedec_id, p.jedec_id, 3);
    save_reset_cmd(s);
    return 0;
}

void save_reset_cmd(save_t *s)
{
    s->phase = SAVE_IDLE;
    s->opcode = 0;
    s->addr_count = 0;
    s->addr = 0;
    s->rd_id_idx = 0;
}

/* 把一段区域擦成 0xFF（Flash 擦除；EEPROM 无需擦除，本函数仅供 Flash）。 */
static void save_erase_range(save_t *s, uint32_t start, size_t len)
{
    for (size_t i = 0; i < len && (start + i) < s->size; i++)
        s->data[start + i] = 0xFF;
}

/* 写一个字节：Flash 用「与」语义（只能 1→0，擦除后才能写 1）；EEPROM/FRAM 直接覆盖。 */
static void save_write_byte(save_t *s, uint32_t a, uint8_t v)
{
    a &= (uint32_t)(s->size - 1);
    if (s->is_flash)
        s->data[a] &= v;
    else
        s->data[a] = v;
}

/* 写数据阶段的地址推进：EEPROM/Flash 按页回绕（跨页写回到本页开头），FRAM 无页限制。 */
static void save_advance_write(save_t *s)
{
    uint32_t a = s->addr;
    if (s->page_size > 0) {
        uint32_t page = (uint32_t)s->page_size;
        a = (a & ~(page - 1)) | ((a + 1) & (page - 1));
    } else {
        a = (a + 1) & (uint32_t)(s->size - 1);
    }
    s->addr = a;
}

uint8_t save_transfer(save_t *s, uint8_t out)
{
    if (s->data == NULL || s->size == 0)
        return 0xFF;

    /* ---- 新命令：操作码 ---- */
    if (s->phase == SAVE_IDLE) {
        s->opcode = out;
        s->addr = 0;
        s->addr_count = 0;
        s->rd_id_idx = 0;

        switch (out) {
        case 0x06: /* WREN 写使能 */
            s->wen = 1;
            s->status |= 0x02;
            return 0;
        case 0x04: /* WRDI 写禁止 */
            s->wen = 0;
            s->status &= (uint8_t)~0x02;
            return 0;
        case 0x05: /* RDSR 读状态 */
            return s->status;
        case 0x01: /* WRSR 写状态（下一字节是状态值） */
            s->phase = SAVE_WRSR;
            return 0;
        case 0x9F: /* RDID 读 JEDEC ID（命令字节后连续回 3 字节） */
            s->phase = SAVE_RDID;
            return 0;
        case 0xC7: /* CE 整片擦除（0x60 同） */
        case 0x60:
            if (s->is_flash) save_erase_range(s, 0, s->size);
            s->wen = 0;
            s->status &= (uint8_t)~0x02;
            return 0;
        case 0xB9: /* DP 深度掉电 */
        case 0xAB: /* RDP 退出掉电 */
            return 0;
        case 0x03: /* READ 读数据（地址字节数 = 芯片类型决定） */
            s->phase = SAVE_ADDR;
            return 0;
        case 0x02: /* WRITE / PP 写数据 */
        case 0x0A: /* PW 页写（M45PE 别名） */
            s->phase = SAVE_ADDR;
            return 0;
        case 0x0B: /* FAST READ（地址 + 1 哑元，Flash 固定 24 位地址） */
            s->phase = SAVE_ADDR;
            s->addr_bytes = 3;
            return 0;
        case 0xD8: /* SE 扇区擦除（Flash 固定 24 位地址） */
        case 0xDB: /* PE 页擦除 */
            s->phase = SAVE_ADDR;
            s->addr_bytes = 3;
            return 0;
        default:   /* 未知命令：单字节无地址，回 IDLE */
            s->phase = SAVE_IDLE;
            return 0;
        }
    }

    /* ---- WRSR 的数据字节 ---- */
    if (s->phase == SAVE_WRSR) {
        s->status = (uint8_t)((s->status & 0x03) | (out & 0xFC)); /* bit0-1 保留，其余可写 */
        s->wen = (s->status >> 1) & 1;
        s->phase = SAVE_IDLE;
        return 0;
    }

    /* ---- RDID 继续回 ID ---- */
    if (s->phase == SAVE_RDID) {
        uint8_t id = s->rd_id_idx < 3 ? s->jedec_id[s->rd_id_idx] : 0xFF;
        s->rd_id_idx++;
        if (s->rd_id_idx >= 3)
            s->phase = SAVE_IDLE;
        return id;
    }

    /* ---- 收地址字节 ---- */
    if (s->phase == SAVE_ADDR) {
        s->addr = (s->addr << 8) | out;
        s->addr_count++;
        if (s->addr_count < s->addr_bytes)
            return 0;

        /* 地址收齐：512B EEPROM 的 A8 位来自 opcode bit3（9 位地址） */
        if (s->is_512b)
            s->addr |= ((s->opcode >> 3) & 1u) << 8;

        switch (s->opcode) {
        case 0x03: /* READ */
            s->phase = SAVE_READ;
            return 0;
        case 0x0B: /* FAST READ：先吞 1 个哑元字节 */
            s->phase = SAVE_FASTDUMMY;
            return 0;
        case 0x02: /* WRITE / PP / PW */
        case 0x0A:
            s->phase = SAVE_WRITE;
            return 0;
        case 0xD8: /* SE：扇区擦除 64KB（对齐到 0x10000） */
            if (s->is_flash) save_erase_range(s, s->addr & ~0xFFFFu, 0x10000);
            s->wen = 0; s->status &= (uint8_t)~0x02; s->phase = SAVE_IDLE; return 0;
        case 0xDB: /* PE：页擦除 256B（对齐到 0x100） */
            if (s->is_flash) save_erase_range(s, s->addr & ~0xFFu, 0x100);
            s->wen = 0; s->status &= (uint8_t)~0x02; s->phase = SAVE_IDLE; return 0;
        default:
            s->phase = SAVE_IDLE;
            return 0;
        }
    }

    /* ---- FAST READ 的哑元字节 ---- */
    if (s->phase == SAVE_FASTDUMMY) {
        s->phase = SAVE_READ;
        return 0;
    }

    /* ---- 读数据 ---- */
    if (s->phase == SAVE_READ) {
        uint8_t v = s->data[s->addr & (uint32_t)(s->size - 1)];
        s->addr = (s->addr + 1) & (uint32_t)(s->size - 1);
        return v;
    }

    /* ---- 写数据 ---- */
    if (s->phase == SAVE_WRITE) {
        save_write_byte(s, s->addr, out);
        save_advance_write(s);
        return 0;
    }

    return 0;
}

int save_load_file(save_t *s, const char *path)
{
    if (s->data == NULL)
        return -1;
    memset(s->data, 0xFF, s->size); /* 先回擦除态，文件不足部分保持 0xFF */
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return 0; /* 文件不存在：视为全新存档 */
    (void)!fread(s->data, 1, s->size, f);
    fclose(f);
    return 0;
}

int save_save_file(const save_t *s, const char *path)
{
    if (s->data == NULL)
        return -1;
    FILE *f = fopen(path, "wb");
    if (f == NULL)
        return -1;
    size_t n = fwrite(s->data, 1, s->size, f);
    fclose(f);
    return (n == s->size) ? 0 : -1;
}

#ifdef _WIN32
int save_load_file_w(save_t *s, const wchar_t *path)
{
    if (s->data == NULL)
        return -1;
    memset(s->data, 0xFF, s->size);
    FILE *f = _wfopen(path, L"rb");
    if (f == NULL)
        return 0;
    (void)!fread(s->data, 1, s->size, f);
    fclose(f);
    return 0;
}

int save_save_file_w(const save_t *s, const wchar_t *path)
{
    if (s->data == NULL)
        return -1;
    FILE *f = _wfopen(path, L"wb");
    if (f == NULL)
        return -1;
    size_t n = fwrite(s->data, 1, s->size, f);
    fclose(f);
    return (n == s->size) ? 0 : -1;
}
#endif
