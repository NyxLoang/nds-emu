#ifndef NDS_EMU_CART_SAVE_H
#define NDS_EMU_CART_SAVE_H

#include <stdint.h>
#include <stddef.h>

/* 阶段 16：存档芯片（backup memory）。挂在卡带槽辅助 SPI 总线（AUXSPICNT/AUXSPIDATA），
   与阶段 15 的 ROM 命令总线（ROMCTRL/CARD_COMMAND/CARD_DATA）是两套独立接口。
   本文件只做「芯片级」仿真：SPI 字节状态机（命令/地址/数据）+ 存档缓冲 + 文件持久化。
   片选（AUXSPICNT bit13/bit15）由 cartbus 管理，它决定何时复位命令、何时传输字节。 */

typedef enum {
    SAVE_NONE = 0,
    SAVE_EEPROM_512B,   /* 512 字节（16B 页，9 位地址，A8 在 opcode bit3） */
    SAVE_EEPROM_8K,     /* 8KB（32B 页，16 位地址） */
    SAVE_EEPROM_64K,    /* 64KB（128B 页，24 位地址） */
    SAVE_EEPROM_128K,   /* 128KB（128B 页，24 位地址，只用 17 位） */
    SAVE_FLASH_256K,    /* 256KB Flash（256B 页，24 位地址） */
    SAVE_FLASH_512K,    /* 512KB Flash */
    SAVE_FLASH_1M,      /* 1MB Flash */
    SAVE_FLASH_8M,      /* 8MB Flash */
    SAVE_FRAM_32K,      /* 32KB FRAM（无页限制，16 位地址） */
} save_type_t;

/* SPI 命令状态机相位 */
enum {
    SAVE_IDLE = 0,  /* 等待操作码（新命令开头） */
    SAVE_WRSR,      /* WRSR：下一字节是状态寄存器值 */
    SAVE_RDID,      /* RDID：连续回 3 字节 JEDEC ID */
    SAVE_ADDR,      /* 收地址字节 */
    SAVE_FASTDUMMY, /* FAST READ：地址后 1 个哑元字节 */
    SAVE_READ,      /* 读数据 */
    SAVE_WRITE,     /* 写数据 */
};

typedef struct save {
    uint8_t     *data;        /* 存档缓冲（save_configure 分配，save_free 释放） */
    size_t       size;        /* 缓冲字节数 */
    save_type_t  type;

    /* 芯片参数（configure 时按 type 填入） */
    int          addr_bytes;  /* READ/WRITE/擦除 的地址字节数 */
    int          is_512b;     /* 512B EEPROM：A8 位来自 opcode bit3 */
    int          is_flash;    /* Flash：写用「与」语义、擦除填 0xFF */
    int          page_size;   /* 写页大小（FRAM=0 无限制） */
    uint8_t      jedec_id[3]; /* RDID 返回的 JEDEC ID（Flash 用） */

    /* SPI 状态机 */
    int          phase;       /* SAVE_* 相位 */
    uint8_t      opcode;      /* 当前命令 */
    int          addr_count;  /* 已收到的地址字节数 */
    uint32_t     addr;        /* 有效地址（收齐后，含 512B 的 A8） */
    int          rd_id_idx;   /* RDID 已回字节数 */
    int          wen;         /* 写使能锁存 WEL（WREN 置位、WRDI/写擦后清） */
    uint8_t      status;      /* 状态寄存器：bit0 WIP(恒0) bit1 WEL */
} save_t;

/* 清零字段（不分配缓冲）。 */
void save_init(save_t *s);

/* 释放缓冲并复位。 */
void save_free(save_t *s);

/* 按类型分配并填 0xFF（擦除态）。返回 0 成功，-1 失败。 */
int save_configure(save_t *s, save_type_t type);

/* 复位命令状态机（片选撤下时调用，回到等待操作码）。 */
void save_reset_cmd(save_t *s);

/* 一次 SPI 字节传输：out = 主机写入（MOSI），返回芯片回读字节（MISO）。 */
uint8_t save_transfer(save_t *s, uint8_t out);

/* 文件持久化（窄字符路径）。load：存在则读入、不存在保持 0xFF 全新态；
   save：整块写回。返回 0 成功，-1 失败。 */
int save_load_file(save_t *s, const char *path);
int save_save_file(const save_t *s, const char *path);

#ifdef _WIN32
#include <wchar.h>
/* 宽字符版：支持中文/Unicode 路径。 */
int save_load_file_w(save_t *s, const wchar_t *path);
int save_save_file_w(const save_t *s, const wchar_t *path);
#endif

/* 由 ROM 路径派生 .sav 存档路径（替换扩展名）。返回 malloc 的缓冲，调用方负责 free。 */
#ifdef _WIN32
wchar_t *save_make_path_w(const wchar_t *rom_path);
#else
char *save_make_path(const char *rom_path);
#endif

#endif /* NDS_EMU_CART_SAVE_H */
