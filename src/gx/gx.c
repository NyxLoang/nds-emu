#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "gx.h"

/* 阶段 19：NDS 3D 几何引擎最小实现。
   命令流经 GXFIFO（0x04000400）或命令端口（0x04000440+cmd*4）送入，
   按「命令码 + 若干参数」解码后即时执行：矩阵运算、顶点提交（变换+投影+视口），
   END_VTXS 时把收集到的顶点按图元光栅化到 3D 帧缓冲（256×192 RGB555）。
   简化：命令同步执行（不模拟 4 拍 FIFO 延迟）；平色着色（无光照/纹理采样）。 */

/* ---- 命令 FIFO（解码状态机） ---- */

#define GX_QUEUE_CAP 256

typedef struct {
    uint8_t cmd;
    int nparams;          /* 该命令需要的参数总数 */
    int filled;           /* 已收参数个数 */
    int32_t params[16];   /* 参数缓冲（矩阵最多 16 个） */
} gx_pend_t;

static gx_pend_t g_q[GX_QUEUE_CAP];
static int g_q_head = 0;   /* 队头下标 */
static int g_q_len = 0;    /* 队列长度 */
static int g_pending = 0;  /* 还需填充的参数总数（0=下一次 GXFIFO 写是命令字） */

static int gx_cmd_nparams(uint8_t cmd)
{
    switch (cmd) {
    case GX_CMD_MTX_MODE:     return 1;
    case GX_CMD_MTX_PUSH:     return 0;
    case GX_CMD_MTX_POP:      return 1;
    case GX_CMD_MTX_STORE:    return 1;
    case GX_CMD_MTX_RESTORE:  return 1;
    case GX_CMD_MTX_IDENTITY: return 0;
    case GX_CMD_MTX_LOAD_4x4: return 16;
    case GX_CMD_MTX_LOAD_4x3: return 12;
    case GX_CMD_MTX_MULT_4x4: return 16;
    case GX_CMD_MTX_MULT_4x3: return 12;
    case GX_CMD_MTX_MULT_3x3: return 9;
    case GX_CMD_MTX_SCALE:    return 3;
    case GX_CMD_MTX_TRANS:    return 3;
    case GX_CMD_COLOR:        return 1;
    case GX_CMD_NORMAL:       return 1;
    case GX_CMD_TEXCOORD:     return 1;
    case GX_CMD_VTX_16:       return 2;
    case GX_CMD_VTX_10:       return 1;
    case GX_CMD_VTX_XY:       return 1;
    case GX_CMD_VTX_XZ:       return 1;
    case GX_CMD_VTX_YZ:       return 1;
    case GX_CMD_VTX_DIFF:     return 1;
    case GX_CMD_POLYGON_ATTR: return 1;
    case GX_CMD_BEGIN_VTXS:   return 1;
    case GX_CMD_END_VTXS:     return 0;
    case GX_CMD_SWAP_BUFFERS: return 1;
    case GX_CMD_VIEWPORT:     return 1;
    default:                  return 0; /* 未知命令：吞掉，不崩 */
    }
}

static void gx_exec(gx_t *g, uint8_t cmd, const int32_t *p);

/* 执行队头所有已收集完整参数的命令（0 参数命令入队后即执行）。 */
static void gx_flush(gx_t *g)
{
    while (g_q_len > 0 && g_q[g_q_head].filled >= g_q[g_q_head].nparams) {
        gx_pend_t c = g_q[g_q_head];
        g_q_head = (g_q_head + 1) % GX_QUEUE_CAP;
        g_q_len--;
        gx_exec(g, c.cmd, c.params);
    }
}

static void gx_enqueue(gx_t *g, uint8_t cmd)
{
    g->cmd_count++;
    int n = gx_cmd_nparams(cmd);
    if (g_q_len >= GX_QUEUE_CAP)
        return; /* 队列满：丢弃（正常流程不会发生） */
    int idx = (g_q_head + g_q_len) % GX_QUEUE_CAP;
    g_q[idx].cmd = cmd;
    g_q[idx].nparams = n;
    g_q[idx].filled = 0;
    g_q_len++;
    g_pending += n;
    gx_flush(g);
}

