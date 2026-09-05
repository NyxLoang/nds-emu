#include "memctl.h"

void memctl_reset(memctl_t *mc)
{
    if (mc == NULL)
        return;
    mc->exmem[0] = IO_EXMEMCNT_INIT;
    mc->exmem[1] = IO_EXMEMCNT_INIT; /* 高位随后会随 ARM9 写同步，初值按直接启动 */
    mc->wramcnt  = IO_WRAMCNT_INIT;
}

int memctl_is_addr(uint32_t addr)
{
    return (addr >= IO_EXMEMCNT_ADDR && addr < IO_EXMEMCNT_END)
        || addr == IO_WRAMCNT_ARM7_ADDR
        || addr == IO_WRAMCNT_ARM9_ADDR;
}

/* 按半字语义写入 EXMEMCNT：
   ARM9 可写 bit15/11/7-0，bit13/14 只读保留，写后高 7 位（bit7-14）镜像给 ARM7；
   ARM7 只能写自己的低 7 位（bit0-6），高 7 位保持 ARM9 镜像。 */
static void memctl_write_exmem(memctl_t *mc, int is_arm7, uint16_t val)
{
    if (!is_arm7) {
        uint16_t old = mc->exmem[0];
        mc->exmem[0] = (uint16_t)((old & 0x6000u) | (val & 0x88FFu));
        mc->exmem[1] = (uint16_t)((mc->exmem[0] & 0xFF80u)
                                | (mc->exmem[1] & 0x007Fu));
    } else {
        mc->exmem[1] = (uint16_t)((mc->exmem[1] & 0xFF80u)
                                | (val & 0x007Fu));
    }
}

uint8_t memctl_read8(const memctl_t *mc, uint32_t addr, int is_arm7)
{
    if (addr >= IO_EXMEMCNT_ADDR && addr < IO_EXMEMCNT_END) {
        uint16_t v = mc->exmem[is_arm7 ? 1 : 0];
        return (addr & 1u) ? (uint8_t)(v >> 8) : (uint8_t)(v & 0xFFu);
    }
    /* WRAMCNT：ARM9 在 0x247 读，ARM7 在 0x241 读（ARM7 侧是只读视图） */
    if ((!is_arm7 && addr == IO_WRAMCNT_ARM9_ADDR)
     || (is_arm7 && addr == IO_WRAMCNT_ARM7_ADDR))
        return mc->wramcnt;
    return 0;
}

void memctl_write8(memctl_t *mc, uint32_t addr, uint8_t val, int is_arm7)
{
    if (addr >= IO_EXMEMCNT_ADDR && addr < IO_EXMEMCNT_END) {
        uint16_t cur = mc->exmem[is_arm7 ? 1 : 0];
        if (addr & 1u)
            cur = (uint16_t)((cur & 0x00FFu) | ((uint16_t)val << 8));
        else
            cur = (uint16_t)((cur & 0xFF00u) | val);
        memctl_write_exmem(mc, is_arm7, cur);
        return;
    }
    /* 只有 ARM9 的 0x04000247 是写口；ARM7 的 0x241 / ARM9 的 0x241 无写语义 */
    if (!is_arm7 && addr == IO_WRAMCNT_ARM9_ADDR)
        mc->wramcnt = val & 3u;
}
