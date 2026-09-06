#include <string.h>
#include "cartbus.h"

void cartbus_init(cartbus_t *cb)
{
    memset(cb, 0, sizeof(*cb));
    save_init(&cb->save);
}

void cartbus_destroy(cartbus_t *cb)
{
    save_free(&cb->save);
}

void cartbus_attach(cartbus_t *cb, const uint8_t *rom, size_t rom_size)
{
    cb->rom = rom;
    cb->rom_size = rom_size;
    /* 21-B9w：芯片 ID 由“补成 2 的幂后的 ROM 大小”推导（melonDS NDSCart），
       不是 ROM 头的游戏代码。 */
    size_t padded = 1;
    while (padded < rom_size)
        padded <<= 1;
    cb->chip_id = 0x000000C2u;
    if (padded >= 1024u * 1024u && padded <= 128u * 1024u * 1024u)
        cb->chip_id |= (uint32_t)((padded >> 20) - 1u) << 8;
    else
        cb->chip_id |= (uint32_t)(0x100u - (padded >> 28)) << 8;
    cb->chip_read = 0;
    cb->rom_mask = (uint32_t)(padded - 1u);
}

int cartbus_attach_save(cartbus_t *cb, save_type_t type)
{
    return save_configure(&cb->save, type);
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
        else
            b = 0x00; /* 21-B9z: melonDS PadToPowerOf2 对尾部用 0 填充 */
        v |= (uint32_t)b << (i * 8);
    }
    return v;
}

/* 锁存 CARD_COMMAND 并开始传输。
   只实现读命令 B7/B8（GetData）；其余命令（芯片 ID / KEY1 激活等）后续阶段补。 */
static void cartbus_activate(cartbus_t *cb)
{
    uint8_t c0 = cb->cmd[0];

    /* B8：读芯片 ID（直接返回 ChipID）；B7：读 ROM。
       B7 命令格式：B7 addr[31:24] addr[23:16] addr[15:8] addr[7:0] 00 00 00
       （命令字节大端：cmd[0] 是命令号，cmd[1..4] 是 32 位地址）。 */
    if (c0 == 0xB8) {
        cb->xfer_addr = 0;
        cb->xfer_remaining = 4;
        cb->chip_read = 1;
    } else if (c0 == 0xB7) {
        uint32_t addr = ((uint32_t)cb->cmd[1] << 24)
                      | ((uint32_t)cb->cmd[2] << 16)
                      | ((uint32_t)cb->cmd[3] << 8)
                      | ((uint32_t)cb->cmd[4]);
        cb->xfer_addr = addr & cb->rom_mask;
        /* melonDS CartCommon：B7 若请求 <0x8000，重定向到 0x8000+(低 9 位) */
        if (cb->xfer_addr < 0x8000u)
            cb->xfer_addr = 0x8000u + (cb->xfer_addr & 0x1FFu);
        cb->chip_read = 0;
        uint32_t blk = (cb->romctrl & CART_ROMCTRL_BLOCK_MASK) >> 24;
        uint32_t size;
        if (blk == 7)      size = 4u;              /* 4 字节（KEY2 区用） */
        else if (blk == 0) size = 0u;              /* None：无数据 */
        else               size = 0x100u << blk;   /* melonDS: 1=0x200 … 6=0x4000 */
        cb->xfer_remaining = size;
    } else {
        /* 未支持的命令：不产生数据，但仍置就绪，读 CARD_DATA 会得 0xFFFFFFFF。 */
        cb->xfer_addr = 0;
        cb->xfer_remaining = 0;
        cb->chip_read = 0;
    }

    /* 瞬时传输模型：命令一锁存，块内数据立即全部就绪（DRQ=1），
       bit31 保持“传输忙”直到块被读完，最后一块读完才回落 0。
       FFXII 用 CPU 轮询 ROMCTRL：bit23=1 时读 CARD_DATA，bit31=1 期间循环。 */
    if (cb->xfer_remaining > 0) {
        cb->romctrl |= CART_ROMCTRL_DRQ;
        cb->romctrl |= CART_ROMCTRL_ACTIVATE; /* bit31：传输中/忙 */
    } else {
        cb->romctrl &= ~(CART_ROMCTRL_DRQ | CART_ROMCTRL_ACTIVATE);
    }
}