static void gx_feed_param(gx_t *g, int32_t val)
{
    if (g_q_len == 0)
        return;
    gx_pend_t *p = &g_q[g_q_head];
    if (p->filled < p->nparams) {
        p->params[p->filled++] = val;
        g_pending--;
    }
    gx_flush(g);
}

/* ---- 矩阵运算（行主序 4×4，1.19.12） ---- */

static void gx_mat_identity(int64_t *m)
{
    memset(m, 0, 16 * sizeof(int64_t));
    m[0] = m[5] = m[10] = m[15] = GX_FP_ONE;
}

static void gx_mat_copy(int64_t *d, const int64_t *s)
{
    memcpy(d, s, 16 * sizeof(int64_t));
}

/* r = a * b（行主序：r[i][j] = Σ_k a[i][k] * b[k][j]，乘积右移 12 位回 1.19.12） */
static void gx_mat_mul(int64_t *r, const int64_t *a, const int64_t *b)
{
    int64_t t[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            int64_t s = 0;
            for (int k = 0; k < 4; k++)
                s += a[i * 4 + k] * b[k * 4 + j];
            t[i * 4 + j] = s >> GX_FP_SHIFT;
        }
    memcpy(r, t, sizeof(t));
}

/* 列主序装载：参数顺序 col0[4] col1[4] ...（ncols=3 时第 4 列固定 [0,0,0,1]）。 */
static void gx_load_colmajor(int64_t *m, const int32_t *p, int ncols)
{
    for (int i = 0; i < ncols * 4; i++) {
        int row = i % 4;
        int col = i / 4;
        m[row * 4 + col] = p[i];
    }
    if (ncols == 3) {
        m[3] = m[7] = m[11] = 0;
        m[15] = GX_FP_ONE;
    }
}

/* 10 位有符号数符号扩展（VTX_10/VTX_DIFF） */
static int32_t gx_sext10(int32_t v)
{
    return (v & 0x200) ? (v | ~0x3FF) : v;
}

/* ---- 顶点变换 + 光栅化 ---- */

void gx_transform_vertex(const gx_t *g, int32_t x, int32_t y, int32_t z, int *sx, int *sy)
{
    int64_t clip[16];
    gx_mat_mul(clip, g->pos, g->proj); /* clip = pos × proj */

    int64_t w = GX_FP_ONE;
    int64_t cx = (int64_t)x * clip[0] + (int64_t)y * clip[4] + (int64_t)z * clip[8] + w * clip[12];
    int64_t cy = (int64_t)x * clip[1] + (int64_t)y * clip[5] + (int64_t)z * clip[9] + w * clip[13];
    int64_t cw = (int64_t)x * clip[3] + (int64_t)y * clip[7] + (int64_t)z * clip[11] + w * clip[15];

    int64_t nx = cx >> GX_FP_SHIFT; /* NDC（1.19.12） */
    int64_t ny = cy >> GX_FP_SHIFT;
    int64_t nw = cw >> GX_FP_SHIFT;

    int64_t px, py;
    if (nw != 0) {
        px = (nx << GX_FP_SHIFT) / nw; /* 透视除 */
        py = (ny << GX_FP_SHIFT) / nw;
    } else {
        px = nx; /* 退化（w=0）：直接当正交用，避免除零 */
        py = ny;
    }

    /* 视口映射：NDC(-1..1) → (vx1..vx2, vy1..vy2)，y 翻转（+1=顶） */
    int wpx = g->vx2 - g->vx1;
    int hpx = g->vy2 - g->vy1;
    *sx = (int)(g->vx1 + ((px + GX_FP_ONE) * (int64_t)wpx) / (2 * GX_FP_ONE));
    *sy = (int)(g->vy1 + ((GX_FP_ONE - py) * (int64_t)hpx) / (2 * GX_FP_ONE));
}

