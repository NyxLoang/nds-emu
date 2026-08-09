#ifndef NDS_EMU_IO_IRQ_H
#define NDS_EMU_IO_IRQ_H

#include <stdint.h>

/* 中断寄存器地址（ARM9；ARM7 在 0x040003xx，本模拟器暂只实现 ARM9） */
#define IO_IME_ADDR 0x04000208u   /* 总开关：bit0=1 才允许中断 */
#define IO_IE_ADDR  0x04000210u   /* 各中断源使能位 */
#define IO_IF_ADDR  0x04000214u   /* 挂起位：硬件置 1，软件写 1 清除 */
#define IO_IRQ_END  0x04000218u   /* 中断区上界（不含），用于区间判断 */

/* IF 里的 VBlank 位（bit3）。其余中断源阶段 6 不产生，先不定义。 */
#define IO_IF_VBLANK (1u << 3)

/* 中断控制器状态：三个 32 位寄存器。
   真机 IME/IE/IF 多为 32 位寄存器，这里按整字存储，按字节访问。 */
typedef struct irq {
    uint32_t ime;  /* 总开关（Interrupt Master Enable） */
    uint32_t ie;   /* 使能（Interrupt Enable） */
    uint32_t ifl;  /* 挂起（Interrupt Flag） */
} irq_t;

int irq_is_addr(uint32_t addr);
uint8_t irq_read8(const irq_t *irq, uint32_t addr);
void irq_write8(irq_t *irq, uint32_t addr, uint8_t val);

/* 硬件信号：一帧结束 → 把 VBlank 位挂起（主循环每帧调用） */
void irq_set_vblank(irq_t *irq);

/* 是否真的会发生中断：IF 有挂起 且 IE 使能 且 IME 总开关打开 */
int irq_pending(const irq_t *irq);

#endif /* NDS_EMU_IO_IRQ_H */