/* 21-B9zb: B7/B8 改为按 melonDS 周期时序就绪；覆盖上面的瞬时 DRQ */
static void cartbus_schedule_delay(cartbus_t *cb)
{
    if (cb->xfer_remaining == 0) {
        cb->wait_phase = 0;
        cb->wait_cycles = 0;
        cb->romctrl &= ~CART_ROMCTRL_DRQ;
        return;
    }
    uint32_t blk2 = (cb->romctrl & CART_ROMCTRL_BLOCK_MASK) >> 24;
    uint32_t xfercycle = (cb->romctrl & (1u << 27)) ? 8u : 5u;
    uint32_t cmddelay = 8u + (cb->romctrl & 0x1FFFu);
    if (blk2 != 0u)
        cmddelay += ((cb->romctrl >> 16) & 0x3Fu);
    cb->romctrl &= ~CART_ROMCTRL_DRQ;
    cb->wait_cycles = xfercycle * (cmddelay + 4u);
    cb->wait_phase  = 1;
    cb->xfer_pos    = 0;
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
        uint16_t old = cb->auxspicnt;
        cb->auxspicnt = (uint16_t)((cb->auxspicnt & ~(0xFFu << shift))
                                   | ((uint32_t)val << shift));
        /* 撤下存档芯片片选（bit13/bit15 任一清零）时复位其命令状态机 */
        int old_cs = (old & AUXSPICNT_CS) == AUXSPICNT_CS;
        int new_cs = (cb->auxspicnt & AUXSPICNT_CS) == AUXSPICNT_CS;
        if (old_cs && !new_cs)
            save_reset_cmd(&cb->save);
        return;
    }
    if (addr >= CART_AUXSPIDATA && addr < CART_AUXSPIDATA + 2) {
        if (addr == CART_AUXSPIDATA) {
            /* 写低字节 = 启动一次 SPI 传输（MOSI=val，收 MISO 存回低字节）。
               仅当片选选中（bit13+bit15）时真正送进存档芯片。 */
            int cs = (cb->auxspicnt & AUXSPICNT_CS) == AUXSPICNT_CS;
            if (cs) {
                uint8_t in = save_transfer(&cb->save, val);
                cb->auxspidata = (uint16_t)((cb->auxspidata & 0xFF00u) | in);
                /* 不保持片选（bit6=0）：本字节传完即撤片选 → 命令结束 */
                if (!(cb->auxspicnt & AUXSPICNT_HOLD))
                    save_reset_cmd(&cb->save);
            }
        } else {
            /* 高字节（0x040001A3）：AUXSPIDATA 实为 8 位，忽略 */
            cb->auxspidata = (uint16_t)((cb->auxspidata & 0x00FFu)
                                        | ((uint32_t)val << 8));
        }
        return;
    }
    if (addr >= CART_ROMCTRL && addr < CART_ROMCTRL + 4) {
        uint32_t shift = (addr - CART_ROMCTRL) * 8;
        cb->romctrl = (cb->romctrl & ~(0xFFu << shift)) | ((uint32_t)val << shift);
        /* 写 bit31（最高字节的最高位）= 启动传输：锁存命令并置 DRQ */
        if (addr == CART_ROMCTRL + 3 && (val & 0x80u)) {
            cartbus_activate(cb);
            cartbus_schedule_delay(cb);
        }
        return;
    }
    if (addr >= CART_COMMAND && addr < CART_COMMAND + 8) {
        cb->cmd[addr - CART_COMMAND] = val;
        return;
    }
}

/* 21-B9zb: 推进 N 个 ARM9 周期；数据从等待变为就绪时返回 1 */
int cartbus_advance(cartbus_t *cb, uint32_t cycles)
{
    if (cb->wait_phase == 1) {
        if (cycles >= cb->wait_cycles) {
            cb->wait_cycles = 0;
            cb->wait_phase  = 2;
            cb->romctrl |= CART_ROMCTRL_DRQ;
            return 1;
        }
        cb->wait_cycles -= cycles;
    }
    return 0;
}

uint32_t cartbus_read32(cartbus_t *cb)
{
    if (cb->rom == NULL) {
        cb->romctrl &= ~CART_ROMCTRL_DRQ;
        cb->romctrl &= ~CART_ROMCTRL_ACTIVATE;
        return 0xFFFFFFFFu;
    }
    if (cb->xfer_remaining == 0) {
        cb->romctrl &= ~CART_ROMCTRL_DRQ;
        cb->romctrl &= ~CART_ROMCTRL_ACTIVATE;
        return 0xFFFFFFFFu;
    }
    if (cb->wait_phase != 2) {
        /* 21-B9zb: 数据还没就绪时读数据端口不算消费 */
        return 0xFFFFFFFFu;
    }

    uint32_t v;
    if (cb->chip_read)
        v = cb->chip_id;
    else
        v = rom_le32(cb->rom, cb->rom_size, cb->xfer_addr);
    cb->xfer_addr += 4;
    cb->xfer_remaining = (cb->xfer_remaining > 4) ? cb->xfer_remaining - 4 : 0;
    if (cb->xfer_remaining == 0)
        cb->chip_read = 0;
    cb->xfer_pos += 4;
    if (cb->xfer_remaining > 0) {
        /* 21-B9zb: 读走一个字后清 DRQ，按逐字/0x200 块边界排下一次就绪 */
        cb->romctrl &= ~CART_ROMCTRL_DRQ;
        cb->wait_phase = 1;
        uint32_t xfercycle = (cb->romctrl & (1u << 27)) ? 8u : 5u;
        uint32_t delay = xfercycle * 4u;
        if ((cb->xfer_pos & 0x1FFu) == 0u)
            delay += xfercycle * ((cb->romctrl >> 16) & 0x3Fu);
        cb->wait_cycles = delay;
    } else {
        cb->wait_phase  = 0;
        cb->wait_cycles = 0;
    }
    if (cb->xfer_remaining == 0) {
        cb->romctrl &= ~CART_ROMCTRL_DRQ; /* 本块读完，清就绪 */
        cb->romctrl &= ~CART_ROMCTRL_ACTIVATE; /* 传输结束，bit31 回落 0 */
    }
    return v;
}
