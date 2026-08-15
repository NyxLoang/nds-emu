#include <stddef.h>
#include "disp.h"

/* 显示寄存器区间判断：DISPCNT(4B) + BGxCNT(4×2B) + 滚动(4×4B)，主/副各一段。
   主引擎在 0x04000000 起，副引擎在 0x04001000 起，布局相同。 */
int disp_is_addr(uint32_t addr)
{
    if (addr >= IO_DISPCNT && addr < IO_DISPCNT + 4)
        return 1;
    if (addr >= IO_BGCNT_BASE && addr < IO_BGCNT_BASE + 2 * IO_BG_COUNT)
        return 1;
    if (addr >= IO_BG_SCROLL_BASE && addr < IO_BG_SCROLL_BASE + 4 * IO_BG_COUNT)
        return 1;
    if (addr >= IO_DISPCNT_SUB && addr < IO_DISPCNT_SUB + 4)
        return 1;
    if (addr >= IO_BGCNT_SUB_BASE && addr < IO_BGCNT_SUB_BASE + 2 * IO_BG_COUNT)
        return 1;
    if (addr >= IO_BG_SCROLL_SUB_BASE && addr < IO_BG_SCROLL_SUB_BASE + 4 * IO_BG_COUNT)
        return 1;
    if (addr >= IO_BG_AFFINE_BASE && addr < IO_BG_AFFINE_BASE + IO_BG_AFFINE_SIZE)
        return 1;
    if (addr >= IO_BG_AFFINE_SUB_BASE && addr < IO_BG_AFFINE_SUB_BASE + IO_BG_AFFINE_SIZE)
        return 1;
    /* 阶段 20.2/20.3：窗口 0x..40-0x..4A、混合 0x..50-0x..54、捕获 0x..64-0x..67、亮度 0x..6C-0x..6D */
    if (addr >= IO_WIN0H && addr < IO_WINOUT + 2)
        return 1;
    if (addr >= IO_BLENDCNT && addr < IO_BLENDY + 2)
        return 1;
    if (addr >= IO_DISPCAPCNT && addr < IO_DISPCAPCNT + 4)
        return 1;
    if (addr >= IO_MASTER_BRIGHT && addr < IO_MASTER_BRIGHT + 2)
        return 1;
    if (addr >= IO_WIN0H_SUB && addr < IO_WIN0H_SUB + 12)
        return 1;
    if (addr >= IO_BLENDCNT_SUB && addr < IO_BLENDCNT_SUB + 6)
        return 1;
    if (addr >= IO_MASTER_BRIGHT_SUB && addr < IO_MASTER_BRIGHT_SUB + 2)
        return 1;
    return 0;
}

/* 把仿射参数地址解析成：engine(0=主,1=副)、bg(0=BG2,1=BG3)、块内字节偏移(0..15)。
   命中返回 1。块内布局：off 0..7 = PA/PB/PC/PD(4×16b)，off 8..11 = X(32b)，off 12..15 = Y(32b)。 */
static int disp_affine_parse(uint32_t addr, int *engine, int *bg, int *off)
{
    if (addr >= IO_BG_AFFINE_BASE && addr < IO_BG_AFFINE_BASE + IO_BG_AFFINE_SIZE) {
        *engine = 0;
        addr -= IO_BG_AFFINE_BASE;
    } else if (addr >= IO_BG_AFFINE_SUB_BASE && addr < IO_BG_AFFINE_SUB_BASE + IO_BG_AFFINE_SIZE) {
        *engine = 1;
        addr -= IO_BG_AFFINE_SUB_BASE;
    } else {
        return 0;
    }
    *bg = addr >= 16 ? 1 : 0;
    *off = (int)(addr & 15u);
    return 1;
}

/* 16 位寄存器表：把窗口/混合/亮度地址映射到 disp_t 内字段（阶段 20.2/20.3）。 */
static const struct { uint32_t addr; size_t off; } disp_reg16s[] = {
    { IO_WIN0H,          offsetof(disp_t, win0h) },
    { IO_WIN1H,          offsetof(disp_t, win1h) },
    { IO_WIN0V,          offsetof(disp_t, win0v) },
    { IO_WIN1V,          offsetof(disp_t, win1v) },
    { IO_WININ,          offsetof(disp_t, winin) },
    { IO_WINOUT,         offsetof(disp_t, winout) },
    { IO_BLENDCNT,       offsetof(disp_t, blendcnt) },
    { IO_BLENDALPHA,     offsetof(disp_t, blendalpha) },
    { IO_BLENDY,         offsetof(disp_t, blendy) },
    { IO_MASTER_BRIGHT,  offsetof(disp_t, master_bright) },
    { IO_WIN0H_SUB + 0,  offsetof(disp_t, win0h_sub) },
    { IO_WIN0H_SUB + 2,  offsetof(disp_t, win1h_sub) },
    { IO_WIN0H_SUB + 4,  offsetof(disp_t, win0v_sub) },
    { IO_WIN0H_SUB + 6,  offsetof(disp_t, win1v_sub) },
    { IO_WIN0H_SUB + 8,  offsetof(disp_t, winin_sub) },
    { IO_WIN0H_SUB + 10, offsetof(disp_t, winout_sub) },
    { IO_BLENDCNT_SUB + 0, offsetof(disp_t, blendcnt_sub) },
    { IO_BLENDCNT_SUB + 2, offsetof(disp_t, blendalpha_sub) },
    { IO_BLENDCNT_SUB + 4, offsetof(disp_t, blendy_sub) },
    { IO_MASTER_BRIGHT_SUB, offsetof(disp_t, master_bright_sub) },
};
#define DISP_REG16_COUNT (sizeof(disp_reg16s) / sizeof(disp_reg16s[0]))

