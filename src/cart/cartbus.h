#ifndef NDS_EMU_CART_CARTBUS_H
#define NDS_EMU_CART_CARTBUS_H

#include <stdint.h>
#include <stddef.h>
#include "save.h"

/* 阶段 15：卡带总线寄存器（NDS7 与 NDS9 同址）。
   卡带总线由 ARM7 主控，但两核访问同一组寄存器。数据端口 CARD_DATA 在 IO 区外
   （0x04100010，与 IPC FIFO RECV 0x04100000 类似），由 bus 单独转发。
   阶段 16：AUXSPICNT/AUXSPIDATA 接上存档芯片（save.h）的真 SPI 语义。 */

/* 控制寄存器（IO 区 0x040001A0 起） */
#define CART_AUXSPICNT   0x040001A0u   /* 2 字节：卡带 SPI/ROM 控制 */
#define CART_AUXSPIDATA  0x040001A2u   /* 1 字节：SPI 数据（写=启动一次传输） */
#define CART_ROMCTRL     0x040001A4u   /* 4 字节：卡带总线时序/控制 */
#define CART_COMMAND     0x040001A8u   /* 8 字节：命令输出（..AF，大端） */
#define CART_CMD_END     0x040001B0u   /* 命令区上界（含 KEY2 种子区，本阶段忽略） */

/* AUXSPICNT 关键位（16 位寄存器） */
#define AUXSPICNT_HOLD      (1u << 6)   /* 保持片选：0=每字节后撤、1=保持 */
#define AUXSPICNT_BUSY      (1u << 7)   /* 忙（只读，瞬时模型恒 0） */
#define AUXSPICNT_SLOTMODE  (1u << 13)  /* 0=ROM 并行模式，1=SPI/存档模式 */
#define AUXSPICNT_ENABLE    (1u << 15)  /* 槽使能 */
#define AUXSPICNT_CS        (AUXSPICNT_SLOTMODE | AUXSPICNT_ENABLE) /* 选中存档芯片 */

/* 数据端口（IO 区外，由 bus_read32/bus_write32 转发） */
#define CART_DATA        0x04100010u   /* 4 字节：数据端口（读自动 +4） */

/* ROMCTRL 关键位（32 位寄存器，真机 bit 编号） */
#define CART_ROMCTRL_DRQ        (1u << 23)  /* 数据就绪（只读）：1=Ready/DRQ */
#define CART_ROMCTRL_BLOCK_MASK (7u << 24)  /* 块大小：0=None 1=0x100..7=4 字节 */
#define CART_ROMCTRL_ACTIVATE   (1u << 31)  /* 启动传输（写 1 锁存命令并开始） */

/* 卡带总线状态：持有已装载 ROM 的只读指针 + 传输游标。
   ROM 数据本体由 cart_t 持有（cart_load），这里只借指针，不负责释放。 */
typedef struct cartbus {
    const uint8_t *rom;        /* 指向 cart->data（未装载为 NULL） */
    size_t         rom_size;   /* ROM 字节数 */
    uint16_t       auxspicnt;  /* AUXSPICNT */
    uint16_t       auxspidata; /* AUXSPIDATA（低字节有效） */
    uint32_t       romctrl;    /* ROMCTRL（含 bit23 DRQ） */
    uint8_t        cmd[8];     /* CARD_COMMAND 8 字节（大端） */
    uint32_t       xfer_addr;  /* 当前传输读地址（命令里的 32 位地址） */
    uint32_t       xfer_remaining; /* 本次传输剩余字节 */
    save_t         save;       /* 存档芯片（阶段 16：AUXSPI 的 SPI 从设备） */
    uint32_t       chip_id;    /* 21-B9w: 0xB8 命令返回的芯片 ID（由补力散后 ROM 大小算出） */
    int            chip_read;  /* 当前传输是否为 B8 ChipID 读取 */
    uint32_t       rom_mask;   /* 21-B9z: 补成 2 的幂后的 ROM 掩码（melonDS B7 使用） */
    uint32_t wait_cycles; /* 21-B9zb: next CARD_DATA ready delay in ARM9 cycles */
    uint32_t xfer_pos;    /* 21-B9zb: bytes transferred in current command */
    int      wait_phase;  /* 21-B9zb: 0=idle 1=waiting data 2=data ready */
} cartbus_t;

/* 清零初始化。 */
void cartbus_init(cartbus_t *cb);

/* 释放内部资源（存档缓冲）。 */
void cartbus_destroy(cartbus_t *cb);

/* 附着 ROM 数据（只借指针，不拷贝、不释放）。 */
void cartbus_attach(cartbus_t *cb, const uint8_t *rom, size_t rom_size);

/* 配置存档芯片类型（分配缓冲，填 0xFF 擦除态）。 */
int cartbus_attach_save(cartbus_t *cb, save_type_t type);

/* 判断地址是否落在卡带控制寄存器区（0x040001A0..0x040001AF）。 */
int cartbus_is_addr(uint32_t addr);

/* 控制寄存器按字节读写（AUXSPICNT/AUXSPIDATA/ROMCTRL/CARD_COMMAND）。
   写 ROMCTRL bit31 会锁存命令并开始传输（置 DRQ）。 */
uint8_t cartbus_read8(cartbus_t *cb, uint32_t addr);
void    cartbus_write8(cartbus_t *cb, uint32_t addr, uint8_t val);

/* 数据端口 CARD_DATA 32 位读（小端拼 4 字节，读后 xfer_addr += 4）。
   未装载 ROM 或越界返回 0xFFFFFFFF。 */
uint32_t cartbus_read32(cartbus_t *cb);
/* 21-B9zb: advance N ARM9 cycles; returns 1 when CARD_DATA becomes ready */
int cartbus_advance(cartbus_t *cb, uint32_t cycles);

/* 卡带是否处于「数据就绪」（ROMCTRL bit23）。DMA 卡带触发源据此点火。 */
static inline int cartbus_ready(const cartbus_t *cb)
{
    return (cb->romctrl & CART_ROMCTRL_DRQ) != 0;
}

#endif /* NDS_EMU_CART_CARTBUS_H */
