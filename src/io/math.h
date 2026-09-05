#ifndef NDS_EMU_IO_MATH_H
#define NDS_EMU_IO_MATH_H

#include <stdint.h>

/* 21-B9k：ARM9 硬件除法/开方（0x04000280 起）。
   DIVCNT(0x280,16 位) + DIV_NUMER/DENOM(64 位) + DIV_RESULT/REM(只读 64 位)
   SQRTCNT(0x2B0,16 位) + SQRT_RESULT(0x2B4,32 位只读) + SQRT_PARAM(64 位)。
   写 DIVCNT/SQRTCNT/操作数都会立即开始一次计算；本项目延续“瞬时模型”，
   计算瞬间完成，Busy(bit15) 读回恒 0。寄存器只有 ARM9 侧。 */

#define IO_MATH_BASE   0x04000280u
#define IO_MATH_END    0x040002C0u   /* 上界（不含） */

#define MATH_DIVCNT      0x04000280u
#define MATH_DIV_NUMER   0x04000290u
#define MATH_DIV_DENOM   0x04000298u
#define MATH_DIV_RESULT  0x040002A0u
#define MATH_DIV_REM     0x040002A8u
#define MATH_SQRTCNT     0x040002B0u
#define MATH_SQRT_RESULT 0x040002B4u
#define MATH_SQRT_PARAM  0x040002B8u

/* DIVCNT 位 */
#define MATH_DIV_MODE_MASK 0x0003u
#define MATH_DIV0          (1u << 14) /* 除零错误（只读） */
#define MATH_DIV_BUSY      (1u << 15) /* 忙（只读，瞬时模型恒 0） */

/* SQRTCNT 位 */
#define MATH_SQRT_MODE     (1u << 0)  /* 0=32 位输入 1=64 位输入 */
#define MATH_SQRT_BUSY     (1u << 15) /* 忙（只读，瞬时模型恒 0） */

typedef struct math {
    uint16_t divcnt;        /* DIVCNT（低 2 位模式；只读位按结果更新） */
    uint32_t div_num[2];    /* 被除数：低字/高字 */
    uint32_t div_den[2];    /* 除数：低字/高字 */
    uint32_t div_quot[2];   /* 商（只读） */
    uint32_t div_rem[2];    /* 余数（只读） */
    uint16_t sqrtcnt;       /* SQRTCNT */
    uint32_t sqrt_res;      /* SQRT_RESULT（只读） */
    uint32_t sqrt_val[2];   /* SQRT_PARAM：低字/高字 */
} math_t;

int math_is_addr(uint32_t addr);
uint8_t math_read8(const math_t *m, uint32_t addr);
void math_write8(math_t *m, uint32_t addr, uint8_t val);

#endif /* NDS_EMU_IO_MATH_H */
