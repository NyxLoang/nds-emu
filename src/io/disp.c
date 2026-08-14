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
    return 0;
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
}
