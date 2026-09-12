#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "cartbus.h"

void cartbus_init(cartbus_t *cb)
{
    memset(cb, 0, sizeof(*cb));
    save_init(&cb->save);
    /* 21-B9yi：未开始传输时数据端口读到的应是“空 FIFO”的残留值
       （真机/测试期望 0xFFFFFFFF） */
    cb->data[0] = cb->data[1] = 0xFFFFFFFFu;
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
static void cartbus_fetch_delay(cartbus_t *cb, int first); /* 前向声明 */
static void cartbus_end(cartbus_t *cb);

static void cartbus_activate(cartbus_t *cb)
{
    uint8_t c0 = cb->cmd[0];
    /* 21-B9yi 诊断：NDS_CARTLOG=1 → 打印每次启动传输的命令/地址/块长
       （与参考核 cartlog 的命令流对照，定位「START 之后卡死」） */
    static int g_cartlog_state, g_cartlog_n;
    if (g_cartlog_state == 0) {
        const char *e = getenv("NDS_CARTLOG");
        g_cartlog_state = (e != NULL && e[0] != '0' && e[0] != '\0') ? 1 : -1;
    }
    if (g_cartlog_state == 1 && g_cartlog_n < 20000) {
        g_cartlog_n++;
        printf("cartlog: #%d cmd=%02X%02X%02X%02X%02X%02X%02X%02X"
               " addr=%02X%02X%02X%02X romctrl=%08X\n",
               g_cartlog_n, cb->cmd[0], cb->cmd[1], cb->cmd[2], cb->cmd[3],
               cb->cmd[4], cb->cmd[5], cb->cmd[6], cb->cmd[7],
               cb->cmd[1], cb->cmd[2], cb->cmd[3], cb->cmd[4], cb->romctrl);
    }

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

    /* 21-B9yi：按 melonDS 口径——命令锁存后置 bit31，数据由卡带侧按周期
       **预取进 2 字 FIFO**（见 cartbus_advance），不再“一锁存就全就绪”。 */
    cb->xfer_len = cb->xfer_remaining;
    cb->xfer_pos = 0;
    cb->data_head = 0;
    cb->data_tail = 0;
    cb->data_count = 0;
    cb->data_late = 0;
    cb->data[0] = cb->data[1] = 0xFFFFFFFFu;
    if (cb->xfer_remaining > 0) {
        cb->romctrl |= CART_ROMCTRL_ACTIVATE; /* bit31：传输中/忙 */
        {
            extern unsigned long long g_dbg_frame;
            static long lo = -2, hi = -1;
            if (lo == -2) {
                const char *e = getenv("NDS_CARTLOG2");
                lo = 0; hi = -1;
                if (e != NULL && sscanf(e, "%ld-%ld", &lo, &hi) != 2) { lo = 0; hi = -1; }
            }
            if ((long)g_dbg_frame >= lo && (long)g_dbg_frame <= hi)
                printf("cartlog2: f=%llu START cmd=%02X%02X%02X%02X len=%u romctrl=%08X\n",
                       g_dbg_frame, cb->cmd[0], cb->cmd[1], cb->cmd[2], cb->cmd[3],
                       cb->xfer_len, cb->romctrl);
        }
        cartbus_fetch_delay(cb, 1);           /* 排第一次取数 */
    } else {
        cb->romctrl &= ~(CART_ROMCTRL_DRQ | CART_ROMCTRL_ACTIVATE);
        cb->wait_phase = 0;
        cb->wait_cycles = 0;
    }
}

/* 21-B9yi：排「取下 N 个字」的事件（对齐 melonDS 的 ScheduleEvent）：
   首次取数 = xfercycle * (cmddelay + 4)；后续每字 = xfercycle * (4 [+gap2])。 */
static void cartbus_fetch_delay(cartbus_t *cb, int first)
{
    uint32_t xfercycle = (cb->romctrl & (1u << 27)) ? 8u : 5u;
    uint32_t cycles;
    if (first) {
        uint32_t cmddelay = 8u + (cb->romctrl & 0x1FFFu);
        if (cb->xfer_len != 0u)
            cmddelay += ((cb->romctrl >> 16) & 0x3Fu);
        cycles = xfercycle * (cmddelay + 4u);
    } else {
        uint32_t delay = 4u;
        if ((cb->xfer_pos & 0x1FFu) == 0u)
            delay += ((cb->romctrl >> 16) & 0x3Fu);
        cycles = xfercycle * delay;
    }
    cb->wait_cycles = cycles;
    cb->wait_phase  = 1;
}

/* 传输结束：清忙位/DRQ，并在 AUXSPICNT bit14 使能时挂卡带完成中断。 */
static void cartbus_end(cartbus_t *cb)
{
    int irq_en = (cb->auxspicnt & (1u << 14)) != 0;
    {
        /* 21-B9yi 诊断：NDS_CARTLOG2=LO-HI → 该帧区间内打印传输开始/结束/读取 */
        extern unsigned long long g_dbg_frame;
        static long lo = -2, hi = -1;
        if (lo == -2) {
            const char *e = getenv("NDS_CARTLOG2");
            lo = 0; hi = -1;
            if (e != NULL && sscanf(e, "%ld-%ld", &lo, &hi) != 2) { lo = 0; hi = -1; }
        }
        if ((long)g_dbg_frame >= lo && (long)g_dbg_frame <= hi)
            printf("cartlog2: f=%llu END pos=%u len=%u cnt=%d romctrl=%08X\n",
                   g_dbg_frame, cb->xfer_pos, cb->xfer_len, cb->data_count,
                   cb->romctrl);
    }
    cb->romctrl &= ~(CART_ROMCTRL_DRQ | CART_ROMCTRL_ACTIVATE);
    cb->wait_phase = 0;
    cb->wait_cycles = 0;
    cb->data_count = 0;
    cb->data_late = 0;
    cb->xfer_pos = 0;
    cb->xfer_len = 0;
    cb->xfer_remaining = 0;
    cb->chip_read = 0;
    if (irq_en)
        cb->end_irq = 1;   /* 由 io_advance_cart 转成两核的卡带 IRQ */
}

uint8_t cartbus_read8(cartbus_t *cb, uint32_t addr)
{
    if (addr == CART_ROMCTRL + 3) {
        /* 21-B9yi 诊断：NDS_CARTLOG3=LO-HI → 游戏读 ROMCTRL 高字节（bit31/23） */
        extern unsigned long long g_dbg_frame;
        static long lo = -2, hi = -1;
        if (lo == -2) {
            const char *e = getenv("NDS_CARTLOG3");
            lo = 0; hi = -1;
            if (e != NULL && sscanf(e, "%ld-%ld", &lo, &hi) != 2) { lo = 0; hi = -1; }
        }
        if ((long)g_dbg_frame >= lo && (long)g_dbg_frame <= hi)
            printf("cartlog3: f=%llu RD hi=%02X full=%08X\n",
                   g_dbg_frame, (unsigned)(cb->romctrl >> 24), cb->romctrl);
    }
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
        uint32_t old = cb->romctrl;
        uint32_t wval = (uint32_t)val << shift;
        /* bit31/bit29/bit23 对软件只读（melonDS 写掩码 0xFF7F7FFF 且
           `(ROMCnt & (~mask | 0x20800000))` 强制这三位保持旧值）：
           bit31=传输忙、bit29=KEY2 状态、bit23=DRQ 都由硬件/本模块维护，
           写 1 只表示「请求启动」，不会把忙位本身写成 1。 */
        cb->romctrl = ((old & ~(0xFFu << shift)) | (wval & 0x7F7FFFFFu))
                    | (old & (CART_ROMCTRL_ACTIVATE | (1u << 29)
                              | CART_ROMCTRL_DRQ));
        /* 21-B9yi：只有 bit31 **由 0 变 1** 才启动新传输——melonDS
           `xferstart = (val & ~ROMCnt) & (1<<31)`；并且要求 AUXSPICNT
           使能（bit15=1）且处于 ROM 模式（bit13=0）。
           旧实现「写高字节带 bit7 就重启传输」会把上一笔还没读完的传输
           覆盖掉：游戏在 0x02011C50 循环等 bit31 清零时永远等不到，
           表现为 START 之后卡在加载画面（2020 帧实测两核都停在 IRQ 模式）。 */
        int was_busy = (old & CART_ROMCTRL_ACTIVATE) != 0;
        int xferstart = (wval & CART_ROMCTRL_ACTIVATE) != 0;
        int slot_ok = (cb->auxspicnt & AUXSPICNT_ENABLE) != 0
                   && (cb->auxspicnt & AUXSPICNT_SLOTMODE) == 0;
        if (!was_busy && xferstart && slot_ok) {
            cartbus_activate(cb);
            cartbus_fetch_delay(cb, 1);
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
    if (cb->wait_phase != 1)
        return 0;
    if (cycles < cb->wait_cycles) {
        cb->wait_cycles -= cycles;
        return 0;
    }
    cb->wait_cycles = 0;
    cb->wait_phase  = 0;
    /* 取一个字进 FIFO（melonDS ROMReceiveData） */
    {
        uint32_t v;
        if (cb->chip_read)
            v = cb->chip_id;
        else
            v = rom_le32(cb->rom, cb->rom_size, cb->xfer_addr);
        cb->xfer_addr += 4u;
        cb->data[cb->data_head] = v;
        cb->data_head ^= 1;
        cb->data_count++;
        cb->xfer_pos += 4u;
        cb->xfer_remaining = (cb->xfer_remaining > 4u)
                           ? cb->xfer_remaining - 4u : 0u;
        if (cb->xfer_pos >= cb->xfer_len)
            cb->chip_read = 0;
        cb->romctrl |= CART_ROMCTRL_DRQ;   /* 数据就绪（bit23） */
    }
    /* FIFO 还有空位且块内还有数据 → 继续预取；否则记 late（等 CPU 读走再排） */
    if (cb->xfer_pos < cb->xfer_len) {
        if (cb->data_count < 2)
            cartbus_fetch_delay(cb, 0);
        else
            cb->data_late = 1;
    }
    return 1;
}

uint32_t cartbus_read32(cartbus_t *cb)
{
    /* 21-B9yi：按 melonDS `ReadROMData` 的口径——从预取 FIFO 取一字；
       FIFO 空时返回上一次的字（真机是 FIFO 里残留的数据，不是 0xFFFFFFFF）。 */
    uint32_t ret = cb->data[cb->data_tail];

    if (cb->rom == NULL) {
        cartbus_end(cb);
        return 0xFFFFFFFFu;
    }

    if (cb->data_count > 0) {
        cb->data_tail ^= 1;
        cb->data_count--;
    }
    if (cb->data_count > 0)
        cb->romctrl |= CART_ROMCTRL_DRQ;
    else
        cb->romctrl &= ~CART_ROMCTRL_DRQ;

    {
        /* 21-B9yi 诊断：NDS_CARTLOG2=LO-HI → 打印本帧的每次数据端口读 */
        extern unsigned long long g_dbg_frame;
        static long lo = -2, hi = -1;
        if (lo == -2) {
            const char *e = getenv("NDS_CARTLOG2");
            lo = 0; hi = -1;
            if (e != NULL && sscanf(e, "%ld-%ld", &lo, &hi) != 2) { lo = 0; hi = -1; }
        }
        if ((long)g_dbg_frame >= lo && (long)g_dbg_frame <= hi)
            printf("cartlog2: f=%llu READ ret=%08X pos=%u len=%u cnt=%d romctrl=%08X\n",
                   g_dbg_frame, ret, cb->xfer_pos, cb->xfer_len, cb->data_count,
                   cb->romctrl);
    }

    if (cb->xfer_pos < cb->xfer_len) {
        /* FIFO 满时挂起的预取：CPU 读走后继续排 */
        if (cb->data_late && cb->wait_phase == 0) {
            cb->data_late = 0;
            cartbus_fetch_delay(cb, 0);
        }
    } else if (cb->data_count == 0) {
        cartbus_end(cb);   /* 块尾数据取完 → 传输结束，bit31 回落 0 */
    }
    return ret;
}
