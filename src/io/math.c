#include <stdint.h>
#include "math.h"

int math_is_addr(uint32_t addr)
{
    return addr >= IO_MATH_BASE && addr < IO_MATH_END;
}

/* 64 位寄存器的某个字节读（low/high 各 4 字节小端）。 */
static uint8_t math_read64(const uint32_t w[2], uint32_t addr)
{
    uint32_t local = (addr & 7u);       /* 相对 8 字节对齐基址的偏移 */
    uint32_t v = w[local < 4u ? 0 : 1];
    return (uint8_t)(v >> ((local & 3u) * 8));
}

/* 64 位寄存器的某个字节写（同上）。 */
static void math_write64(uint32_t w[2], uint32_t addr, uint8_t val)
{
    uint32_t local = (addr & 7u);
    uint32_t shift = (local & 3u) * 8;
    if (local < 4u)
        w[0] = (w[0] & ~(0xFFu << shift)) | ((uint32_t)val << shift);
    else
        w[1] = (w[1] & ~(0xFFu << shift)) | ((uint32_t)val << shift);
}

static uint64_t math_get64(const uint32_t w[2])
{
    return (uint64_t)w[0] | ((uint64_t)w[1] << 32);
}

static void math_set64(uint32_t w[2], int64_t v)
{
    uint64_t u = (uint64_t)v;
    w[0] = (uint32_t)u;
    w[1] = (uint32_t)(u >> 32);
}

/* 按 DIVCNT 模式立即完成一次除法。
   语义参考 melonDS/真机：模式 0=32/32、1=64/32（含保留模式 3）、2=64/64；
   除零时商=±1、余数=被除数；溢出 -MAX/-1 按硬件钳制。 */
static void math_div_go(math_t *m)
{
    uint16_t mode = m->divcnt & MATH_DIV_MODE_MASK;

    m->divcnt &= (uint16_t)~(MATH_DIV0 | MATH_DIV_BUSY);

    if (mode == 0) {
        int32_t num = (int32_t)m->div_num[0];
        int32_t den = (int32_t)m->div_den[0];
        if (den == 0) {
            m->div_quot[0] = (num < 0) ? 1u : 0xFFFFFFFFu;
            m->div_quot[1] = (num < 0) ? 0xFFFFFFFFu : 0u;
            math_set64(m->div_rem, num);
        } else if (num == INT32_MIN && den == -1) {
            /* 32 位模式溢出：低字 80000000h，高字 0（硬件不更新余数） */
            m->div_quot[0] = 0x80000000u;
            m->div_quot[1] = 0u;
        } else {
            math_set64(m->div_quot, num / den);
            math_set64(m->div_rem, num % den);
        }
    } else if (mode == 1 || mode == 3) {
        int64_t num = (int64_t)math_get64(m->div_num);
        int32_t den = (int32_t)m->div_den[0];
        if (den == 0) {
            math_set64(m->div_quot, (num < 0) ? 1 : -1);
            math_set64(m->div_rem, num);
        } else if (num == INT64_MIN && den == -1) {
            math_set64(m->div_quot, INT64_MIN);
            math_set64(m->div_rem, 0);
        } else {
            math_set64(m->div_quot, num / den);
            math_set64(m->div_rem, num % den);
        }
    } else { /* mode == 2 */
        int64_t num = (int64_t)math_get64(m->div_num);
        int64_t den = (int64_t)math_get64(m->div_den);
        if (den == 0) {
            math_set64(m->div_quot, (num < 0) ? 1 : -1);
            math_set64(m->div_rem, num);
        } else if (num == INT64_MIN && den == -1) {
            math_set64(m->div_quot, INT64_MIN);
            math_set64(m->div_rem, 0);
        } else {
            math_set64(m->div_quot, num / den);
            math_set64(m->div_rem, num % den);
        }
    }

    /* 除零标志看完整 64 位除数（即使模式只取低 32 位） */
    if ((m->div_den[0] | m->div_den[1]) == 0)
        m->divcnt |= MATH_DIV0;
}