/* 边函数：点 p 在边 a→b 的哪一侧（正=左，负=右，0=线上） */
static int gx_edge(int ax, int ay, int bx, int by, int px, int py)
{
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

/* 三点同侧判定：三条边符号一致（含 0）即在三角形内，与绕序无关 */
static int gx_point_in_tri(int px, int py, int x0, int y0, int x1, int y1, int x2, int y2)
{
    int e0 = gx_edge(x0, y0, x1, y1, px, py);
    int e1 = gx_edge(x1, y1, x2, y2, px, py);
    int e2 = gx_edge(x2, y2, x0, y0, px, py);
    int neg = (e0 < 0) || (e1 < 0) || (e2 < 0);
    int pos = (e0 > 0) || (e1 > 0) || (e2 > 0);
    return !(neg && pos);
}

void gx_raster_tri(gx_t *g, int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color)
{
    int minx = x0 < x1 ? x0 : x1; minx = minx < x2 ? minx : x2;
    int maxx = x0 > x1 ? x0 : x1; maxx = maxx > x2 ? maxx : x2;
    int miny = y0 < y1 ? y0 : y1; miny = miny < y2 ? miny : y2;
    int maxy = y0 > y1 ? y0 : y1; maxy = maxy > y2 ? maxy : y2;

    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= GX_SCREEN_W) maxx = GX_SCREEN_W - 1;
    if (maxy >= GX_SCREEN_H) maxy = GX_SCREEN_H - 1;

    for (int yy = miny; yy <= maxy; yy++)
        for (int xx = minx; xx <= maxx; xx++)
            if (gx_point_in_tri(xx, yy, x0, y0, x1, y1, x2, y2))
                g->fb[yy * GX_SCREEN_W + xx] = color;
}

static void gx_submit_vertex(gx_t *g, int32_t x, int32_t y, int32_t z)
{
    g->px = x; g->py = y; g->pz = z;
    if (!g->in_vtxs)
        return;
    if (g->vcount >= GX_MAX_VERTS)
        return;
    int sx, sy;
    gx_transform_vertex(g, x, y, z, &sx, &sy);
    g->verts[g->vcount].sx = sx;
    g->verts[g->vcount].sy = sy;
    g->verts[g->vcount].color = (uint16_t)(g->color & 0xFFFF);
    g->vcount++;
}

static void gx_raster_vert_tri(gx_t *g, const gx_vertex_t *a, const gx_vertex_t *b, const gx_vertex_t *c)
{
    gx_raster_tri(g, a->sx, a->sy, b->sx, b->sy, c->sx, c->sy, a->color);
    g->tri_count++;
}

static void gx_emit_prims(gx_t *g)
{
    int n = g->vcount;
    switch (g->begin_prim) {
    case GX_PRIM_TRIANGLES:
        for (int i = 0; i + 2 < n; i += 3)
            gx_raster_vert_tri(g, &g->verts[i], &g->verts[i + 1], &g->verts[i + 2]);
        break;
    case GX_PRIM_QUADS:
        for (int i = 0; i + 3 < n; i += 4) {
            gx_raster_vert_tri(g, &g->verts[i], &g->verts[i + 1], &g->verts[i + 2]);
            gx_raster_vert_tri(g, &g->verts[i], &g->verts[i + 2], &g->verts[i + 3]);
        }
        break;
    case GX_PRIM_TRI_STRIP:
        for (int i = 0; i + 2 < n; i++)
            gx_raster_vert_tri(g, &g->verts[i], &g->verts[i + 1], &g->verts[i + 2]);
        break;
    case GX_PRIM_TRI_FAN:
        for (int i = 1; i + 1 < n; i++)
            gx_raster_vert_tri(g, &g->verts[0], &g->verts[i], &g->verts[i + 1]);
        break;
    }
}

/* ---- 命令执行 ---- */

static int64_t *gx_cur(gx_t *g)
{
    switch (g->mt_mode) {
    case 0: return g->proj;
    case 3: return g->tex;
    default: return g->pos; /* 1=位置，2=位置+向量（本实现合并） */
    }
}

