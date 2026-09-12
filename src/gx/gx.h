#ifndef NDS_EMU_GX_H
#define NDS_EMU_GX_H

#include <stdint.h>

/* 阶段 19：NDS 3D 几何引擎（Geometry Engine）最小实现。
   吃「命令流」（矩阵/顶点/多边形属性），做矩阵变换 + 顶点变换 + 裁剪，
   输出屏幕空间三角形，由软件光栅化写入 3D 帧缓冲（256×192 RGB555）。
   寄存器：DISP3DCNT(0x04000060)、GXFIFO(0x04000400)、命令端口(0x04000440 起)、
   GXSTAT(0x04000600)、RAM_COUNT(0x04000604)。
   注意：0x04000400..0x040005FF 与 ARM7 音频寄存器重叠——按访问者 CPU 分流
   （ARM9→几何，ARM7→音频），由 io 层按 is_arm7 路由。 */

#define GX_SCREEN_W 256
#define GX_SCREEN_H 192

/* 寄存器地址 */
#define IO_DISP3DCNT     0x04000060u
#define GX_GXFIFO        0x04000400u
#define GX_GXFIFO_END    0x04000440u   /* GXFIFO 镜像上界（16 字） */
#define GX_CMD_PORT_BASE 0x04000440u   /* 端口 = 0x400 + cmd*4 */
#define GX_CMD_PORT_END  0x04000600u
#define GX_GXSTAT        0x04000600u
#define GX_RAM_COUNT     0x04000604u
#define GX_REGION_END    0x04000608u

/* DISP3DCNT 位 */
#define DISP3D_ENABLE      (1u << 13)
#define DISP3D_UNDER_OVER  (1u << 12)

/* GXSTAT 位 */
#define GXSTAT_FIFO_LESS_HALF (1u << 25)  /* 21-B9wy：FIFO 不足半满（有空间收命令） */
#define GXSTAT_FIFO_EMPTY  (1u << 26)
#define GXSTAT_BUSY        (1u << 27)
#define GXSTAT_IRQ_MODE    (3u << 30)

/* 几何命令码 */
#define GX_CMD_MTX_MODE     0x10
#define GX_CMD_MTX_PUSH     0x11
#define GX_CMD_MTX_POP      0x12
#define GX_CMD_MTX_STORE    0x13
#define GX_CMD_MTX_RESTORE  0x14
#define GX_CMD_MTX_IDENTITY 0x15
#define GX_CMD_MTX_LOAD_4x4 0x16
#define GX_CMD_MTX_LOAD_4x3 0x17
#define GX_CMD_MTX_MULT_4x4 0x18
#define GX_CMD_MTX_MULT_4x3 0x19
#define GX_CMD_MTX_MULT_3x3 0x1A
#define GX_CMD_MTX_SCALE    0x1B
#define GX_CMD_MTX_TRANS    0x1C
#define GX_CMD_COLOR        0x20
#define GX_CMD_NORMAL       0x21
#define GX_CMD_TEXCOORD     0x22
#define GX_CMD_VTX_16       0x23
#define GX_CMD_VTX_10       0x24
#define GX_CMD_VTX_XY       0x25
#define GX_CMD_VTX_XZ       0x26
#define GX_CMD_VTX_YZ       0x27
#define GX_CMD_VTX_DIFF     0x28
#define GX_CMD_POLYGON_ATTR 0x29
#define GX_CMD_BEGIN_VTXS   0x40
#define GX_CMD_END_VTXS     0x41
#define GX_CMD_SWAP_BUFFERS 0x50
#define GX_CMD_VIEWPORT     0x60

/* 图元类型（BEGIN_VTXS 参数低 2 位） */
#define GX_PRIM_TRIANGLES 0
#define GX_PRIM_QUADS     1
#define GX_PRIM_TRI_STRIP 2
#define GX_PRIM_TRI_FAN   3

/* 定点：12 位小数（1.0 = 0x1000）。矩阵元素 1.19.12，顶点 1.3.12。 */
#define GX_FP_SHIFT 12
#define GX_FP_ONE   (1 << GX_FP_SHIFT)

/* 顶点列表上限（一次性最多收这么多顶点才光栅化） */
#define GX_MAX_VERTS 1024

typedef struct gx_vertex {
    int sx, sy;          /* 屏幕像素坐标 */
    uint16_t color;      /* RGB555 */
} gx_vertex_t;

