#include "irq.h"

/* 判断地址是否落在中断寄存器区（IME/IE/IF 三个寄存器范围） */
int irq_is_addr(uint32_t addr)
{
    return addr >= IO_IME_ADDR && addr < IO_IRQ_END;
}

/* 取 32 位寄存器的某个字节（0=最低字节）。用 (addr - 基址)*8 定位字节偏移。 */
static uint32_t byte_of(uint32_t reg, uint32_t addr, uint32_t base)
{
    return (reg >> ((addr - base) * 8)) & 0xFFu;
}

uint8_t irq_read8(const irq_t *irq, uint32_t addr)
{
    if (addr >= IO_IME_ADDR && addr < IO_IME_ADDR + 4)
        return (uint8_t)byte_of(irq->ime, addr, IO_IME_ADDR);
    if (addr >= IO_IE_ADDR && addr < IO_IE_ADDR + 4)
        return (uint8_t)byte_of(irq->ie, addr, IO_IE_ADDR);
    if (addr >= IO_IF_ADDR && addr < IO_IF_ADDR + 4)
        return (uint8_t)byte_of(irq->ifl, addr, IO_IF_ADDR);
    return 0;
}

/* 写一个字节到 32 位寄存器里对应的 8 位位置（小端：基址=最低字节）。 */
static void write_byte(uint32_t *reg, uint32_t addr, uint32_t base, uint8_t val)
{
    uint32_t shift = (addr - base) * 8;
    uint32_t mask  = 0xFFu << shift;
    *reg = (*reg & ~mask) | ((uint32_t)val << shift);
}

void irq_write8(irq_t *irq, uint32_t addr, uint8_t val)
{
    if (addr >= IO_IME_ADDR && addr < IO_IME_ADDR + 4) {
        write_byte(&irq->ime, addr, IO_IME_ADDR, val);
        return;
    }
    if (addr >= IO_IE_ADDR && addr < IO_IE_ADDR + 4) {
        write_byte(&irq->ie, addr, IO_IE_ADDR, val);
        return;
    }
    if (addr >= IO_IF_ADDR && addr < IO_IF_ADDR + 4) {
        /* IF 写 1 清除：往该位写 1 → 该位被清 0；写 0 的位不受影响。
           这是硬件寄存器「写 1 清 0」语义，不是直接回写。 */
        irq->ifl &= ~((uint32_t)val << ((addr - IO_IF_ADDR) * 8));
        return;
    }
}

void irq_set_vblank(irq_t *irq)
{
    irq->ifl |= IO_IF_VBLANK;
}

void irq_set_card(irq_t *irq)
{
    irq->ifl |= IO_IF_CARD_DONE;
}

int irq_pending(const irq_t *irq)
{
    /* 三者缺一不可：有挂起 + 使能 + 总开关打开 */
    return (irq->ifl & irq->ie) != 0 && (irq->ime & 1u) != 0;
}
