#ifndef NDS_EMU_IO_POWER_H
#define NDS_EMU_IO_POWER_H

#include <stdint.h>

/* 21-B9n：系统电源/调试寄存器
   POSTFLG(0x04000300, 每核 1 字节)、POWCNT(0x04000304, ARM9=POWCNT1 /
   ARM7=POWCNT2, 16 位)、WIFIWAITCNT(0x04000206, ARM7, 16 位)。
   直接启动语义（melonDS）：POSTFLG 两核=1、POWCNT1=0x820F（LCD+2D+3D+
   EngineB+换屏）、POWCNT2=0x0001（喇叭开）、WIFIWAITCNT=0x0030。 */

#define IO_POWER_POSTFLG   0x04000300u
#define IO_POWER_POWCNT    0x04000304u
#define IO_POWER_END       0x04000308u

#define IO_POWER_WIFIWAIT  0x04000206u

/* POWCNT1 可写位：bit0 LCD、bit1 EngineA、bit2 3D 渲染、bit3 3D 几何、
   bit9 EngineB、bit15 上下屏交换 */
#define POWER1_WRITE_MASK 0x820Fu
/* POWCNT2 可写位：bit0 喇叭、bit1 Wi-Fi */
#define POWER2_WRITE_MASK 0x0003u

typedef struct power {
    uint8_t  post[2];      /* POSTFLG：post[0]=ARM9、post[1]=ARM7 */
    uint16_t pow[2];       /* POWCNT：pow[0]=ARM9(POWCNT1)、pow[1]=ARM7(POWCNT2) */
    uint16_t wifiwait;     /* WIFIWAITCNT（ARM7，固件初始化 0x0030） */
} power_t;

void power_reset(power_t *p);
int power_is_addr(uint32_t addr);
uint8_t power_read8(const power_t *p, uint32_t addr, int is_arm7);
void power_write8(power_t *p, uint32_t addr, uint8_t val, int is_arm7);

#endif /* NDS_EMU_IO_POWER_H */