/* 命中返回字段指针，未命中返回 NULL。16 位寄存器按字节访问：地址可能落在
   低字节（偶地址）或高字节（奇地址），故用 addr & ~1u 对齐到寄存器基址。 */
static uint16_t *disp_reg16(disp_t *d, uint32_t addr)
{
    uint32_t base = addr & ~1u;
    for (size_t i = 0; i < DISP_REG16_COUNT; i++)
        if (disp_reg16s[i].addr == base)
            return (uint16_t *)((char *)d + disp_reg16s[i].off);
    return NULL;
}

uint8_t disp_read8(const disp_t *d, uint32_t addr)
{
    /* DISPCNT：32 位，小端按字节取（基址=最低字节） */
    if (addr >= IO_DISPCNT && addr < IO_DISPCNT + 4)
        return (uint8_t)(d->dispcnt >> ((addr - IO_DISPCNT) * 8));
    if (addr >= IO_DISPCNT_SUB && addr < IO_DISPCNT_SUB + 4)
        return (uint8_t)(d->dispcnt_sub >> ((addr - IO_DISPCNT_SUB) * 8));

    /* BGxCNT：16 位，每 BG 占 2 字节 */
    if (addr >= IO_BGCNT_BASE && addr < IO_BGCNT_BASE + 2 * IO_BG_COUNT) {
        uint32_t off = addr - IO_BGCNT_BASE;
        return (uint8_t)(d->bgcnt[off / 2] >> ((off % 2) * 8));
    }
    if (addr >= IO_BGCNT_SUB_BASE && addr < IO_BGCNT_SUB_BASE + 2 * IO_BG_COUNT) {
        uint32_t off = addr - IO_BGCNT_SUB_BASE;
        return (uint8_t)(d->bgcnt_sub[off / 2] >> ((off % 2) * 8));
    }

    /* 滚动：16 位，每 BG 占 4 字节（HOFS + VOFS） */
    if (addr >= IO_BG_SCROLL_BASE && addr < IO_BG_SCROLL_BASE + 4 * IO_BG_COUNT) {
        uint32_t off = addr - IO_BG_SCROLL_BASE;
        uint16_t v = (off % 4) < 2 ? d->hofs[off / 4] : d->vofs[off / 4];
        return (uint8_t)(v >> ((off % 2) * 8));
    }
    if (addr >= IO_BG_SCROLL_SUB_BASE && addr < IO_BG_SCROLL_SUB_BASE + 4 * IO_BG_COUNT) {
        uint32_t off = addr - IO_BG_SCROLL_SUB_BASE;
        uint16_t v = (off % 4) < 2 ? d->hofs_sub[off / 4] : d->vofs_sub[off / 4];
        return (uint8_t)(v >> ((off % 2) * 8));
    }

    /* 仿射参数：PA-PD 16 位小端、X/Y 32 位小端 */
    int engine, bg, off;
    if (disp_affine_parse(addr, &engine, &bg, &off)) {
        const int16_t *pa = engine ? d->bg_pa_sub[bg] : d->bg_pa[bg];
        const int32_t *ref = engine ? d->bg_ref_sub[bg] : d->bg_ref[bg];
        if (off < 8)
            return (uint8_t)((uint16_t)pa[off / 2] >> ((off % 2) * 8));
        uint32_t r = (uint32_t)(off < 12 ? ref[0] : ref[1]);
        return (uint8_t)(r >> ((off - (off < 12 ? 8 : 12)) * 8));
    }

    /* 阶段 20.2/20.3：窗口/混合/亮度 16 位寄存器 */
    {
        uint16_t *r = disp_reg16((disp_t *)d, addr);
        if (r != NULL)
            return (uint8_t)(*r >> ((addr & 1u) * 8));
    }
    /* DISPCAPCNT：32 位 */
    if (addr >= IO_DISPCAPCNT && addr < IO_DISPCAPCNT + 4)
        return (uint8_t)(d->dispcapcnt >> ((addr - IO_DISPCAPCNT) * 8));
    return 0;
}

