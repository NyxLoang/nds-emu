#include <stdint.h>
#include "power.h"

void power_reset(power_t *p)
{
    p->post[0] = 0x01;
    p->post[1] = 0x01;
    p->pow[0] = 0x820Fu;   /* 直接启动：LCD/2D/3D/EngineB/换屏全部打开 */
    p->pow[1] = 0x0001u;   /* 喇叭开，Wi-Fi 关 */
    p->wifiwait = 0x0030u; /* 固件写好的 Wi-Fi 时序 */
}

int power_is_addr(uint32_t addr)
{
    return (addr >= IO_POWER_POSTFLG && addr < IO_POWER_END)
        || (addr >= IO_POWER_WIFIWAIT && addr < IO_POWER_WIFIWAIT + 2);
}

uint8_t power_read8(const power_t *p, uint32_t addr, int is_arm7)
{
    int idx = is_arm7 ? 1 : 0;
    if (addr == IO_POWER_POSTFLG)
        return p->post[idx];
    if (addr == IO_POWER_POWCNT)
        return (uint8_t)(p->pow[idx] & 0xFFu);
    if (addr == IO_POWER_POWCNT + 1)
        return (uint8_t)(p->pow[idx] >> 8);
    if (is_arm7 && addr == IO_POWER_WIFIWAIT)
        return (uint8_t)(p->wifiwait & 0xFFu);
    if (is_arm7 && addr == IO_POWER_WIFIWAIT + 1)
        return (uint8_t)(p->wifiwait >> 8);
    return 0;
}

void power_write8(power_t *p, uint32_t addr, uint8_t val, int is_arm7)
{
    int idx = is_arm7 ? 1 : 0;
    if (addr == IO_POWER_POSTFLG) {
        /* bit0 只能置 1 不能清（启动完成标志）；ARM7 的 bit1 恒 0 */
        uint8_t mask = is_arm7 ? 0x01u : 0x03u;
        if (p->post[idx] & 0x01u)
            val |= 0x01u;
        p->post[idx] = (uint8_t)(val & mask);
        return;
    }
    if (addr == IO_POWER_POWCNT) {
        uint8_t mask = (uint8_t)(is_arm7 ? POWER2_WRITE_MASK
                                         : POWER1_WRITE_MASK);
        p->pow[idx] = (uint16_t)((p->pow[idx] & 0xFF00u) | (val & mask));
        return;
    }
    if (addr == IO_POWER_POWCNT + 1) {
        uint8_t mask = (uint8_t)(is_arm7 ? (POWER2_WRITE_MASK >> 8)
                                         : (POWER1_WRITE_MASK >> 8));
        p->pow[idx] = (uint16_t)((p->pow[idx] & 0x00FFu)
                                 | ((uint16_t)(val & mask) << 8));
        return;
    }
    if (is_arm7 && addr == IO_POWER_WIFIWAIT) {
        p->wifiwait = (uint16_t)((p->wifiwait & 0xFF00u) | (val & 0x3Fu));
        return;
    }
    if (is_arm7 && addr == IO_POWER_WIFIWAIT + 1) {
        /* WIFIWAITCNT 只有 bit0-5；高字节写入忽略 */
        return;
    }
}
