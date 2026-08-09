#include <stdlib.h>
#include "io.h"

io_t *io_create(void)
{
    return calloc(1, sizeof(io_t));
}

void io_destroy(io_t *io)
{
    free(io);
}

/* 按地址分发到对应功能文件。未实现的寄存器地址：读 0、写忽略。 */
uint8_t io_read8(const io_t *io, uint32_t addr)
{
    if (irq_is_addr(addr))
        return irq_read8(&io->irq, addr);
    if (addr >= IO_TIMER0_BASE && addr < IO_TIMER_END)
        return timer_read8(&io->timer[(addr - IO_TIMER0_BASE) / IO_TIMER_STRIDE], addr);
    if (addr >= IO_KEYINPUT_ADDR && addr < IO_KEYINPUT_END)
        return key_read8(&io->keypad, addr);
    return 0;
}

void io_write8(io_t *io, uint32_t addr, uint8_t val)
{
    if (irq_is_addr(addr)) {
        irq_write8(&io->irq, addr, val);
        return;
    }
    if (addr >= IO_TIMER0_BASE && addr < IO_TIMER_END) {
        timer_write8(&io->timer[(addr - IO_TIMER0_BASE) / IO_TIMER_STRIDE], addr, val);
        return;
    }
    if (addr >= IO_KEYINPUT_ADDR && addr < IO_KEYINPUT_END) {
        key_write8(&io->keypad, addr, val);
        return;
    }
    /* 其余 IO 地址：写忽略（沿用阶段 2 的桩语义） */
}

void io_set_vblank(io_t *io)
{
    irq_set_vblank(&io->irq);
}

int io_irq_pending(const io_t *io)
{
    return irq_pending(&io->irq);
}

void io_set_keyinput(io_t *io, uint16_t pressed)
{
    key_set_pressed(&io->keypad, pressed);
}

void io_advance_timers(io_t *io)
{
    for (int i = 0; i < IO_TIMER_COUNT; i++)
        timer_advance(&io->timer[i]);
}