static void gx_exec(gx_t *g, uint8_t cmd, const int32_t *p)
{
    int64_t *m = gx_cur(g);
    int64_t tmp[16];
    /* 21-B9yi(续16)：按 melonDS `GPU3D` 的 `AddCycles()` 口径给每条命令记工作
       周期（矩阵类 16-35、顶点 15-18、交换缓冲 254、其余 3-5），期间 GXSTAT
       bit27 置位；`gx_advance()` 按系统时钟消耗。游戏用 bit27 等待 3D 完成，
       本地此前恒为 0 ⇒ 等待被跳过、进度超前（实测 f=2120 起提前 ~30 帧）。 */
    uint32_t cost = 4;
    switch (cmd) {
    case GX_CMD_SWAP_BUFFERS:  cost = 254; break;
    case GX_CMD_MTX_LOAD_4x4:
    case GX_CMD_MTX_MULT_4x4:
    case GX_CMD_MTX_MULT_4x3:  cost = 35; break;
    case GX_CMD_MTX_MULT_3x3:
    case GX_CMD_MTX_SCALE:
    case GX_CMD_MTX_TRANS:     cost = 17; break;
    case GX_CMD_MTX_LOAD_4x3:  cost = 16; break;
    case GX_CMD_VTX_16:
    case GX_CMD_VTX_10:
    case GX_CMD_VTX_XY:
    case GX_CMD_VTX_XZ:
    case GX_CMD_VTX_YZ:
    case GX_CMD_VTX_DIFF:      cost = 18; break;
    case GX_CMD_BEGIN_VTXS:
    case GX_CMD_END_VTXS:      cost = 3; break;
    case GX_CMD_POLYGON_ATTR:  cost = 5; break;
    default:                   cost = 4; break;
    }
    /* NDS_NOGXBUSY=1 → 关掉忙窗（A/B 对照用） */
    {
        static int nogx = -1;
        if (nogx < 0) nogx = (getenv("NDS_NOGXBUSY") != NULL) ? 1 : 0;
        if (!nogx) {
            g->busy_cycles += cost;
            g->gxstat |= GXSTAT_BUSY;
        }
    }
    switch (cmd) {
    case GX_CMD_MTX_MODE: g->mt_mode = p[0] & 3; break;
    case GX_CMD_MTX_IDENTITY: gx_mat_identity(m); break;
    case GX_CMD_MTX_PUSH:
        if (g->sp < 32) { gx_mat_copy(g->stack[g->sp], m); g->sp++; }
        break;
    case GX_CMD_MTX_POP:
        if (p[0] > 0 && p[0] <= g->sp) { g->sp -= p[0]; gx_mat_copy(m, g->stack[g->sp]); }
        break;
    case GX_CMD_MTX_STORE:
        if (p[0] >= 0 && p[0] < 32) gx_mat_copy(g->stack[p[0]], m);
        break;
    case GX_CMD_MTX_RESTORE:
        if (p[0] >= 0 && p[0] < 32) gx_mat_copy(m, g->stack[p[0]]);
        break;
    case GX_CMD_MTX_LOAD_4x4: gx_load_colmajor(m, p, 4); break;
    case GX_CMD_MTX_LOAD_4x3: gx_load_colmajor(m, p, 3); break;
    case GX_CMD_MTX_MULT_4x4: gx_load_colmajor(tmp, p, 4); gx_mat_mul(m, m, tmp); break;
    case GX_CMD_MTX_MULT_4x3: gx_load_colmajor(tmp, p, 3); gx_mat_mul(m, m, tmp); break;
    case GX_CMD_MTX_MULT_3x3:
        gx_mat_identity(tmp);
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                tmp[j * 4 + i] = p[i * 3 + j];
        gx_mat_mul(m, m, tmp);
        break;
    case GX_CMD_MTX_SCALE:
        gx_mat_identity(tmp);
        tmp[0] = p[0]; tmp[5] = p[1]; tmp[10] = p[2];
        gx_mat_mul(m, m, tmp);
        break;
    case GX_CMD_MTX_TRANS:
        gx_mat_identity(tmp);
        tmp[12] = p[0]; tmp[13] = p[1]; tmp[14] = p[2];
        gx_mat_mul(m, m, tmp);
        break;
    case GX_CMD_COLOR: g->color = (uint32_t)(p[0] & 0xFFFF); break;
    case GX_CMD_NORMAL: break; /* 无光照，忽略法线 */
    case GX_CMD_TEXCOORD:
        g->tc_s = (int32_t)(int16_t)(p[0] & 0xFFFF);
        g->tc_t = (int32_t)(int16_t)(p[0] >> 16);
        break;
    case GX_CMD_VTX_16:
        gx_submit_vertex(g,
            (int32_t)(int16_t)(p[0] & 0xFFFF),
            (int32_t)(int16_t)(p[0] >> 16),
            (int32_t)(int16_t)(p[1] & 0xFFFF));
        break;
    case GX_CMD_VTX_10:
        gx_submit_vertex(g,
            gx_sext10(p[0] & 0x3FF) << 2,
            gx_sext10((p[0] >> 10) & 0x3FF) << 2,
            gx_sext10((p[0] >> 20) & 0x3FF) << 2);
        break;
    case GX_CMD_VTX_XY:
        gx_submit_vertex(g,
            (int32_t)(int16_t)(p[0] & 0xFFFF),
            (int32_t)(int16_t)(p[0] >> 16),
            g->pz);
        break;
    case GX_CMD_VTX_XZ:
        gx_submit_vertex(g,
            (int32_t)(int16_t)(p[0] & 0xFFFF),
            g->py,
            (int32_t)(int16_t)(p[0] >> 16));
        break;
    case GX_CMD_VTX_YZ:
        gx_submit_vertex(g, g->px,
            (int32_t)(int16_t)(p[0] & 0xFFFF),
            (int32_t)(int16_t)(p[0] >> 16));
        break;
    case GX_CMD_VTX_DIFF:
        gx_submit_vertex(g,
            g->px + (gx_sext10(p[0] & 0x3FF) << 2),
            g->py + (gx_sext10((p[0] >> 10) & 0x3FF) << 2),
            g->pz + (gx_sext10((p[0] >> 20) & 0x3FF) << 2));
        break;
    case GX_CMD_POLYGON_ATTR: break; /* 平色简化：忽略属性 */
    case GX_CMD_BEGIN_VTXS:
        g->begin_prim = p[0] & 3;
        g->in_vtxs = 1;
        g->vcount = 0;
        break;
    case GX_CMD_END_VTXS:
        g->in_vtxs = 0;
        gx_emit_prims(g);
        break;
    case GX_CMD_SWAP_BUFFERS: break; /* 即时光栅化，无交换缓冲 */
    case GX_CMD_VIEWPORT:
        g->vx1 = p[0] & 0xFF;
        g->vy1 = (p[0] >> 8) & 0xFF;
        g->vx2 = (p[0] >> 16) & 0xFF;
        g->vy2 = (p[0] >> 24) & 0xFF;
        break;
    default: break;
    }
}

