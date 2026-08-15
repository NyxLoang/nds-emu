#include <stdlib.h>
#include "io.h"
#include "bus/bus.h"

io_t *io_create(void)
{
    io_t *io = calloc(1, sizeof(io_t));
    if (io != NULL) {
        cartbus_init(&io->cartbus);
        gx_reset(&io->gx); /* 矩阵置单位阵 + 视口默认 + GXSTAT 置 FIFO 空 */
    }
    return io;
}

void io_destroy(io_t *io)
{
    if (io != NULL)
        cartbus_destroy(&io->cartbus);
    free(io);
}

/* 读 FIFO CNT 的某个字节（16 位寄存器，0=低字节） */
static uint8_t fifo_cnt_read8(ipc_fifo_t *f, uint32_t addr, int is_arm7)
{
    uint16_t cnt = fifo_cnt_read(f, is_arm7);
    if (addr == IO_FIFO_CNT)
        return (uint8_t)(cnt & 0xFFu);
    return (uint8_t)(cnt >> 8);
}

/* 写 FIFO CNT 的某个字节（16 位寄存器） */
static void fifo_cnt_write8(ipc_fifo_t *f, uint32_t addr, uint8_t val, int is_arm7)
{
    uint16_t cnt = fifo_cnt_read(f, is_arm7);
    if (addr == IO_FIFO_CNT)
        cnt = (uint16_t)((cnt & 0xFF00u) | val);
    else
        cnt = (uint16_t)((cnt & 0x00FFu) | ((uint16_t)val << 8));
    fifo_cnt_write(f, is_arm7, cnt);
}

/* FIFO 状态变化会同时影响两核的 IF（一方发送 → 另一方接收非空），
   故任何 FIFO 操作后对两核都做一次中断边沿检测。 */
static void io_fifo_update_irq_all(io_t *io)
{
    fifo_update_irq(&io->fifo, 0, &io->irq[0]); /* ARM9 视角 */
    fifo_update_irq(&io->fifo, 1, &io->irq[1]); /* ARM7 视角 */
}

/* 卡带就绪时触发等待卡带数据的 DMA（start mode = card）。
   幂等：dma_fire 只搬已使能且模式匹配的通道，搬完自动清使能。 */
static void io_card_dma_check(io_t *io)
{
    if (cartbus_ready(&io->cartbus))
        dma_fire(&io->dma, io->bus, DMA_START_CARD);
}

/* 按地址分发到对应功能文件。未实现的寄存器地址：读 0、写忽略。 */
uint8_t io_read8(const io_t *io, uint32_t addr, int is_arm7)
{
    if (irq_is_addr(addr))
        return irq_read8(&io->irq[is_arm7 ? 1 : 0], addr);
    if (fifo_is_cnt_addr(addr))
        return fifo_cnt_read8((ipc_fifo_t *)&io->fifo, addr, is_arm7);
    if (addr >= IO_TIMER0_BASE && addr < IO_TIMER_END)
        return timer_read8(&io->timer[(addr - IO_TIMER0_BASE) / IO_TIMER_STRIDE], addr);
    if (addr >= IO_KEYINPUT_ADDR && addr < IO_KEYINPUT_END)
        return key_read8(&io->keypad, addr);
    if (dma_is_addr(addr))
        return dma_read8(&io->dma, addr);
    if (cartbus_is_addr(addr))
        return cartbus_read8((cartbus_t *)&io->cartbus, addr);
    if (disp_is_addr(addr))
        return disp_read8(&io->disp, addr);
    if (touch_is_addr(addr))
        return touch_read8((touch_t *)&io->touch, addr);
    if (gx_is_addr(addr) && !is_arm7)
        return gx_read8(&io->gx, addr);
    if (snd_is_addr(addr) && is_arm7)
        return snd_read8(&io->snd, addr);
    return 0;
}

