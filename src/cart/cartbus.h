#ifndef NDS_EMU_CART_CARTBUS_H
#define NDS_EMU_CART_CARTBUS_H

#include <stdint.h>
#include <stddef.h>

/* 阶段 15：卡带总线寄存器（NDS7 与 NDS9 同址）。
   卡带总线由 ARM7 主控，但两核访问同一组寄存器。数据端口 CARD_DATA 在 IO 区外
   （0x04100010，与 IPC FIFO RECV 0x04100000 类似），由 bus 单独转发。 */

/* 控制寄存器（IO 区 0x040001A0 起） */
#define CART_AUXSPICNT   0x040001A0u   /* 2 字节：卡带 SPI/ROM 控制 */
#define CART_AUXSPIDATA  0x040001A2u   /* 2 字节：SPI 数据 */
#define CART_ROMCTRL     0x040001A4u   /* 4 字节：卡带总线时序/控制 */
#define CART_COMMAND     0x040001A8u   /* 8 字节：命令输出（..AF，大端） */
#define CART_CMD_END     0x040001B0u   /* 命令区上界（含 KEY2 种子区，本阶段忽略） */

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
    uint16_t       auxspidata; /* AUXSPIDATA */
    uint32_t       romctrl;    /* ROMCTRL（含 bit23 DRQ） */
    uint8_t        cmd[8];     /* CARD_COMMAND 8 字节（大端） */
    uint32_t       xfer_addr;  /* 当前传输读地址（命令里的 32 位地址） */
    uint32_t       xfer_remaining; /* 本次传输剩余字节 */
} cartbus_t;

/* 清零初始化。 */
void cartbus_init(cartbus_t *cb);

/* 附着 ROM 数据（只借指针，不拷贝、不释放）。 */
void cartbus_attach(cartbus_t *cb, const uint8_t *rom, size_t rom_size);

/* 判断地址是否落在卡带控制寄存器区（0x040001A0..0x040001AF）。 */
int cartbus_is_addr(uint32_t addr);

/* 控制寄存器按字节读写（AUXSPICNT/AUXSPIDATA/ROMCTRL/CARD_COMMAND）。
   写 ROMCTRL bit31 会锁存命令并开始传输（置 DRQ）。 */
uint8_t cartbus_read8(cartbus_t *cb, uint32_t addr);
void    cartbus_write8(cartbus_t *cb, uint32_t addr, uint8_t val);

/* 数据端口 CARD_DATA 32 位读（小端拼 4 字节，读后 xfer_addr += 4）。
   未装载 ROM 或越界返回 0xFFFFFFFF。 */
uint32_t cartbus_read32(cartbus_t *cb);

/* 卡带是否处于「数据就绪」（ROMCTRL bit23）。DMA 卡带触发源据此点火。 */
static inline int cartbus_ready(const cartbus_t *cb)
{
    return (cb->romctrl & CART_ROMCTRL_DRQ) != 0;
}

#endif /* NDS_EMU_CART_CARTBUS_H */