/* ---- 对外接口 ---- */

int gx_is_addr(uint32_t addr)
{
    if (addr >= IO_DISP3DCNT && addr < IO_DISP3DCNT + 4)
        return 1;
    if (addr >= GX_GXSTAT && addr < GX_REGION_END)
        return 1;
    return 0;
}

uint8_t gx_read8(const gx_t *g, uint32_t addr)
{
    if (addr >= IO_DISP3DCNT && addr < IO_DISP3DCNT + 4)
        return (uint8_t)(g->disp3dcnt >> ((addr - IO_DISP3DCNT) * 8));
    if (addr >= GX_GXSTAT && addr < GX_GXSTAT + 4)
        return (uint8_t)(g->gxstat >> ((addr - GX_GXSTAT) * 8));
    /* RAM_COUNT：简化返回 0（未统计顶点/多边形缓冲占用） */
    return 0;
}

void gx_write8(gx_t *g, uint32_t addr, uint8_t val)
{
    if (addr >= IO_DISP3DCNT && addr < IO_DISP3DCNT + 4) {
        int shift = (int)((addr - IO_DISP3DCNT) * 8);
        /* 21-B9xw：DISP3DCNT 语义对齐 melonDS GPU3D::Write16/32——
           DispCnt = (val & 0x4FFF) | (DispCnt & 0x3000)；其中 bit12/13
           是**写 1 清零**位（写 0 不动它们）。本地此前按普通字节写处理，
           结果 bit12/13 一直是 1（帧 1900 实测本机 0x3011、参考核 0x0011）。 */
        uint32_t word = g->disp3dcnt;
        if (shift == 0) {
            word = (word & ~0x00FFu) | (uint32_t)val;
        } else if (shift == 8) {
            word = (word & ~0x0F00u) | ((uint32_t)(val & 0x0Fu) << 8);
            word = (word & ~0x3000u) | (g->disp3dcnt & 0x3000u);
            if (val & 0x10u) word &= ~(1u << 12);
            if (val & 0x20u) word &= ~(1u << 13);
        }
        g->disp3dcnt = word;
    }
    /* GXSTAT 0x04000603：bit30-31 为 FIFO IRQ 模式（melonDS Write8 口径），
       0x04000601 bit7 清矩阵栈复位标志；RAM_COUNT 保持只读。 */
    if (addr == GX_GXSTAT + 1 && (val & 0x80u)) {
        g->gxstat &= ~0x8000u;
        return;
    }
    if (addr == GX_GXSTAT + 3) {
        uint32_t mode = (uint32_t)(val & 0xC0u) << 24;
        g->gxstat = (g->gxstat & ~GXSTAT_IRQ_MODE) | mode;
        return;
    }
}

