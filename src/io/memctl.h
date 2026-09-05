#ifndef NDS_EMU_IO_MEMCTL_H
#define NDS_EMU_IO_MEMCTL_H

#include <stdint.h>

/* 内存控制寄存器组（阶段 21-B7，真实 ROM bring-up）。
   按 melonDS / GBATEK 口径实现两个寄存器：
   - EXMEMCNT（0x04000204，16 位）：每核一份可读值；
     ARM9 写后把自己的高 7 位（bit7-14）同步给 ARM7，ARM7 只能改自己低 7 位（bit0-6）。
   - WRAMCNT：ARM9 在 0x04000247 读写，ARM7 在 0x04000241 只读；低 2 位决定
     Shared WRAM（32KB）归谁：0=全归 ARM9，1=ARM9 高 16KB + ARM7 低 16KB，
     2=ARM9 低 16KB + ARM7 高 16KB，3=全归 ARM7。 */
#define IO_EXMEMCNT_ADDR      0x04000204u
#define IO_EXMEMCNT_END       0x04000206u
#define IO_WRAMCNT_ARM7_ADDR  0x04000241u
#define IO_WRAMCNT_ARM9_ADDR  0x04000247u

/* 直接启动（本项目无 BIOS 模拟，等价于 melonDS SetupDirectBoot）初始值：
   Shared WRAM 默认全给 ARM7；EXMEMCNT 高 7 位两核同源。 */
#define IO_EXMEMCNT_INIT      0xE880u
#define IO_WRAMCNT_INIT       3u

typedef struct memctl {
    uint16_t exmem[2]; /* EXMEMCNT 每核可见值：exmem[0]=ARM9、exmem[1]=ARM7 */
    uint8_t  wramcnt;  /* WRAMCNT：低 2 位有效，bus 据此切分 Shared WRAM */
} memctl_t;

void memctl_reset(memctl_t *mc);
int  memctl_is_addr(uint32_t addr);
uint8_t memctl_read8(const memctl_t *mc, uint32_t addr, int is_arm7);
void memctl_write8(memctl_t *mc, uint32_t addr, uint8_t val, int is_arm7);

#endif /* NDS_EMU_IO_MEMCTL_H */
