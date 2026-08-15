#include <stdint.h>
#include "bios_arith.h"
#include "cpu/cpu.h"

/* SWI 0x09 Div：有符号除法 r0/r1。
   入：r0=被除数（有符号）、r1=除数（有符号）
   出：r0=商（有符号）、r1=余数（有符号）、r3=|商|（无符号）
   除零在真机是死循环（无定义结果），这里防御性返回 0/被除数，避免 C 的 UB。 */
int bios_div(arm_cpu_t *cpu)
{
    int32_t num = (int32_t)cpu->r[0];
    int32_t den = (int32_t)cpu->r[1];
    int32_t quot, rem;

    if (den == 0) {
        quot = 0;
        rem = num;
    } else {
        quot = num / den;
        rem = num % den;
    }

    cpu->r[0] = (uint32_t)quot;
    cpu->r[1] = (uint32_t)rem;
    cpu->r[3] = (uint32_t)(quot < 0 ? -quot : quot); /* |商|（无符号） */
    return 1;
}

/* SWI 0x0D Sqrt：无符号整数开方。
   入：r0=无符号 32 位；出：r0=无符号 16 位整数结果（向下取整）。
   逐位试商：结果最多 16 位，从最高位 0x8000 开始逐位确定。 */
int bios_sqrt(arm_cpu_t *cpu)
{
    uint32_t x = cpu->r[0];
    uint32_t res = 0;
    uint32_t add = 0x8000u;

    while (add != 0) {
        uint32_t tmp = res | add;
        /* tmp 最大 0xFFFF，其平方 0xFFFE0001 < 2^32，不会溢出 */
        if (x >= tmp * tmp)
            res = tmp;
        add >>= 1;
    }

    cpu->r[0] = res & 0xFFFFu;
    return 1;
}