void io_write8(io_t *io, uint32_t addr, uint8_t val, int is_arm7)
{
    if (irq_is_addr(addr)) {
        irq_write8(&io->irq[is_arm7 ? 1 : 0], addr, val);
        return;
    }
    if (fifo_is_cnt_addr(addr)) {
        fifo_cnt_write8(&io->fifo, addr, val, is_arm7);
        io_fifo_update_irq_all(io);
        return;
    }
    if (fifo_is_send_addr(addr)) {
        /* SEND 是 32 位寄存器，字节写语义未定义，忽略（32 位写走 io_send32） */
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
    if (dma_is_addr(addr)) {
        /* 写 CNT_H 且使能=1 时在 dma_write8 内同步触发立即搬运；
           若卡带已就绪且本条是卡带触发源，则在写完后补触发。 */
        dma_write8(&io->dma, addr, val, io->bus);
        io_card_dma_check(io);
        return;
    }
    if (cartbus_is_addr(addr)) {
        int was_ready = cartbus_ready(&io->cartbus);
        cartbus_write8(&io->cartbus, addr, val);
        /* 命令刚被激活（卡带由 Busy 变 Ready）：置卡带完成中断并触发卡带 DMA */
        if (!was_ready && cartbus_ready(&io->cartbus)) {
            irq_set_card(&io->irq[0]);
            irq_set_card(&io->irq[1]);
            io_card_dma_check(io);
        }
        return;
    }
    if (disp_is_addr(addr)) {
        disp_write8(&io->disp, addr, val);
        return;
    }
    if (touch_is_addr(addr)) {
        touch_write8(&io->touch, addr, val);
        return;
    }
    if (gx_is_addr(addr) && !is_arm7) {
        gx_write8(&io->gx, addr, val);
        return;
    }
    if (snd_is_addr(addr) && is_arm7) {
        snd_write8(&io->snd, addr, val);
        return;
    }
    /* 其余 IO 地址：写忽略（沿用阶段 2 的桩语义） */
}

/* FIFO 32 位收发（bus 对 0x04000188 写 / 0x04100000 读整体转发） */
uint32_t io_recv32(io_t *io, int is_arm7)
{
    uint32_t v = fifo_recv(&io->fifo, is_arm7);
    io_fifo_update_irq_all(io);
    return v;
}

void io_send32(io_t *io, int is_arm7, uint32_t val)
{
    fifo_send(&io->fifo, is_arm7, val);
    io_fifo_update_irq_all(io);
}

void io_gx_write32(io_t *io, uint32_t addr, uint32_t val)
{
    gx_write32(&io->gx, addr, val);
}

uint32_t io_card_data_read32(io_t *io)
{
    return cartbus_read32(&io->cartbus);
}

void io_card_data_write32(io_t *io, uint32_t val)
{
    (void)val; /* 读 ROM 用不到写端口；EEPROM 等写路径留待后续阶段 */
}

void io_attach_cart(io_t *io, const uint8_t *rom, size_t rom_size)
{
    cartbus_attach(&io->cartbus, rom, rom_size);
}

void io_attach_save(io_t *io, save_type_t type)
{
    cartbus_attach_save(&io->cartbus, type);
}

save_t *io_get_save(io_t *io)
{
    return &io->cartbus.save;
}

void io_set_vblank(io_t *io)
{
    irq_set_vblank(&io->irq[0]); /* VBlank 是 ARM9 显示事件 */
    dma_fire(&io->dma, io->bus, DMA_START_VBLANK); /* 阶段 15：触发 VBlank DMA */
}

int io_irq_pending(const io_t *io)
{
    return irq_pending(&io->irq[0]); /* 主循环关心 ARM9 是否发生中断 */
}

void io_set_keyinput(io_t *io, uint16_t pressed)
{
    key_set_pressed(&io->keypad, pressed);
}

void io_set_touch(io_t *io, uint16_t adc_x, uint16_t adc_y, int down)
{
    touch_set_pos(&io->touch, adc_x, adc_y, down);
}

void io_advance_timers(io_t *io)
{
    for (int i = 0; i < IO_TIMER_COUNT; i++)
        timer_advance(&io->timer[i]);
}