void gx_write32(gx_t *g, uint32_t addr, uint32_t val)
{
    if (addr >= GX_GXFIFO && addr < GX_GXFIFO_END) {
        g->fifo_writes++;
        if (g_pending == 0) {
            /* 命令字：低 4 字节各一条命令（0=NOP） */
            for (int i = 0; i < 4; i++) {
                uint8_t cmd = (uint8_t)((val >> (i * 8)) & 0xFF);
                if (cmd)
                    gx_enqueue(g, cmd);
            }
        } else {
            /* 参数字 */
            gx_feed_param(g, (int32_t)val);
        }
    } else if (addr >= GX_CMD_PORT_BASE && addr < GX_CMD_PORT_END) {
        g->port_writes++;
        /* 命令端口：地址低字节编码命令码，写入值是该命令的唯一参数 */
        uint8_t cmd = (uint8_t)((addr - GX_GXFIFO) >> 2);
        gx_enqueue(g, cmd);
        gx_feed_param(g, (int32_t)val);
    }
}

void gx_reset(gx_t *g)
{
    memset(g, 0, sizeof(*g));
    /* 21-B9wy：参考核 GPU3D::Read32(0x04000600) 的读数里，
       fifolevel==0 时同时置 bit25（不足半满）与 bit26（空）。游戏常靠 bit25
       判断“还能往 FIFO 塞命令”，缺了它会一直等，显示列表永远发不出去。 */
    g->gxstat = GXSTAT_FIFO_EMPTY | GXSTAT_FIFO_LESS_HALF;
    gx_mat_identity(g->proj);
    gx_mat_identity(g->pos);
    gx_mat_identity(g->tex);
    for (int i = 0; i < 32; i++)
        gx_mat_identity(g->stack[i]);
    g->vx2 = GX_SCREEN_W - 1;
    g->vy2 = GX_SCREEN_H - 1;
    g->color = 0x7FFF;
    g_q_head = g_q_len = g_pending = 0;
}

const uint16_t *gx_framebuffer(const gx_t *g)
{
    return g->fb;
}

/* 21-B9yi(续16)：按系统时钟消耗 3D 引擎的工作周期。
   melonDS 是 `GPU3D::Run()` 用 ARM9 时间戳推进 `CycleCount`，到 0 才清
   `GXSTAT bit27`；本地在 ARM9 的每个时钟片（`io_advance_cart` 处）调用本函数，
   让「等 3D 忙」的轮询循环耗时与参考核同量级。 */
void gx_advance(gx_t *g, uint32_t cycles)
{
    if (g->busy_cycles == 0)
        return;
    if (cycles >= g->busy_cycles) {
        g->busy_cycles = 0;
        g->gxstat &= ~GXSTAT_BUSY;
    } else {
        g->busy_cycles -= cycles;
    }
}