/* 往 16 位寄存器写一个字节（小端：低位字节在低地址） */
static void write_byte16(uint16_t *reg, int byte_idx, uint8_t val)
{
    uint16_t mask = (uint16_t)(0xFFu << (byte_idx * 8));
    *reg = (uint16_t)((*reg & ~mask) | ((uint16_t)val << (byte_idx * 8)));
}

void disp_write8(disp_t *d, uint32_t addr, uint8_t val)
{
    /* DISPCNT：32 位 */
    if (addr >= IO_DISPCNT && addr < IO_DISPCNT + 4) {
        uint32_t shift = (addr - IO_DISPCNT) * 8;
        d->dispcnt = (d->dispcnt & ~(0xFFu << shift)) | ((uint32_t)val << shift);
        return;
    }
    if (addr >= IO_DISPCNT_SUB && addr < IO_DISPCNT_SUB + 4) {
        uint32_t shift = (addr - IO_DISPCNT_SUB) * 8;
        d->dispcnt_sub = (d->dispcnt_sub & ~(0xFFu << shift)) | ((uint32_t)val << shift);
        return;
    }

    /* BGxCNT：16 位 */
    if (addr >= IO_BGCNT_BASE && addr < IO_BGCNT_BASE + 2 * IO_BG_COUNT) {
        uint32_t off = addr - IO_BGCNT_BASE;
        write_byte16(&d->bgcnt[off / 2], (int)(off % 2), val);
        return;
    }
    if (addr >= IO_BGCNT_SUB_BASE && addr < IO_BGCNT_SUB_BASE + 2 * IO_BG_COUNT) {
        uint32_t off = addr - IO_BGCNT_SUB_BASE;
        write_byte16(&d->bgcnt_sub[off / 2], (int)(off % 2), val);
        return;
    }

    /* 滚动：16 位 */
    if (addr >= IO_BG_SCROLL_BASE && addr < IO_BG_SCROLL_BASE + 4 * IO_BG_COUNT) {
        uint32_t off = addr - IO_BG_SCROLL_BASE;
        uint16_t *reg = (off % 4) < 2 ? &d->hofs[off / 4] : &d->vofs[off / 4];
        write_byte16(reg, (int)(off % 2), val);
        return;
    }
    if (addr >= IO_BG_SCROLL_SUB_BASE && addr < IO_BG_SCROLL_SUB_BASE + 4 * IO_BG_COUNT) {
        uint32_t off = addr - IO_BG_SCROLL_SUB_BASE;
        uint16_t *reg = (off % 4) < 2 ? &d->hofs_sub[off / 4] : &d->vofs_sub[off / 4];
        write_byte16(reg, (int)(off % 2), val);
        return;
    }

    /* 仿射参数：PA-PD 16 位、X/Y 32 位（byte3 只低 4 位有效，高位忽略） */
    int engine, bg, off;
    if (disp_affine_parse(addr, &engine, &bg, &off)) {
        int16_t *pa = engine ? d->bg_pa_sub[bg] : d->bg_pa[bg];
        int32_t *ref = engine ? d->bg_ref_sub[bg] : d->bg_ref[bg];
        if (off < 8) {
            uint16_t v = (uint16_t)pa[off / 2];
            write_byte16(&v, (int)(off % 2), val);
            pa[off / 2] = (int16_t)v;
            return;
        }
        int xy = off < 12 ? 0 : 1;
        uint32_t shift = (uint32_t)(off - (off < 12 ? 8 : 12)) * 8;
        uint32_t v = (uint32_t)ref[xy];
        uint32_t byte = shift == 24 ? (uint32_t)(val & 0x0Fu) : val; /* bit28-31 无效 */
        v = (v & ~(0xFFu << shift)) | (byte << shift);
        ref[xy] = (int32_t)v;
        return;
    }

    /* 阶段 20.2/20.3：窗口/混合/亮度 16 位寄存器 */
    {
        uint16_t *r = disp_reg16(d, addr);
        if (r != NULL) {
            write_byte16(r, (int)(addr & 1u), val);
            return;
        }
    }
    /* DISPCAPCNT：32 位 */
    if (addr >= IO_DISPCAPCNT && addr < IO_DISPCAPCNT + 4) {
        uint32_t shift = (addr - IO_DISPCAPCNT) * 8;
        d->dispcapcnt = (d->dispcapcnt & ~(0xFFu << shift)) | ((uint32_t)val << shift);
        return;
    }
}
