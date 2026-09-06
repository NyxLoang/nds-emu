#ifndef NDS_EMU_IO_DISP_H
#define NDS_EMU_IO_DISP_H

#include <stdint.h>

/* 2D 显示控制寄存器（主引擎 + 副引擎，阶段 9 起实现真实 2D）。
   DISPCNT 为 32 位，BGxCNT / 滚动为 16 位，均按字节由 io_read8/io_write8 访问。 */

/* 主引擎（Engine A）地址 */
#define IO_DISPCNT          0x04000000u   /* 显示控制（32 位） */
#define IO_DISPSTAT         0x04000004u   /* 显示状态（16 位，bit0=VBlank） */
#define IO_BGCNT_BASE       0x04000008u   /* BG0-3 控制（各 16 位） */
#define IO_BG_SCROLL_BASE   0x04000010u   /* BG0-3 滚动 HOFS/VOFS（各 16 位） */

/* 副引擎（Engine B）地址 */
#define IO_DISPCNT_SUB      0x04001000u
#define IO_DISPSTAT_SUB     0x04001004u
#define IO_BGCNT_SUB_BASE   0x04001008u
#define IO_BG_SCROLL_SUB_BASE 0x04001010u

#define IO_BG_COUNT 4

/* 仿射（旋转/缩放）背景参数区（阶段 20.1）：主引擎 0x04000020 起 0x20 字节，副引擎 +0x1000。
   布局：BG2 的 PA/PB/PC/PD（4×16bit）+ X/Y 参考点（2×32bit），其后 BG3 同样 0x10 字节。 */
#define IO_BG_AFFINE_BASE     0x04000020u
#define IO_BG_AFFINE_SUB_BASE 0x04001020u
#define IO_BG_AFFINE_SIZE     0x20u

/* BGxCNT 溢出位：旋转/缩放模式下，纹理越界时 0=透明、1=环绕（阶段 20.1） */
#define BGCNT_OVERFLOW        (1u << 13)

/* ---- 阶段 20.2/20.3：混合/亮度/窗口/捕获寄存器（主引擎地址，副引擎 +0x1000） ---- */
#define IO_WIN0H             0x04000040u
#define IO_WIN1H             0x04000042u
#define IO_WIN0V             0x04000044u
#define IO_WIN1V             0x04000046u
#define IO_WININ             0x04000048u
#define IO_WINOUT            0x0400004Au
#define IO_BLENDCNT          0x04000050u
#define IO_BLENDALPHA        0x04000052u
#define IO_BLENDY            0x04000054u
#define IO_DISPCAPCNT        0x04000064u
#define IO_MASTER_BRIGHT     0x0400006Cu

#define IO_WIN0H_SUB         0x04001040u
#define IO_BLENDCNT_SUB      0x04001050u
#define IO_MASTER_BRIGHT_SUB 0x0400106Cu

/* BLENDCNT 位：0-5 第一目标(BG0-3/OBJ/BD)、6-7 模式、8-13 第二目标 */
#define BLEND_1ST_BG0        (1u << 0)
#define BLEND_1ST_BG1        (1u << 1)
#define BLEND_1ST_BG2        (1u << 2)
#define BLEND_1ST_BG3        (1u << 3)
#define BLEND_1ST_OBJ        (1u << 4)
#define BLEND_1ST_BD         (1u << 5)
#define BLEND_MODE_SHIFT     6
#define BLEND_MODE_MASK      (3u << 6)
#define BLEND_2ND_BG0        (1u << 8)
#define BLEND_2ND_BG1        (1u << 9)
#define BLEND_2ND_BG2        (1u << 10)
#define BLEND_2ND_BG3        (1u << 11)
#define BLEND_2ND_OBJ        (1u << 12)
#define BLEND_2ND_BD         (1u << 13)

/* MASTER_BRIGHT 位：0-4 因子、14-15 模式(0=关,1=增亮,2=减暗) */
#define MB_FACTOR_MASK       0x1Fu
#define MB_MODE_SHIFT        14

/* DISPCAPCNT 位（仅主引擎）：31 使能、16-17 写块、20-21 尺寸、24 源A、29-30 源选择 */
#define DISPCAP_ENABLE       (1u << 31)

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
    uint16_t dispstat;                /* 主 DISPSTAT（bit0=VBlank 等） */
    uint16_t dispstat_sub;            /* 副 DISPSTAT */
    uint16_t bgcnt[IO_BG_COUNT];      /* 主 BG0-3 CNT */
    uint16_t bgcnt_sub[IO_BG_COUNT];  /* 副 BG0-3 CNT */
    uint16_t hofs[IO_BG_COUNT];       /* 主 BG0-3 水平滚动 */
    uint16_t vofs[IO_BG_COUNT];       /* 主 BG0-3 垂直滚动 */
    uint16_t hofs_sub[IO_BG_COUNT];   /* 副 BG0-3 水平滚动 */
    uint16_t vofs_sub[IO_BG_COUNT];   /* 副 BG0-3 垂直滚动 */
    /* 阶段 20.1：BG2/BG3 仿射参数。PA-PD 为 1.7.8 定点（16 位有符号），
       X/Y 参考点为 1.19.8 定点（32 位有符号，高 4 位忽略）。 */
    int16_t bg_pa[2][4];              /* 主 [bg 0=BG2,1=BG3][A,B,C,D] */
    int32_t bg_ref[2][2];             /* 主 [bg][0=X,1=Y] */
    int16_t bg_pa_sub[2][4];          /* 副 [bg][A,B,C,D] */
    int32_t bg_ref_sub[2][2];         /* 副 [bg][0=X,1=Y] */
    /* 阶段 20.2/20.3：混合 / 亮度 / 窗口 / 显示捕获 */
    uint16_t blendcnt;                /* BLENDCNT（主） */
    uint16_t blendcnt_sub;            /* BLENDCNT（副） */
    uint16_t blendalpha;              /* BLDALPHA：EVA(0-4) EVB(8-12) */
    uint16_t blendalpha_sub;
    uint16_t blendy;                  /* BLDY：EVY(0-4) */
    uint16_t blendy_sub;
    uint16_t master_bright;           /* MASTER_BRIGHT（主） */
    uint16_t master_bright_sub;
    uint32_t dispcapcnt;              /* DISPCAPCNT（仅主引擎） */
    uint16_t win0h, win1h, win0v, win1v;      /* 主窗口矩形 */
    uint16_t win0h_sub, win1h_sub, win0v_sub, win1v_sub;
    uint16_t winin, winout;           /* 主窗口使能 */
    uint16_t winin_sub, winout_sub;
} disp_t;

int disp_is_addr(uint32_t addr);
uint8_t disp_read8(const disp_t *d, uint32_t addr);
void disp_write8(disp_t *d, uint32_t addr, uint8_t val);

#endif /* NDS_EMU_IO_DISP_H */