/* 按 SQRTCNT 模式立即完成一次开方（返回整数平方根）。 */
static void math_sqrt_go(math_t *m)
{
    uint64_t val;
    uint64_t rem = 0;
    uint32_t res = 0;
    uint32_t nbits, topshift;

    m->sqrtcnt &= (uint16_t)~MATH_SQRT_BUSY;

    if (m->sqrtcnt & MATH_SQRT_MODE) {
        val = math_get64(m->sqrt_val);
        nbits = 32;
        topshift = 62;
    } else {
        val = m->sqrt_val[0];           /* 32 位输入只取低字 */
        nbits = 16;
        topshift = 30;
    }

    for (uint32_t i = 0; i < nbits; i++) {
        rem = (rem << 2) + ((val >> topshift) & 3u);
        val <<= 2;
        res <<= 1;
        uint32_t prod = (res << 1) + 1;
        if (rem >= prod) {
            rem -= prod;
            res++;
        }
    }
    m->sqrt_res = res;
}

uint8_t math_read8(const math_t *m, uint32_t addr)
{
    if (addr == MATH_DIVCNT)
        return (uint8_t)(m->divcnt & 0xFFu);
    if (addr == MATH_DIVCNT + 1)
        return (uint8_t)(m->divcnt >> 8);
    if (addr >= MATH_DIV_NUMER && addr < MATH_DIV_NUMER + 8)
        return math_read64(m->div_num, addr);
    if (addr >= MATH_DIV_DENOM && addr < MATH_DIV_DENOM + 8)
        return math_read64(m->div_den, addr);
    if (addr >= MATH_DIV_RESULT && addr < MATH_DIV_RESULT + 8)
        return math_read64(m->div_quot, addr);
    if (addr >= MATH_DIV_REM && addr < MATH_DIV_REM + 8)
        return math_read64(m->div_rem, addr);
    if (addr == MATH_SQRTCNT)
        return (uint8_t)(m->sqrtcnt & 0xFFu);
    if (addr == MATH_SQRTCNT + 1)
        return (uint8_t)(m->sqrtcnt >> 8);
    if (addr >= MATH_SQRT_RESULT && addr < MATH_SQRT_RESULT + 4) {
        uint32_t shift = (addr - MATH_SQRT_RESULT) * 8;
        return (uint8_t)(m->sqrt_res >> shift);
    }
    if (addr >= MATH_SQRT_PARAM && addr < MATH_SQRT_PARAM + 8)
        return math_read64(m->sqrt_val, addr);
    return 0;
}

void math_write8(math_t *m, uint32_t addr, uint8_t val)
{
    if (addr == MATH_DIVCNT) {
        m->divcnt = (uint16_t)((m->divcnt & 0xFF00u) | val);
        math_div_go(m);             /* 写控制也触发一次计算 */
        return;
    }
    if (addr >= MATH_DIV_NUMER && addr < MATH_DIV_NUMER + 8) {
        math_write64(m->div_num, addr, val);
        math_div_go(m);
        return;
    }
    if (addr >= MATH_DIV_DENOM && addr < MATH_DIV_DENOM + 8) {
        math_write64(m->div_den, addr, val);
        math_div_go(m);
        return;
    }
    if (addr == MATH_SQRTCNT) {
        m->sqrtcnt = (uint16_t)((m->sqrtcnt & 0xFF00u) | val);
        math_sqrt_go(m);
        return;
    }
    if (addr >= MATH_SQRT_PARAM && addr < MATH_SQRT_PARAM + 8) {
        math_write64(m->sqrt_val, addr, val);
        math_sqrt_go(m);
        return;
    }
    /* DIVCNT 高字节、DIV_RESULT/REM、SQRTCNT 高字节、SQRT_RESULT：只读，忽略写 */
}
