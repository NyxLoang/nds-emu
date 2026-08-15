#include <string.h>
#include "cartbus.h"

void cartbus_init(cartbus_t *cb)
{
    memset(cb, 0, sizeof(*cb));
}

void cartbus_attach(cartbus_t *cb, const uint8_t *rom, size_t rom_size)
{
    cb->rom = rom;
    cb->rom_size = rom_size;
}

int cartbus_is_addr(uint32_t addr)
{
    return addr >= CART_AUXSPICNT && addr < CART_CMD_END;
}

/* 从 ROM 缓冲按小端读 32 位（低地址=低字节）。越界按 0xFF 补（未映射读语义）。 */
static uint32_t rom_le32(const uint8_t *rom, size_t rom_size, uint32_t addr)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t b = 0xFF;
        uint32_t a = addr + (uint32_t)i;
        if (a < rom_size)
            b = rom[a];
        v |= (uint32_t)b << (i * 8);
    }
    return v;
}

/* 锁存 CARD_COMMAND 并开始传输。
   只实现读命令 B7/B8（GetData）；其余命令（芯片 ID / KEY1 激活等）后续阶段补。 */
static void cartbus_activate(cartbus_t *cb)
{
    uint8_t c0 = cb->cmd[0];

    /* B7/B8：读 ROM。命令格式 B7 addr[31:24] addr[23:16] addr[15:8] addr[7:0] 00 00 00
       （命令字节大端：cmd[0] 是命令号，cmd[1..4] 是 32 位地址）。 */
    if (c0 == 0xB7 || c0 == 0xB8) {
        cb->xfer_addr = ((uint32_t)cb->cmd[1] << 24)
                      | ((uint32_t)cb->cmd[2] << 16)
                      | ((uint32_t)cb->cmd[3] << 8)
                      | ((uint32_t)cb->cmd[4]);
        uint32_t blk = (cb->romctrl & CART_ROMCTRL_BLOCK_MASK) >> 24;
        uint32_t size;
        if (blk == 0)      size = 0x200u;          /* None → 默认 0x200（游戏常规块） */
        else if (blk == 7) size = 4u;              /* 4 字节（KEY2 区用） */
        else               size = 0x100u << (blk - 1); /* 1=0x100 2=0x200 … 6=0x2000 */
        cb->xfer_remaining = size;
    } else {
        /* 未支持的命令：不产生数据，但仍置就绪，读 CARD_DATA 会得 0xFFFFFFFF。 */
        cb->xfer_addr = 0;
        cb->xfer_remaining = 0;
    }

    cb->romctrl |= CART_ROMCTRL_DRQ; /* 瞬时传输模型：立即就绪 */
}

uint8_t cartbus_read8(cartbus_t *cb, uint32_t addr)
{
    if (addr >= CART_AUXSPICNT && addr < CART_AUXSPICNT + 2)
        return (uint8_t)(cb->auxspicnt >> ((addr - CART_AUXSPICNT) * 8));
    if (addr >= CART_AUXSPIDATA && addr < CART_AUXSPIDATA + 2)
        return (uint8_t)(cb->auxspidata >> ((addr - CART_AUXSPIDATA) * 8));
    if (addr >= CART_ROMCTRL && addr < CART_ROMCTRL + 4)
        return (uint8_t)(cb->romctrl >> ((addr - CART_ROMCTRL) * 8));
    if (addr >= CART_COMMAND && addr < CART_COMMAND + 8)
        return cb->cmd[addr - CART_COMMAND];
    return 0;
}

void cartbus_write8(cartbus_t *cb, uint32_t addr, uint8_t val)
{
    if (addr >= CART_AUXSPICNT && addr < CART_AUXSPICNT + 2) {
        uint32_t shift = (addr - CART_AUXSPICNT) * 8;
        cb->auxspicnt = (uint16_t)((cb->auxspicnt & ~(0xFFu << shift))
                                   | ((uint32_t)val << shift));
        return;
    }
    if (addr >= CART_AUXSPIDATA && addr < CART_AUXSPIDATA + 2) {
        uint32_t shift = (addr - CART_AUXSPIDATA) * 8;
        cb->auxspidata = (uint16_t)((cb->auxspidata & ~(0xFFu << shift))
                                    | ((uint32_t)val << shift));
        return;
    }
    if (addr >= CART_ROMCTRL && addr < CART_ROMCTRL + 4) {
        uint32_t shift = (addr - CART_ROMCTRL) * 8;
        cb->romctrl = (cb->romctrl & ~(0xFFu << shift)) | ((uint32_t)val << shift);
        /* 写 bit31（最高字节的最高位）= 启动传输：锁存命令并置 DRQ */
        if (addr == CART_ROMCTRL + 3 && (val & 0x80u))
            cartbus_activate(cb);
        return;
    }
    if (addr >= CART_COMMAND && addr < CART_COMMAND + 8) {
        cb->cmd[addr - CART_COMMAND] = val;
        return;
    }
}

uint32_t cartbus_read32(cartbus_t *cb)
{
    if (cb->rom == NULL) {
        cb->romctrl &= ~CART_ROMCTRL_DRQ;
        return 0xFFFFFFFFu;
    }
    if (cb->xfer_remaining == 0) {
        cb->romctrl &= ~CART_ROMCTRL_DRQ;
        return 0xFFFFFFFFu;
    }

    uint32_t v = rom_le32(cb->rom, cb->rom_size, cb->xfer_addr);
    cb->xfer_addr += 4;
    cb->xfer_remaining = (cb->xfer_remaining > 4) ? cb->xfer_remaining - 4 : 0;
    if (cb->xfer_remaining == 0)
        cb->romctrl &= ~CART_ROMCTRL_DRQ; /* 本块读完，清就绪 */
    return v;
}