typedef struct gx {
    uint32_t disp3dcnt;  /* DISP3DCNT */
    uint32_t gxstat;     /* GXSTAT（FIFO 空/忙状态位） */
    /* 21-B9yi(续16)：3D 引擎「工作周期」余额。melonDS 里每条 GX 命令都会
       `AddCycles(3..254)` 累加工作周期，`GPU3D::Run()` 按 ARM9 时间戳消耗它们，
       期间 `GXSTAT bit27`（3D 忙）保持置位；游戏正是用
       「`ldr r0,[0x04000600]; ands r0,#0x8000000; bne -8`」等这段忙。
       本地此前从不置 bit27 ⇒ 这类等待被整段跳过，游戏跑得比参考核快
       （实测 f=2120 起画面比参考核早 ~30 帧）。 */
    uint32_t busy_cycles;
    /* 21-B9yi(续22)：GX 命令 FIFO 里已入队、尚未被 3D 引擎消费的**字数**
       （melonDS 的 `CmdFIFO` 是 112 项 = 224 字）。模式 7（GX FIFO）DMA 每次
       只能推进到「有空位」为止，搬不完就保持 InProgress 等下次——本地此前
       一次搬完 ⇒ 立刻满足 RemCount==0 而挂出 IF bit11，且 GXSTAT 的 FIFO
       电平位恒为「空」，与参考核不同。 */
    uint32_t fifo_words;
#define GX_FIFO_CAP_WORDS 224u
    /* 21-B9yi(续27)：melonDS 的 GX FIFO 是 **112 条命令条目**（`CmdPIPE`），
       满了就 `GXFIFOStall()` 卡住写入方（CPU/模式 7 DMA）；游戏则用
       「等 GXSTAT bit27 落 0」来等引擎腾空。本地没有 CPU 停摆机制，
       改为**用 bit27 表达「FIFO 满」**（游戏本来就在轮询它），
       参数写入不受影响（参数属于已有条目）。 */
#define GX_FIFO_CAP_ENTRIES 112u
    /* 21-B9yi(续23)：FIFO 消费的**周期余数累加器**。ARM9 每步只推进 1-2 个周期，
       若直接 `cycles/div` 取整会永远得 0 ⇒ FIFO 永不消费 ⇒ 模式 7 DMA 永远卡住
       （实测 f=2141 起 ARM9 在 0x0202350C 三指令小循环里死等 10+ 帧）。
       与「卡带时钟用满周期预算」是同一类修正。 */
    uint32_t fifo_drain_acc;
    /* 21-B9yi(续27)：交换缓冲之后的「管线忙」标志（对应 melonDS 的 bit27）。 */
    int swap_busy;

    int mt_mode;         /* 当前矩阵模式 0=投影 1/2=位置 3=纹理 */
    int64_t proj[16];    /* 投影矩阵 */
    int64_t pos[16];     /* 位置矩阵 */
    int64_t tex[16];     /* 纹理矩阵 */
    int64_t stack[32][16]; /* 矩阵栈 */
    int sp;

    /* 当前顶点属性 */
    uint32_t color;      /* 0x20 COLOR */
    /* 诊断计数（21-B9wu）：命令数 / 光栅化三角形数 / FIFO 与命令端口写入次数 */
    uint32_t cmd_count;
    uint32_t tri_count;
    uint32_t fifo_writes;
    uint32_t port_writes;
    int32_t tc_s, tc_t;  /* 0x22 TEXCOORD（1.3.12） */
    int32_t px, py, pz;  /* 上一顶点位置（1.3.12） */

    /* 视口（0x60 VIEWPORT：x1,y1,x2,y2 各 8 位） */
    int vx1, vy1, vx2, vy2;

    /* 顶点列表状态 */
    int begin_prim;
    int in_vtxs;
    gx_vertex_t verts[GX_MAX_VERTS];
    int vcount;

    /* 3D 帧缓冲：256×192 RGB555 */
    uint16_t fb[GX_SCREEN_W * GX_SCREEN_H];
} gx_t;

/* 地址判定：DISP3DCNT + GXSTAT/RAM_COUNT（字节级寄存器，ARM9 侧）。 */
int gx_is_addr(uint32_t addr);
uint8_t gx_read8(const gx_t *g, uint32_t addr);
void gx_write8(gx_t *g, uint32_t addr, uint8_t val);

/* 几何命令区（0x04000400..0x040005FF）32 位写：GXFIFO 命令字 / 命令端口。 */
void gx_write32(gx_t *g, uint32_t addr, uint32_t val);

/* 初始化 / 复位（矩阵置单位阵，帧缓冲清零）。 */
void gx_reset(gx_t *g);

/* 21-B9yi(续16)：按系统时钟消耗 3D 引擎工作周期（到 0 时清 GXSTAT bit27）。 */
void gx_advance(gx_t *g, uint32_t cycles);

/* 21-B9yi(续22)：GX 命令 FIFO 的剩余空间（字）。模式 7 DMA 用它决定一次搬多少。 */
uint32_t gx_fifo_free_words(const gx_t *g);
int gx_fifo_can_accept(const gx_t *g);

/* 21-B9yi(续25) 诊断：队列/引擎状态快照（FIFO 字数、工作周期余额、队列长度、
   待收参数数），供 runner 逐帧 trace 打印。 */
void gx_state(const gx_t *g, uint32_t *fifo_words, uint32_t *busy,
              int *qlen, int *pending);

/* 把 3D 帧缓冲暴露给渲染层（ppu 混合 3D 图层用）。 */
const uint16_t *gx_framebuffer(const gx_t *g);

/* 顶点变换：模型坐标 (x,y,z)（1.3.12）经 pos×proj 变换、透视除、视口映射，
   输出屏幕像素坐标 (sx,sy)。供单元测试直接调用。 */
void gx_transform_vertex(const gx_t *g, int32_t x, int32_t y, int32_t z, int *sx, int *sy);

/* 软件光栅化一个平色三角形到帧缓冲（供单元测试直接调用）。 */
void gx_raster_tri(gx_t *g, int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color);

#endif /* NDS_EMU_GX_H */
