#ifndef NDS_EMU_IO_DISP_H
#define NDS_EMU_IO_DISP_H

#include <stdint.h>

/* 2D 显示控制寄存器（主引擎 + 副引擎，阶段 9 起实现真实 2D）。
   DISPCNT 为 32 位，BGxCNT / 滚动为 16 位，均按字节由 io_read8/io_write8 访问。 */

/* 主引擎（Engine A）地址 */
#define IO_DISPCNT          0x04000000u   /* 显示控制（32 位） */
#define IO_BGCNT_BASE       0x04000008u   /* BG0-3 控制（各 16 位） */
#define IO_BG_SCROLL_BASE   0x04000010u   /* BG0-3 滚动 HOFS/VOFS（各 16 位） */

/* 副引擎（Engine B）地址 */
#define IO_DISPCNT_SUB      0x04001000u
#define IO_BGCNT_SUB_BASE   0x04001008u
#define IO_BG_SCROLL_SUB_BASE 0x04001010u

#define IO_BG_COUNT 4

/* ---- DISPCNT 位定义 ---- */
#define DISPCNT_MODE_MASK        0x00000007u   /* bit0-2 BG 模式 */
#define DISPCNT_FORCED_BLANK     (1u << 7)     /* 强制消隐 */
#define DISPCNT_BG0              (1u << 8)
#define DISPCNT_BG1              (1u << 9)
#define DISPCNT_BG2              (1u << 10)
#define DISPCNT_BG3              (1u << 11)
#define DISPCNT_OBJ              (1u << 12)
#define DISPCNT_DISPLAY_MODE_SHIFT 16          /* bit16-17 显示模式(0=关,1=正常,2=VRAM显示,3=主存) */
#define DISPCNT_DISPLAY_MODE_MASK  (3u << 16)
#define DISPCNT_CHAR_BASE_SHIFT  24            /* bit24-26 字符基址(64K 步进, 仅 Engine A) */
#define DISPCNT_CHAR_BASE_MASK   (7u << 24)
#define DISPCNT_SCREEN_BASE_SHIFT 27           /* bit27-29 屏幕基址(64K 步进, 仅 Engine A) */
#define DISPCNT_SCREEN_BASE_MASK (7u << 27)

/* ---- BGxCNT 位定义 ---- */
#define BGCNT_PRIORITY_SHIFT    0
#define BGCNT_PRIORITY_MASK     (3u << 0)
#define BGCNT_CHAR_BASE_SHIFT   2              /* bit2-5 字符基址块(16KB 步进) */
#define BGCNT_CHAR_BASE_MASK    (0xFu << 2)
#define BGCNT_COLORS_256        (1u << 7)      /* 0=16色/16调色板, 1=256色/1调色板 */
#define BGCNT_DIRECT_COLOR      (1u << 2)      /* 位图模式下 bit2=1 为直色(16bpp)，0 为 256 色(8bpp) */
#define BGCNT_SCREEN_BASE_SHIFT 8              /* bit8-12 屏幕基址块(2KB 步进) */
#define BGCNT_SCREEN_BASE_MASK  (0x1Fu << 8)
#define BGCNT_SCREEN_SIZE_SHIFT 14
#define BGCNT_SCREEN_SIZE_MASK  (3u << 14)

/* 显示寄存器状态：主/副引擎各一套 DISPCNT、4 个 BGxCNT、4 组滚动。 */
typedef struct disp {
    uint32_t dispcnt;                 /* 主 DISPCNT */
    uint32_t dispcnt_sub;             /* 副 DISPCNT */
    uint16_t bgcnt[IO_BG_COUNT];      /* 主 BG0-3 CNT */
    uint16_t bgcnt_sub[IO_BG_COUNT];  /* 副 BG0-3 CNT */
    uint16_t hofs[IO_BG_COUNT];       /* 主 BG0-3 水平滚动 */
    uint16_t vofs[IO_BG_COUNT];       /* 主 BG0-3 垂直滚动 */
    uint16_t hofs_sub[IO_BG_COUNT];   /* 副 BG0-3 水平滚动 */
    uint16_t vofs_sub[IO_BG_COUNT];   /* 副 BG0-3 垂直滚动 */
} disp_t;

int disp_is_addr(uint32_t addr);
uint8_t disp_read8(const disp_t *d, uint32_t addr);
void disp_write8(disp_t *d, uint32_t addr, uint8_t val);

#endif /* NDS_EMU_IO_DISP_H */
