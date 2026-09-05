#include <stdint.h>
#include "bios_crc16.h"
#include "cpu/cpu.h"
#include "bus/bus.h"

/* SWI 0x0E GetCRC16（NDS9/NDS7 通用）：
   入：r0=CRC 初值（调用方常用 0xFFFF）
       r1=数据地址
       r2=数据字节数
   出：r0=计算后的 CRC16

   算法 = 标准 CRC-16/IBM（输入反射、结果反射，生成多项式 0x8005）：
   反射实现先把每字节异或进 CRC 低 8 位，再逐位右移；若移出位为 1，
   则异或反转多项式 0xA001（等价于生成多项式 0x8005 的位反转）。
   GBATEK 给出的逐位伪代码与此写法等价；末尾不做额外异或。 */
int bios_crc16(arm_cpu_t *cpu)
{
    bus_t *bus = cpu->nds->bus;
    uint16_t crc = (uint16_t)cpu->r[0];
    uint32_t src = cpu->r[1];
    uint32_t len = cpu->r[2];

    for (uint32_t i = 0; i < len; i++) {
        crc ^= bus_read8(bus, src + i);
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 1u)
                crc = (uint16_t)((crc >> 1) ^ 0xA001u);
            else
                crc = (uint16_t)(crc >> 1);
        }
    }

    cpu->r[0] = crc;
    return 1; /* BIOS_RET_HANDLED */
}
