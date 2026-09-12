#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "gx.h"
#include "bus/bus.h"   /* 21-B9yi(续34)：纹理/纹理调色板从 VRAM 取数 */

/* 21-B9yi(续35) 诊断：当前帧号（定义在 io.c，runner 每帧更新，只读）。 */
extern unsigned long long g_dbg_frame;

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

/* 21-B9yi(续32)：命令直方图（按 opcode）。用来判断游戏是否真的提交几何：
   0x40 BEGIN_VTXS / 0x23-0x28 顶点的计数为 0 就意味着 3D 管线没被用到。 */
static unsigned long long g_gx_cmd_hist[256];

void gx_cmd_hist_dump(void)
{
    printf("gxhist:");
    for (int c = 0; c < 256; c++) {
        if (g_gx_cmd_hist[c] != 0)
            printf(" %02X=%llu", c, g_gx_cmd_hist[c]);
    }
    printf("\n");
}

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
    case GX_CMD_TEXIMAGE_PARAM: return 1;
    case GX_CMD_PLTT_BASE:    return 1;
    case GX_CMD_DIF_AMB:      return 1;
    case GX_CMD_SPE_EMI:      return 1;
    case GX_CMD_LIGHT_VECTOR: return 1;
    case GX_CMD_LIGHT_COLOR:  return 1;
    /* 21-B9yi(续32)：SHININESS 是**唯一**的多字命令之一（32 个参数）——
       漏掉它会让后面 32 个参数字被当成命令字解析，命令流从此错位，
       队头永久卡在「参数收不满」上，3D 管线一个三角形都画不出来。 */
    case GX_CMD_SHININESS:    return 32;
    case GX_CMD_BEGIN_VTXS:   return 1;
    case GX_CMD_END_VTXS:     return 0;
    case GX_CMD_SWAP_BUFFERS: return 1;
    case GX_CMD_VIEWPORT:     return 1;
    case GX_CMD_BOX_TEST:     return 3;
    case GX_CMD_POS_TEST:     return 2;
    case GX_CMD_VEC_TEST:     return 1;
    default:                  return 0; /* 未知命令：吞掉，不崩 */
    }
}

static void gx_exec(gx_t *g, uint8_t cmd, const int32_t *p);
static void gx_update_busy(gx_t *g);   /* 21-B9yi(续27)：前置声明 */
static void gx_entries_process(gx_t *g);   /* 21-B9yi(续33)：条目流解码 */
static void gx_clip_to_screen(const gx_t *g, int32_t cx, int32_t cy, int32_t cz,
                              int32_t cw, int *sx, int *sy,
                              int32_t *out_invw, int32_t *out_z);

/* 21-B9yi(续24)：**按命令成本消费队列**（不再「入队即执行」）。
   melonDS 的 3D 引擎按 `GPU3D::AddCycles()` 记的每命令工作周期推进
   （矩阵 16-35、顶点 18、交换缓冲 254、其余 3-5 周期），期间 `GXSTAT bit27`
   保持置位；命令只有等成本走完才真正执行。这样 FIFO 的消费速度由**命令成本**
   决定（本地此前是标定的「N 周期/字」常数），模式 7 DMA 的完成时机随之自然对齐。
   `gx_enqueue()`/`gx_feed_param()` 只入队与计字数，执行统一由 `gx_advance()` 驱动。 */
static void gx_run_due(gx_t *g, uint32_t cycles)
{
    uint32_t left = cycles;
    for (;;) {
        if (g->busy_cycles > 0) {
            if (left < g->busy_cycles) {
                g->busy_cycles -= left;
                break;
            }
            left -= g->busy_cycles;
            g->busy_cycles = 0;
        }
        if (g_q_len == 0)
            break;
        gx_pend_t *h = &g_q[g_q_head];
        if (h->filled < h->nparams) {
            /* 21-B9yi(续25) 诊断：NDS_GXDBG=1 → 队头缺参数时打印队列快照
               （定位「哪条命令的参数计数与游戏实际写入不一致」）。 */
            static int dbg = -1, n;
            if (dbg < 0) dbg = (getenv("NDS_GXDBG") != NULL) ? 1 : 0;
            if (dbg && (n++ % 200000) == 0) {
                printf("gxstuck: cmd=%02X need=%d have=%d qlen=%d pending=%d"
                       " fifo_words=%u |", h->cmd, h->nparams, h->filled,
                       g_q_len, g_pending, g->fifo_words);
                for (int qi = 0; qi < g_q_len && qi < 8; qi++) {
                    gx_pend_t *e = &g_q[(g_q_head + qi) % GX_QUEUE_CAP];
                    printf(" %02X(%d/%d)", e->cmd, e->filled, e->nparams);
                }
                printf("\n");
            }
            break;                    /* 参数还没收齐：等软件继续写 */
        }
        gx_pend_t c = *h;
        g_q_head = (g_q_head + 1) % GX_QUEUE_CAP;
        g_q_len--;
        uint32_t words = 1u + (uint32_t)c.nparams;
        g->fifo_words = (g->fifo_words >= words) ? (g->fifo_words - words) : 0u;
        g_pending = (g_pending >= (int)words) ? (g_pending - (int)words) : 0;
        gx_exec(g, c.cmd, c.params);  /* 内部按命令类型累加 busy_cycles */
    }
    /* 21-B9yi(续27)：bit27 只表达「FIFO 满 / 有未完成工作」（见 gx_update_busy）。
       —— 不再「每次入队都置忙」：游戏写长命令时是「发命令+第 1 个参数 → 等 bit27
       落 0 → 再发剩余参数」，按入队置忙会在第 1 个参数后就永久卡住。 */
    if (g->busy_cycles == 0)
        g->swap_busy = 0;      /* 管线排空：交换缓冲的忙窗结束 */
    gx_update_busy(g);
}

/* ------------------------------------------------------------------
   21-B9yi(续33)：GX 命令流的正确模型 —— **条目流**（对齐 melonDS `CmdFIFO`）

   每一次写入（命令端口 0x04000440+N*4、GXFIFO 0x04000400、或模式 7 DMA 搬进来
   的字）产生**一个条目** `{Command, Param}`：

   * 条目被当作命令字时：`Command` 是命令码，`Param` 是它的**第 1 个参数**；
   * 条目被当作参数字时：`Param` 是参数值，`Command` 字节**被忽略**。

   解码顺序就是条目顺序：取队头条目当命令，若该命令需要 n 个参数，则紧随其后的
   n-1 个条目无论来自哪个写入端口都算它的参数。

   本地旧实现把「每次端口写」都当成一条独立命令（命令 + 各自的参数），于是游戏
   用 64 次连写推的 2 条 SHININESS（各 32 参数）被拆成 64 条缺参数的 SHININESS，
   队头永远收不满 ⇒ 7300 帧里 8142 次 BEGIN_VTXS、4.1 万个顶点全部卡在队列里，
   3D 帧缓冲始终全 0（tri=0、fb-nz=0、画面缺 3D 图层）。
   ------------------------------------------------------------------ */

#define GX_ENTRY_CAP 4096

typedef struct {
    uint8_t cmd;
    uint32_t val;
} gx_entry_t;

static gx_entry_t g_ent[GX_ENTRY_CAP];
static int g_e_head = 0;
static int g_e_len = 0;
static int g_need = 0;        /* >0：正在为 g_ccmd 收集参数，还缺这么多 */
static uint8_t g_ccmd = 0;
static int32_t g_cp[32];      /* 收集中的命令的参数（最多 32：SHININESS） */
static int g_chave = 0;

/* 把一条「参数已收齐」的命令放进执行队列（按成本消费，见 gx_run_due）。 */
static void gx_push_ready(gx_t *g, uint8_t cmd, const int32_t *p, int n)
{
    gx_update_busy(g);
    if (g_q_len >= GX_QUEUE_CAP)
        return;                      /* 执行队列满：丢弃（正常流程不会发生） */
    int idx = (g_q_head + g_q_len) % GX_QUEUE_CAP;
    g_q[idx].cmd = cmd;
    g_q[idx].nparams = n;
    g_q[idx].filled = n;
    for (int i = 0; i < n && i < 16; i++)
        g_q[idx].params[i] = p[i];
    g_q_len++;
    g_pending += n + 1;              /* 占用 FIFO 空间：命令字 + 参数 */
}

static void gx_entry_push(gx_t *g, uint8_t cmd, uint32_t val)
{
    (void)g;
    if (g_e_len >= GX_ENTRY_CAP)
        return;                      /* 条目环满：丢弃（应由 FIFO 满阻塞写入方） */
    int idx = (g_e_head + g_e_len) % GX_ENTRY_CAP;
    g_ent[idx].cmd = cmd;
    g_ent[idx].val = val;
    g_e_len++;
}

static void gx_entries_process(gx_t *g)
{
    int dbg = 0;
    static long dbg_frame = -2;      /* NDS_GXDBG_FRAME=N：只 dump 该帧的前 200 条 */
    static int dbg_left = 0;
    {
        static int d = -1;
        if (d < 0) {
            d = (getenv("NDS_GXDBG") != NULL) ? 1 : 0;
            if (d) {
                const char *e = getenv("NDS_GXDBG_FRAME");
                dbg_frame = (e != NULL) ? strtol(e, NULL, 10) : -1;
                dbg_left = 200;
            }
        }
        dbg = d;
        if (dbg && dbg_frame >= 0 &&
            ((long)g_dbg_frame != dbg_frame || dbg_left <= 0))
            dbg = 0;
    }
    while (g_e_len > 0) {
        gx_entry_t e = g_ent[g_e_head];
        g_e_head = (g_e_head + 1) % GX_ENTRY_CAP;
        g_e_len--;

        if (g_need > 0) {            /* 参数字：命令字节忽略 */
            if (g_chave < 32)
                g_cp[g_chave++] = (int32_t)e.val;
            g_need--;
            if (dbg)
                printf("gxdbg: param val=%08X (%d/%d) qlen=%d\n",
                       (unsigned)e.val, g_chave, g_chave + g_need, g_q_len);
            if (g_need == 0)
                gx_push_ready(g, g_ccmd, g_cp, g_chave);
            continue;
        }

        int n = gx_cmd_nparams(e.cmd);
        if (dbg)
            printf("gxdbg: cmd=%02X nparams=%d val=%08X qlen=%d\n",
                   e.cmd, n, (unsigned)e.val, g_q_len);
        if (dbg)
            dbg_left--;
        if (e.cmd == 0)
            continue;                /* 0x00 = NOP：不计数、不入队（无副作用） */
        if (n <= 1) {                /* 0/1 参数的命令：本条就是全部 */
            g->cmd_count++;
            g_gx_cmd_hist[e.cmd]++;
            g_cp[0] = (int32_t)e.val;
            gx_push_ready(g, e.cmd, g_cp, (n == 1) ? 1 : 0);
            continue;
        }
        /* 多参数命令：本条提供第 1 个参数，其余等后续条目 */
        g->cmd_count++;
        g_gx_cmd_hist[e.cmd]++;
        g_ccmd = e.cmd;
        g_chave = 0;
        g_cp[g_chave++] = (int32_t)e.val;
        g_need = n - 1;
        break;                       /* 参数不够，等下一次写入 */
    }
}

/* 21-B9yi(续33)：GXFIFO 的 32 位写入口。
   这是 melonDS `GPU3D::WriteToGXFIFO()` 的**逐句直译**——它把一个 32 位字里的
   最多 4 个命令字节按「每次写派发一个条目」的节拍展开，因此「一个 32 位写 =
   一个条目」，条目参数取该次写入的值：

   * 字里的第一个命令字节若需要参数，则该字先只占位（`return`），它的 param0 由
     下一次写入提供；
   * 0 命令字节（NOP）只在「整个字为 0」时才派发一次，其余跳过。

   本地旧实现把「无待填参数时的一次 32 位写」直接拆成 4 条命令，把游戏用
   `[命令字][参数字]` 交替推流的方式解析错位（实测 0x20/0x7C00 这种
   「命令在前、参数在后」的写法会把参数字当成命令字）。 */
static uint32_t g_fifo_word = 0;      /* CurCommand */
static int g_fifo_ncmd = 0;           /* NumCommands */
static int g_fifo_pcnt = 0;           /* ParamCount */
static int g_fifo_need = 0;           /* TotalParams */

static void gx_fifo_word_in(gx_t *g, uint32_t val)
{
    if (g_fifo_ncmd == 0) {
        g_fifo_ncmd = 4;
        g_fifo_word = val;
        g_fifo_pcnt = 0;
        g_fifo_need = gx_cmd_nparams((uint8_t)(g_fifo_word & 0xFF));
        if (g_fifo_need > 0)
            return;                    /* 该字先占位，参数随下一次写入 */
    } else {
        g_fifo_pcnt++;
    }
    for (;;) {
        if ((g_fifo_word & 0xFF) || (g_fifo_ncmd == 4 && g_fifo_word == 0))
            gx_entry_push(g, (uint8_t)(g_fifo_word & 0xFF), val);
        if (g_fifo_pcnt >= g_fifo_need) {
            g_fifo_word >>= 8;
            g_fifo_ncmd--;
            if (g_fifo_ncmd == 0)
                break;
            g_fifo_pcnt = 0;
            g_fifo_need = gx_cmd_nparams((uint8_t)(g_fifo_word & 0xFF));
        }
        if (g_fifo_pcnt < g_fifo_need)
            break;
    }
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

/* ------------------------------------------------------------------
   21-B9yi(续34)：矩阵语义**逐句对齐 melonDS**（这块此前整体偏离，是 3D 画面
   画不出来的主因之一）。元素索引约定与 melonDS 相同：i = row*4+col，
   而命令参数**按原顺序**填入 m[0..n-1]——此前本地按「列主序」做了转置。

   对应关系（melonDS GPU3D.cpp）：
     MatrixLoad4x4 → 直接 memcpy；MatrixLoad4x3 → 前三列 + [0,0,0,1]
     MatrixMult4x4 → m = s*m；MatrixMult4x3 → m = s(4x3)*m
     MatrixMult3x3 → m = s(3x3)*m（只动左上 3x3）
     MatrixScale   → 每**行**乘以对应分量（m = S*m）
     MatrixTranslate → m[12..15] += s 经当前 m 变换后的平移
   ------------------------------------------------------------------ */

static void gx_mat_load4x4(int64_t *m, const int32_t *p)
{
    for (int i = 0; i < 16; i++)
        m[i] = p[i];
}

static void gx_mat_load4x3(int64_t *m, const int32_t *p)
{
    m[0] = p[0];  m[1] = p[1];  m[2] = p[2];   m[3] = 0;
    m[4] = p[3];  m[5] = p[4];  m[6] = p[5];   m[7] = 0;
    m[8] = p[6];  m[9] = p[7];  m[10] = p[8];  m[11] = 0;
    m[12] = p[9]; m[13] = p[10]; m[14] = p[11]; m[15] = GX_FP_ONE;
}

/* m = s * m（melonDS MatrixMult4x4 的注释与实际公式都是这个方向） */
static void gx_mat_mul_left(int64_t *m, const int64_t *s)
{
    int64_t t[16];
    memcpy(t, m, sizeof t);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            int64_t acc = 0;
            for (int k = 0; k < 4; k++)
                acc += s[i * 4 + k] * t[k * 4 + j];
            m[i * 4 + j] = acc >> GX_FP_SHIFT;
        }
}

/* m = s(4x3) * m：s 的第四列固定 [0,0,0,1] */
static void gx_mat_mul4x3_left(int64_t *m, const int64_t *s)
{
    int64_t t[16];
    memcpy(t, m, sizeof t);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++) {
            int64_t acc = 0;
            for (int k = 0; k < 3; k++)
                acc += s[i * 4 + k] * t[k * 4 + j];
            m[i * 4 + j] = acc >> GX_FP_SHIFT;
        }
    for (int j = 0; j < 4; j++)
        m[12 + j] = (s[9] * t[0 + j] + s[10] * t[4 + j] + s[11] * t[8 + j]
                     + GX_FP_ONE * t[12 + j]) >> GX_FP_SHIFT;
}

/* m = s(3x3) * m：只更新左上 3x3（melonDS MatrixMult3x3） */
static void gx_mat_mul3x3_left(int64_t *m, const int64_t *s)
{
    int64_t t[16];
    memcpy(t, m, sizeof t);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++) {
            int64_t acc = 0;
            for (int k = 0; k < 3; k++)
                acc += s[i * 4 + k] * t[k * 4 + j];
            m[i * 4 + j] = acc >> GX_FP_SHIFT;
        }
}

/* melonDS MatrixScale：m 的每一**行**乘以对应分量 */
static void gx_mat_scale(int64_t *m, const int32_t *p)
{
    /* 21-B9yi(续35)：melonDS 的 `MatrixScale` **只缩放前 3 行**（索引 0..11），
       第 4 行（12..15，含透视用的 W 分量）保持不变。本地此前连第 4 行一起缩放，
       而命令只给 3 个参数 ⇒ 第 4 行被乘上垃圾值（实测 p[3] 残留 0）：
       同一帧对比，参考核 `pos[15]=4096 / proj[15]=81171`，本地 **全是 0**，
       投影 W 整个报废 ⇒ 顶点算出视体外 ⇒ 视体裁剪后一个三角形都不剩。 */
    for (int row = 0; row < 3; row++)
        for (int j = 0; j < 4; j++)
            m[row * 4 + j] = ((int64_t)p[row] * m[row * 4 + j]) >> GX_FP_SHIFT;
}

/* melonDS MatrixTranslate：把平移量经当前 m 变换后叠加到第 4 行 */
static void gx_mat_translate(int64_t *m, const int32_t *p)
{
    for (int j = 0; j < 4; j++)
        m[12 + j] += ((int64_t)p[0] * m[0 + j] + (int64_t)p[1] * m[4 + j]
                      + (int64_t)p[2] * m[8 + j]) >> GX_FP_SHIFT;
}

/* 10 位有符号数符号扩展（VTX_10/VTX_DIFF） */
static int32_t gx_sext10(int32_t v)
{
    return (v & 0x200) ? (v | ~0x3FF) : v;
}

/* ---- 顶点变换 + 光栅化 ---- */

void gx_transform_vertex(const gx_t *g, int32_t x, int32_t y, int32_t z, int *sx, int *sy)
{
    gx_transform_vertex_ex(g, x, y, z, sx, sy, NULL);
}

/* 21-B9yi(续34)：带 w 输出的版本（纹理做透视校正插值要 1/w）。 */
void gx_transform_vertex_ex(const gx_t *g, int32_t x, int32_t y, int32_t z,
                            int *sx, int *sy, int32_t *out_invw)
{
    gx_transform_vertex_z(g, x, y, z, sx, sy, out_invw, NULL);
}

/* 21-B9yi(续34)：带 24 位深度输出的版本（口径 = melonDS `FinalZ`：
   `z = (Zc/Wc * 0x4000 + 0x3FFF) * 0x200`，钳在 0..0xFFFFFF，小 = 近）。 */
void gx_transform_vertex_z(const gx_t *g, int32_t x, int32_t y, int32_t z,
                           int *sx, int *sy, int32_t *out_invw, int32_t *out_z)
{
    int32_t clip4[4];
    gx_transform_vertex_clip(g, x, y, z, clip4);
    gx_clip_to_screen(g, clip4[0], clip4[1], clip4[2], clip4[3],
                      sx, sy, out_invw, out_z);
}

/* 裁剪空间坐标（pos × proj × v，1.19.12） */
void gx_transform_vertex_clip(const gx_t *g, int32_t x, int32_t y, int32_t z,
                              int32_t clip4[4])
{
    int64_t clip[16];
    gx_mat_mul(clip, g->pos, g->proj); /* clip = pos × proj */

    int64_t w = GX_FP_ONE;
    int64_t cx = (int64_t)x * clip[0] + (int64_t)y * clip[4] + (int64_t)z * clip[8] + w * clip[12];
    int64_t cy = (int64_t)x * clip[1] + (int64_t)y * clip[5] + (int64_t)z * clip[9] + w * clip[13];
    int64_t cz = (int64_t)x * clip[2] + (int64_t)y * clip[6] + (int64_t)z * clip[10] + w * clip[14];
    int64_t cw = (int64_t)x * clip[3] + (int64_t)y * clip[7] + (int64_t)z * clip[11] + w * clip[15];

    clip4[0] = (int32_t)cx;
    clip4[1] = (int32_t)cy;
    clip4[2] = (int32_t)cz;
    clip4[3] = (int32_t)cw;
}

/* 裁剪空间 → 屏幕坐标（透视除 + 视口 + 深度），口径见下方注释 */
static void gx_clip_to_screen(const gx_t *g, int32_t cx, int32_t cy, int32_t cz,
                              int32_t cw, int *sx, int *sy,
                              int32_t *out_invw, int32_t *out_z)
{
    int64_t nx = cx >> GX_FP_SHIFT; /* NDC（1.19.12） */
    int64_t ny = cy >> GX_FP_SHIFT;
    int64_t nz = cz >> GX_FP_SHIFT;
    int64_t nw = cw >> GX_FP_SHIFT;

    int64_t px, py;
    if (nw != 0) {
        px = (nx << GX_FP_SHIFT) / nw; /* 透视除 */
        py = (ny << GX_FP_SHIFT) / nw;
    } else {
        px = nx; /* 退化（w=0）：直接当正交用，避免除零 */
        py = ny;
    }
    if (out_invw != NULL) {
        /* 1/w（1.19.12）。w<=0（在相机后方）时退化为 0：这类顶点本来就会被
           裁剪掉，本地不做裁剪，退化为 0 至少不会把 UV 拉飞。 */
        *out_invw = (nw > 0) ? (int32_t)((GX_FP_ONE << GX_FP_SHIFT) / nw) : 0;
    }
    if (out_z != NULL) {
        int64_t zb;
        if (nw != 0)
            zb = ((nz * 0x4000) / nw) + 0x3FFF;
        else
            zb = 0x3FFF;          /* melonDS：W==0 时取 0x7FFE00（最远附近） */
        zb *= 0x200;
        if (nw == 0) zb = 0x7FFE00;
        if (zb < 0) zb = 0;
        else if (zb > 0xFFFFFF) zb = 0xFFFFFF;
        *out_z = (int32_t)zb;
    }

    /* 21-B9yi(续34)：视口映射**按 melonDS 口径**——DS 的 VIEWPORT 参数里
       y0/y1 是「屏幕坐标、0=底」的（GBATEK：y0<y1，y 轴倒置），所以屏幕行号
       要用 `191 - y1` 当上边缘、高 `y1-y0+1`；本地此前直接用 y0 当上边缘、
       高 `y2-y1`，整幅 3D 画面被垂直镜像且少一行。 */
    int vp_top = (GX_SCREEN_H - 1) - g->vy2;
    int vp_h = g->vy2 - g->vy1 + 1;
    int vp_w = g->vx2 - g->vx1 + 1;
    *sx = (int)(g->vx1 + ((px + GX_FP_ONE) * (int64_t)vp_w) / (2 * GX_FP_ONE));
    *sy = (int)(vp_top + ((GX_FP_ONE - py) * (int64_t)vp_h) / (2 * GX_FP_ONE));
    /* 视口右下边界落在「视口外一行/一列」（melonDS 用 &0x1FF/&0xFF 掩码后交给
       光栅化钳位），这里直接钳到屏幕内，保证输出一定是屏内坐标。 */
    if (*sx < 0) *sx = 0;
    else if (*sx >= GX_SCREEN_W) *sx = GX_SCREEN_W - 1;
    if (*sy < 0) *sy = 0;
    else if (*sy >= GX_SCREEN_H) *sy = GX_SCREEN_H - 1;
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
            {
                g->fb[yy * GX_SCREEN_W + xx] = color;
                /* 21-B9yi(续34)：这个公开入口画出来的是**不透明**几何，
                   必须同时写 alpha 平面，否则 3D 图层在合成时会被当成透明。 */
                g->fba[(size_t)yy * GX_SCREEN_W + xx] = 31;
            }
}

static void gx_submit_vertex(gx_t *g, int32_t x, int32_t y, int32_t z)
{
    g->px = x; g->py = y; g->pz = z;
    if (!g->in_vtxs)
        return;
    if (g->vcount >= GX_MAX_VERTS)
        return;
    int sx, sy;
    int32_t invw = 0;
    int32_t dz = 0xFFFFFF;
    int32_t clip4[4];
    gx_transform_vertex_clip(g, x, y, z, clip4);
    gx_clip_to_screen(g, clip4[0], clip4[1], clip4[2], clip4[3], &sx, &sy, &invw, &dz);
    g->verts[g->vcount].sx = sx;
    g->verts[g->vcount].sy = sy;
    g->verts[g->vcount].color = (uint16_t)(g->color & 0xFFFF);
    g->verts[g->vcount].u = g->tc_s;
    g->verts[g->vcount].v = g->tc_t;
    g->verts[g->vcount].invw = invw;
    g->verts[g->vcount].z = dz;
    g->verts[g->vcount].cx = clip4[0];
    g->verts[g->vcount].cy = clip4[1];
    g->verts[g->vcount].cz = clip4[2];
    g->verts[g->vcount].cw = clip4[3];
    if (invw == 0)
        g->vtx_zero_w++;      /* 21-B9yi(续34) 诊断 */
    g->vcount++;
}

/* 21-B9yi(续34)：纹理采样（口径对齐 melonDS `SoftRenderer3D::TextureLookup`）。
   texparam = TEXIMAGE_PARAM，texpal = PLTT_BASE；s/t 为 1.11.4 定点。
   返回 RGB555 颜色，*alpha 为 0..31（0 = 全透明）。 */
static uint16_t gx_tex_lookup(const gx_t *g, uint32_t texparam, uint32_t texpal,
                              int32_t s, int32_t t, uint8_t *alpha)
{
    s >>= 4;
    t >>= 4;
    int32_t width = 8 << ((texparam >> 20) & 0x7);
    int32_t height = 8 << ((texparam >> 23) & 0x7);

    /* 环绕/翻转（bit16/18 = S 方向环绕/镜像，bit17/19 = T 方向） */
    if (texparam & (1u << 16)) {
        if (texparam & (1u << 18)) {
            if (s & width) s = (width - 1) - (s & (width - 1));
            else           s = (s & (width - 1));
        } else {
            s &= width - 1;
        }
    } else {
        if (s < 0) s = 0;
        else if (s >= width) s = width - 1;
    }
    if (texparam & (1u << 17)) {
        if (texparam & (1u << 19)) {
            if (t & height) t = (height - 1) - (t & (height - 1));
            else            t = (t & (height - 1));
        } else {
            t &= height - 1;
        }
    } else {
        if (t < 0) t = 0;
        else if (t >= height) t = height - 1;
    }

    uint32_t vramaddr = (texparam & 0xFFFFu) << 3;
    uint8_t alpha0 = (texparam & (1u << 29)) ? 0 : 31;
    uint16_t color = 0;
    uint8_t a = 31;
    uint32_t pal;

    switch ((texparam >> 26) & 0x7u) {
    case 1: /* A3I5：8 位（低 5 位调色板索引 + 3 位 alpha） */
        vramaddr += (uint32_t)(t * width + s);
        {
            uint8_t pixel = bus_vram_tex8(g->bus, vramaddr);
            pal = (texpal << 4) + ((uint32_t)(pixel & 0x1Fu) << 1);
            color = bus_vram_texpal16(g->bus, pal);
            a = (uint8_t)(((pixel >> 3) & 0x1Cu) + (pixel >> 6));
        }
        break;
    case 2: /* 4 色（2bpp） */
        vramaddr += (uint32_t)((t * width + s) >> 2);
        {
            uint8_t pixel = bus_vram_tex8(g->bus, vramaddr);
            pixel >>= ((s & 0x3) << 1);
            pixel &= 0x3;
            color = bus_vram_texpal16(g->bus, (texpal << 3) + ((uint32_t)pixel << 1));
            a = (pixel == 0) ? alpha0 : 31;
        }
        break;
    case 3: /* 16 色（4bpp） */
        vramaddr += (uint32_t)((t * width + s) >> 1);
        {
            uint8_t pixel = bus_vram_tex8(g->bus, vramaddr);
            if (s & 1) pixel >>= 4;
            else       pixel &= 0xF;
            color = bus_vram_texpal16(g->bus, (texpal << 4) + ((uint32_t)pixel << 1));
            a = (pixel == 0) ? alpha0 : 31;
        }
        break;
    case 4: /* 256 色（8bpp） */
        vramaddr += (uint32_t)(t * width + s);
        {
            uint8_t pixel = bus_vram_tex8(g->bus, vramaddr);
            color = bus_vram_texpal16(g->bus, (texpal << 4) + ((uint32_t)pixel << 1));
            a = (pixel == 0) ? alpha0 : 31;
        }
        break;
    case 5: /* 4x4 压缩（块内 2bpp，调色板信息在槽 1） */
        vramaddr += (uint32_t)((t & 0x3FC) * (width >> 2)) + (uint32_t)(s & 0x3FC);
        vramaddr += (uint32_t)(t & 0x3);
        vramaddr &= 0x7FFFFu;
        {
            uint32_t slot1addr = 0x20000u + ((vramaddr & 0x1FFFCu) >> 1);
            if (vramaddr >= 0x40000u) slot1addr += 0x10000u;
            uint8_t val;
            if (vramaddr >= 0x20000u && vramaddr < 0x40000u) val = 0;
            else {
                val = bus_vram_tex8(g->bus, vramaddr);
                val >>= (2 * (s & 0x3));
            }
            val &= 0x3;
            {
                uint16_t palinfo = (uint16_t)(bus_vram_tex8(g->bus, slot1addr)
                                   | (uint16_t)(bus_vram_tex8(g->bus, slot1addr + 1u) << 8));
                uint32_t paloffset = (uint32_t)(palinfo & 0x3FFFu) << 2;
                uint32_t idx = (uint32_t)((palinfo >> 14) & 0x3u) + (uint32_t)val;
                color = bus_vram_texpal16(g->bus, (texpal << 4) + paloffset
                                          + (idx << 1));
                a = (idx == 0) ? alpha0 : 31;
            }
        }
        break;
    case 6: /* A5I3：8 位（低 3 位索引 + 5 位 alpha） */
        vramaddr += (uint32_t)(t * width + s);
        {
            uint8_t pixel = bus_vram_tex8(g->bus, vramaddr);
            color = bus_vram_texpal16(g->bus, (texpal << 3) + ((uint32_t)(pixel & 0x7u) << 1));
            a = (uint8_t)(pixel >> 3);
        }
        break;
    case 7: /* 直色 16 位 */
        vramaddr += (uint32_t)((t * width + s) << 1);
        {
            uint16_t pixel = (uint16_t)(bus_vram_tex8(g->bus, vramaddr)
                             | (uint16_t)(bus_vram_tex8(g->bus, vramaddr + 1u) << 8));
            color = pixel & 0x7FFFu;
            a = (pixel & 0x8000u) ? 31 : 0;
        }
        break;
    default: /* 0 = 无纹理（由调用方保证不会走到这里） */
        break;
    }
    if (alpha != NULL) *alpha = a;
    return color;
}

/* 多边形 alpha（POLYGON_ATTR bits16-20）与纹理 alpha 相乘。 */
static uint8_t gx_poly_alpha(const gx_t *g)
{
    uint8_t pa = (uint8_t)((g->poly_attr >> 16) & 0x1Fu);
    return (pa == 0) ? 31 : pa;   /* 0 = 不透明（DS 里 0 表示不混合） */
}

/* ------------------------------------------------------------------
   21-B9yi(续35)：视体裁剪（melonDS `ClipPolygon` 的最小可用版本）

   对每个平面（X、Y、Z 各两侧，共 6 个）做 Sutherland–Hodgman：
   平面判据 `d = ±Position[comp] + Position[3] >= 0`（即 |comp| <= W），
   跨越平面时按 `t = da / (da - db)` 线性插值**所有属性**（裁剪空间坐标与纹理坐标）。

   真机的 3D 光栅化只画视体内可见的部分；不做这一步，跨相机的多边形会被
   「透视除 + 钳位」拉成铺满屏幕的巨大楔形——实测 f≥3600 的画面就是那种色带。
   ------------------------------------------------------------------ */

typedef struct {
    int32_t cx, cy, cz, cw;
    int32_t u, v;
} gx_clipv_t;

static int32_t gx_clip_dist(const gx_clipv_t *v, int comp, int sign)
{
    int32_t p = (comp == 0) ? v->cx : (comp == 1) ? v->cy : v->cz;
    /* sign = +1：p + w >= 0（p >= -w）；sign = -1：-p + w >= 0（p <= w） */
    return (int32_t)(sign * (int64_t)p + v->cw);
}

static int gx_clip_plane(const gx_clipv_t *in, int n, int comp, int sign,
                         gx_clipv_t *out)
{
    int m = 0;
    for (int i = 0; i < n; i++) {
        const gx_clipv_t *a = &in[i];
        const gx_clipv_t *b = &in[(i + 1) % n];
        int32_t da = gx_clip_dist(a, comp, sign);
        int32_t db = gx_clip_dist(b, comp, sign);
        if (da >= 0)
            out[m++] = *a;
        if ((da >= 0) != (db >= 0)) {
            int64_t den = (int64_t)da - (int64_t)db;
            if (den != 0) {
                int64_t t = ((int64_t)da << 16) / den;   /* 0..1，1.16 定点 */
                gx_clipv_t *o = &out[m++];
                o->cx = a->cx + (int32_t)(((int64_t)(b->cx - a->cx) * t) >> 16);
                o->cy = a->cy + (int32_t)(((int64_t)(b->cy - a->cy) * t) >> 16);
                o->cz = a->cz + (int32_t)(((int64_t)(b->cz - a->cz) * t) >> 16);
                o->cw = a->cw + (int32_t)(((int64_t)(b->cw - a->cw) * t) >> 16);
                o->u = a->u + (int32_t)(((int64_t)(b->u - a->u) * t) >> 16);
                o->v = a->v + (int32_t)(((int64_t)(b->v - a->v) * t) >> 16);
            }
        }
    }
    return m;
}

/* 把裁剪后的图元（扇形三角化）真正光栅化。顶点属性已保证在视体内。 */
static void gx_raster_clipped_tri(gx_t *g, const gx_clipv_t *a,
                                  const gx_clipv_t *b, const gx_clipv_t *c);
static void gx_raster_vert_tri_raw(gx_t *g, const gx_vertex_t *a,
                                   const gx_vertex_t *b, const gx_vertex_t *c);

static void gx_raster_vert_tri(gx_t *g, const gx_vertex_t *a, const gx_vertex_t *b,
                               const gx_vertex_t *c)
{
    /* 21-B9yi(续35) 诊断：`NDS_POLYDBG=1` 时，帧号 >= 3800 后打印前 6 个三角形的
       裁剪空间坐标与当前矩阵（定位「为什么全被视体裁剪掉」）。 */
    {
        static int dbg = -1, shown;
        if (dbg < 0) dbg = (getenv("NDS_POLYDBG") != NULL) ? 1 : 0;
        if (dbg && shown < 6 && g_dbg_frame >= 3800) {
            printf("polydbg: f=%llu clip a=(%d,%d,%d,%d) b=(%d,%d,%d,%d)"
                   " c=(%d,%d,%d,%d)\n", g_dbg_frame,
                   a->cx, a->cy, a->cz, a->cw, b->cx, b->cy, b->cz, b->cw,
                   c->cx, c->cy, c->cz, c->cw);
            if (shown == 0) {
                printf("polydbg: viewport=(%d,%d)-(%d,%d) polyattr=%08X disp3dcnt=%08X\n",
                       g->vx1, g->vy1, g->vx2, g->vy2, g->poly_attr, g->disp3dcnt);
                printf("polydbg: proj=%lld,%lld,%lld,%lld / %lld,%lld,%lld,%lld\n",
                       (long long)g->proj[0], (long long)g->proj[1],
                       (long long)g->proj[2], (long long)g->proj[3],
                       (long long)g->proj[12], (long long)g->proj[13],
                       (long long)g->proj[14], (long long)g->proj[15]);
                printf("polydbg: pos=%lld,%lld,%lld,%lld / %lld,%lld,%lld,%lld\n",
                       (long long)g->pos[0], (long long)g->pos[1],
                       (long long)g->pos[2], (long long)g->pos[3],
                       (long long)g->pos[12], (long long)g->pos[13],
                       (long long)g->pos[14], (long long)g->pos[15]);
            }
            shown++;
            fflush(stdout);
        }
    }
    /* 组装裁剪空间的三角形并逐平面裁剪 */
    gx_clipv_t poly[16], tmp[16];
    poly[0].cx = a->cx; poly[0].cy = a->cy; poly[0].cz = a->cz; poly[0].cw = a->cw;
    poly[0].u = a->u; poly[0].v = a->v;
    poly[1].cx = b->cx; poly[1].cy = b->cy; poly[1].cz = b->cz; poly[1].cw = b->cw;
    poly[1].u = b->u; poly[1].v = b->v;
    poly[2].cx = c->cx; poly[2].cy = c->cy; poly[2].cz = c->cz; poly[2].cw = c->cw;
    poly[2].u = c->u; poly[2].v = c->v;
    int n = 3;
    static const int comp_of[6] = { 0, 0, 1, 1, 2, 2 };
    static const int sign_of[6] = { 1, -1, 1, -1, 1, -1 };
    for (int pl = 0; pl < 6 && n >= 3; pl++) {
        n = gx_clip_plane(poly, n, comp_of[pl], sign_of[pl], tmp);
        if (n < 3)
            g->clip_rej[pl]++;
        for (int i = 0; i < n; i++)
            poly[i] = tmp[i];
    }
    if (n < 3) {
        g->tri_count++;
        return;                     /* 完全在视体外：丢弃 */
    }
    for (int i = 1; i + 1 < n; i++)
        gx_raster_clipped_tri(g, &poly[0], &poly[i], &poly[i + 1]);
    g->tri_count++;
}

static void gx_raster_clipped_tri(gx_t *g, const gx_clipv_t *va,
                                  const gx_clipv_t *vb, const gx_clipv_t *vc)
{
    gx_vertex_t a, b, c;
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b); memset(&c, 0, sizeof c);
    gx_clip_to_screen(g, va->cx, va->cy, va->cz, va->cw, &a.sx, &a.sy, &a.invw, &a.z);
    gx_clip_to_screen(g, vb->cx, vb->cy, vb->cz, vb->cw, &b.sx, &b.sy, &b.invw, &b.z);
    gx_clip_to_screen(g, vc->cx, vc->cy, vc->cz, vc->cw, &c.sx, &c.sy, &c.invw, &c.z);
    a.u = va->u; a.v = va->v;
    b.u = vb->u; b.v = vb->v;
    c.u = vc->u; c.v = vc->v;
    a.color = b.color = c.color = (uint16_t)(g->color & 0xFFFF);
    gx_raster_vert_tri_raw(g, &a, &b, &c);
}

/* 原 gx_raster_vert_tri 的实现（顶点已在屏幕空间且已裁剪） */
static void gx_raster_vert_tri_raw(gx_t *g, const gx_vertex_t *a,
                                   const gx_vertex_t *b, const gx_vertex_t *c)
{
    /* 21-B9yi(续34)：带纹理/透视校正插值的光栅化。
       纹理映射开启条件与 melonDS 一致：DISP3DCNT bit0（纹理映射使能）
       且 TEXIMAGE_PARAM 的格式域 ≠ 0。未开启时保持旧的平色行为
       （`gx_raster_tri`，单元测试依赖这条路径）。 */
    int use_tex = ((g->disp3dcnt & 1u) != 0)
                  && (((g->tex_param >> 26) & 0x7u) != 0u)
                  && (g->bus != NULL);
    if (!use_tex) {
        g->tri_flat++;
        gx_raster_tri(g, a->sx, a->sy, b->sx, b->sy, c->sx, c->sy, a->color);
        uint8_t pa = gx_poly_alpha(g);
        int fx0 = a->sx, fy0 = a->sy, fx1 = b->sx, fy1 = b->sy, fx2 = c->sx, fy2 = c->sy;
        int fminx = fx0 < fx1 ? fx0 : fx1; fminx = fminx < fx2 ? fminx : fx2;
        int fmaxx = fx0 > fx1 ? fx0 : fx1; fmaxx = fmaxx > fx2 ? fmaxx : fx2;
        int fminy = fy0 < fy1 ? fy0 : fy1; fminy = fminy < fy2 ? fminy : fy2;
        int fmaxy = fy0 > fy1 ? fy0 : fy1; fmaxy = fmaxy > fy2 ? fmaxy : fy2;
        if (fminx < 0) fminx = 0;
        if (fminy < 0) fminy = 0;
        if (fmaxx >= GX_SCREEN_W) fmaxx = GX_SCREEN_W - 1;
        if (fmaxy >= GX_SCREEN_H) fmaxy = GX_SCREEN_H - 1;
        /* 平色路径也要写 alpha 平面（3D 图层合成看的就是它） */
        uint32_t drew = 0;
        for (int yy = fminy; yy <= fmaxy; yy++)
            for (int xx = fminx; xx <= fmaxx; xx++)
                if (gx_point_in_tri(xx, yy, fx0, fy0, fx1, fy1, fx2, fy2)) {
                    g->fba[(size_t)yy * GX_SCREEN_W + xx] = pa;
                    drew++;
                }
        g->px_written += drew;
        if (drew != 0)
            g->tri_drawn++;
        g->tri_count++;
        return;
    }
    g->tri_tex++;

    int x0 = a->sx, y0 = a->sy, x1 = b->sx, y1 = b->sy, x2 = c->sx, y2 = c->sy;
    int area = gx_edge(x0, y0, x1, y1, x2, y2);
    if (area == 0) { g->tri_count++; return; }   /* 退化三角形 */
    int minx = x0 < x1 ? x0 : x1; minx = minx < x2 ? minx : x2;
    int maxx = x0 > x1 ? x0 : x1; maxx = maxx > x2 ? maxx : x2;
    int miny = y0 < y1 ? y0 : y1; miny = miny < y2 ? miny : y2;
    int maxy = y0 > y1 ? y0 : y1; maxy = maxy > y2 ? maxy : y2;
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= GX_SCREEN_W) maxx = GX_SCREEN_W - 1;
    if (maxy >= GX_SCREEN_H) maxy = GX_SCREEN_H - 1;

    uint8_t palpha = gx_poly_alpha(g);
    uint32_t drew = 0;
    for (int yy = miny; yy <= maxy; yy++) {
        for (int xx = minx; xx <= maxx; xx++) {
            if (!gx_point_in_tri(xx, yy, x0, y0, x1, y1, x2, y2))
                continue;
            /* 重心坐标（面积比） */
            int w0 = gx_edge(x1, y1, x2, y2, xx, yy);
            int w1 = gx_edge(x2, y2, x0, y0, xx, yy);
            int w2 = gx_edge(x0, y0, x1, y1, xx, yy);
            int32_t iw = (int32_t)(((int64_t)w0 * a->invw + (int64_t)w1 * b->invw
                                    + (int64_t)w2 * c->invw) / area);
            /* 透视校正：u/w、v/w 线性插值后再除以插值出的 1/w */
            int64_t uw = ((int64_t)w0 * ((int64_t)a->u * a->invw)
                          + (int64_t)w1 * ((int64_t)b->u * b->invw)
                          + (int64_t)w2 * ((int64_t)c->u * c->invw)) / area;
            int64_t vw = ((int64_t)w0 * ((int64_t)a->v * a->invw)
                          + (int64_t)w1 * ((int64_t)b->v * b->invw)
                          + (int64_t)w2 * ((int64_t)c->v * c->invw)) / area;
            int32_t u = (iw != 0) ? (int32_t)(uw / iw) : (int32_t)(uw >> GX_FP_SHIFT);
            int32_t v = (iw != 0) ? (int32_t)(vw / iw) : (int32_t)(vw >> GX_FP_SHIFT);
            /* 深度：Zc/Wc 在屏幕空间线性（透视校正后），直接重心插值 */
            int32_t pz = (int32_t)(((int64_t)w0 * a->z + (int64_t)w1 * b->z
                                    + (int64_t)w2 * c->z) / area);
            if (pz < 0) pz = 0;
            else if (pz > 0xFFFFFF) pz = 0xFFFFFF;
            size_t pi = (size_t)yy * GX_SCREEN_W + xx;
            /* melonDS 深度测试：新片元更近（更小的 z）才通过；bit14 = 相等测试 */
            if (g->poly_attr & (1u << 14)) {
                int32_t diff = (int32_t)g->zbuf[pi] - pz;
                if (!((uint32_t)(diff + 0x200) <= 0x400u))
                    continue;
            } else if (pz >= (int32_t)g->zbuf[pi]) {
                continue;
            }

            uint8_t talpha = 31;
            uint16_t col = gx_tex_lookup(g, g->tex_param, g->pltt_base, u, v, &talpha);
            if (talpha == 0)
                continue;                    /* 全透明：不写，露出下面图层 */
            uint8_t aa = (uint8_t)(((unsigned)talpha * (unsigned)palpha) / 31u);
            if (aa == 0)
                continue;
            g->fb[pi] = col;
            g->fba[pi] = aa;
            g->zbuf[pi] = (uint32_t)pz;
            drew++;
        }
    }
    g->px_written += drew;
    if (drew != 0)
        g->tri_drawn++;
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
    g->exec_hist[cmd]++;
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
            /* 21-B9yi(续27)：只有交换缓冲才进入「管线忙」（melonDS 口径），
               其余命令只累加工作周期。 */
            if (cmd == GX_CMD_SWAP_BUFFERS) {
                g->swap_busy = 1;
                g->gxstat |= GXSTAT_BUSY;
            }
        }
    }
    switch (cmd) {
    case GX_CMD_MTX_MODE: g->mt_mode = p[0] & 3; break;
    case GX_CMD_MTX_IDENTITY: gx_mat_identity(m); break;
    /* 21-B9yi(续35)：矩阵栈语义逐句对齐 melonDS（投影/纹理单槽、位置 32 槽，
       POP 的参数是**带符号偏移**、从栈指针里减掉）。 */
    case GX_CMD_MTX_PUSH:
        if (g->mt_mode == 0) {
            if (g->proj_sp > 0)
                g->gxstat |= (1u << 15);        /* 栈溢出标志（melonDS 口径） */
            gx_mat_copy(g->proj_stack, g->proj);
            g->proj_sp = (g->proj_sp + 1) & 1;
        } else if (g->mt_mode == 3) {
            if (g->tex_sp > 0)
                g->gxstat |= (1u << 15);
            gx_mat_copy(g->tex_stack, g->tex);
            g->tex_sp = (g->tex_sp + 1) & 1;
        } else {
            if (g->pos_sp > 30)
                g->gxstat |= (1u << 15);
            gx_mat_copy(g->pos_stack[g->pos_sp & 0x1F], g->pos);
            g->pos_sp = (g->pos_sp + 1) & 0x3F;
        }
        break;
    case GX_CMD_MTX_POP:
        if (g->mt_mode == 0) {
            if (g->proj_sp == 0)
                g->gxstat |= (1u << 15);
            g->proj_sp = (g->proj_sp - 1) & 1;
            gx_mat_copy(g->proj, g->proj_stack);
        } else if (g->mt_mode == 3) {
            if (g->tex_sp == 0)
                g->gxstat |= (1u << 15);
            g->tex_sp = (g->tex_sp - 1) & 1;
            gx_mat_copy(g->tex, g->tex_stack);
        } else {
            int32_t offset = (int32_t)((uint32_t)p[0] << 26) >> 26;  /* 6 位带符号 */
            g->pos_sp = (g->pos_sp - offset) & 0x3F;
            if (g->pos_sp > 30)
                g->gxstat |= (1u << 15);
            gx_mat_copy(g->pos, g->pos_stack[g->pos_sp & 0x1F]);
        }
        break;
    case GX_CMD_MTX_STORE:
        if (g->mt_mode == 0) {
            gx_mat_copy(g->proj_stack, g->proj);
        } else if (g->mt_mode == 3) {
            gx_mat_copy(g->tex_stack, g->tex);
        } else {
            gx_mat_copy(g->pos_stack[p[0] & 0x1F], g->pos);
        }
        break;
    case GX_CMD_MTX_RESTORE:
        if (g->mt_mode == 0) {
            gx_mat_copy(g->proj, g->proj_stack);
        } else if (g->mt_mode == 3) {
            gx_mat_copy(g->tex, g->tex_stack);
        } else {
            gx_mat_copy(g->pos, g->pos_stack[p[0] & 0x1F]);
        }
        break;
    case GX_CMD_MTX_LOAD_4x4: gx_mat_load4x4(m, p); break;
    case GX_CMD_MTX_LOAD_4x3: gx_mat_load4x3(m, p); break;
    case GX_CMD_MTX_MULT_4x4:
        gx_mat_load4x4(tmp, p);
        gx_mat_mul_left(m, tmp);
        break;
    case GX_CMD_MTX_MULT_4x3:
        gx_mat_load4x3(tmp, p);
        gx_mat_mul4x3_left(m, tmp);
        break;
    case GX_CMD_MTX_MULT_3x3:
        gx_mat_identity(tmp);
        /* melonDS 把 9 个参数按行填入 s[0..8]（第四列固定 [0,0,0,1]） */
        for (int i = 0; i < 9; i++)
            tmp[(i / 3) * 4 + (i % 3)] = p[i];
        gx_mat_mul3x3_left(m, tmp);
        break;
    case GX_CMD_MTX_SCALE: gx_mat_scale(m, p); break;
    case GX_CMD_MTX_TRANS: gx_mat_translate(m, p); break;
    case GX_CMD_COLOR: g->color = (uint32_t)(p[0] & 0xFFFF); break;
    case GX_CMD_NORMAL: break; /* 无光照，忽略法线 */
    case GX_CMD_TEXCOORD:
        {
            int32_t rs = (int32_t)(p[0] & 0xFFFF);
            int32_t rt = (int32_t)((uint32_t)p[0] >> 16);
            /* 21-B9yi(续34)：TEXIMAGE_PARAM bit30 = 纹理坐标变换使能
               （melonDS：TexCoords = RawTexCoords × TexMatrix，1.19.12 → >>12）。
               游戏实测该位为 1，此前不做变换 ⇒ UV 全错、贴图变成大色带。 */
            if (((g->tex_param >> 30) & 1u) != 0) {
                g->tc_s = (int32_t)(((int64_t)rs * g->tex[0] + (int64_t)rt * g->tex[4]
                                     + g->tex[8] + g->tex[12]) >> GX_FP_SHIFT);
                g->tc_t = (int32_t)(((int64_t)rs * g->tex[1] + (int64_t)rt * g->tex[5]
                                     + g->tex[9] + g->tex[13]) >> GX_FP_SHIFT);
            } else {
                g->tc_s = rs;
                g->tc_t = rt;
            }
        }
        break;
    case GX_CMD_VTX_16:
        gx_submit_vertex(g,
            (int32_t)(int16_t)(p[0] & 0xFFFF),
            (int32_t)(int16_t)(p[0] >> 16),
            (int32_t)(int16_t)(p[1] & 0xFFFF));
        break;
    case GX_CMD_VTX_10:
        /* 21-B9yi(续35)：10 位坐标取**符号扩展后的原值**（melonDS：
           `CurVertex[i] = (s16)((param & 0x3FF) << 6) >> 6`，即符号扩展、不缩放）。
           本地此前 `<< 2`（放大 4 倍），VTX_DIFF（本游戏用得最多，639 万条）
           累加出来的顶点直接飞出视体 ⇒ 裁剪后一个三角形都不剩。 */
        gx_submit_vertex(g,
            gx_sext10(p[0] & 0x3FF),
            gx_sext10((p[0] >> 10) & 0x3FF),
            gx_sext10((p[0] >> 20) & 0x3FF));
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
            g->px + gx_sext10(p[0] & 0x3FF),
            g->py + gx_sext10((p[0] >> 10) & 0x3FF),
            g->pz + gx_sext10((p[0] >> 20) & 0x3FF));
        break;
    /* 21-B9yi(续34)：POLYGON_ATTR 取 alpha（bits16-20）与纹理开关 */
    case GX_CMD_POLYGON_ATTR: g->poly_attr = (uint32_t)p[0]; break;
    case GX_CMD_TEXIMAGE_PARAM: g->tex_param = (uint32_t)p[0]; break;
    case GX_CMD_PLTT_BASE: g->pltt_base = (uint32_t)p[0] & 0x1FFFu; break;
    case GX_CMD_BEGIN_VTXS:
        g->begin_prim = p[0] & 3;
        g->in_vtxs = 1;
        g->vcount = 0;
        break;
    case GX_CMD_END_VTXS:
        g->in_vtxs = 0;
        gx_emit_prims(g);
        break;
    /* 21-B9yi(续34)：交换缓冲 → 深度缓冲置最远（见 gx.h 里 zbuf 的说明）。
       颜色缓冲不清（真机也不清，靠游戏自己画清屏多边形覆盖）。 */
    case GX_CMD_SWAP_BUFFERS:
        for (size_t i = 0; i < (size_t)GX_SCREEN_W * GX_SCREEN_H; i++)
            g->zbuf[i] = 0xFFFFFFu;
        break;
    case GX_CMD_VIEWPORT:
        g->vx1 = p[0] & 0xFF;
        g->vy1 = (p[0] >> 8) & 0xFF;
        g->vx2 = (p[0] >> 16) & 0xFF;
        g->vy2 = (p[0] >> 24) & 0xFF;
        break;
    default: break;
    }
    /* 21-B9yi(续35) 诊断：NDS_MDBG=1 时，目标帧里每条矩阵命令都打印
       参数与结果矩阵的关键元素（与参考核 REF_MDBG 同口径对照）。 */
    if (cmd >= 0x10 && cmd <= 0x1C) {
        static int md = -1;
        static long mframe = -2;
        if (md < 0) {
            md = (getenv("NDS_MDBG") != NULL) ? 1 : 0;
            const char *e = getenv("NDS_MDBG_FRAME");
            mframe = (e != NULL) ? strtol(e, NULL, 10) : -1;
        }
        if (md && mframe >= 0 && (long)g_dbg_frame >= mframe
            && (long)g_dbg_frame <= mframe + 20)
            printf("mdbg: f=%llu cmd=%02X p=%08X,%08X,%08X mode=%d"
                   " proj=%lld,%lld,%lld,%lld pos=%lld,%lld,%lld,%lld\n",
                   g_dbg_frame, cmd, (unsigned)p[0], (unsigned)p[1], (unsigned)p[2],
                   g->mt_mode,
                   (long long)g->proj[0], (long long)g->proj[5],
                   (long long)g->proj[15], (long long)g->proj[3],
                   (long long)g->pos[0], (long long)g->pos[5],
                   (long long)g->pos[15], (long long)g->pos[3]);
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
        /* 21-B9yi(续22)：FIFO 里按字计数（32 位写 = 2 字），供模式 7 DMA 判断空位。 */
        g->fifo_words += 2u;
        gx_fifo_word_in(g, val);
    } else if (addr >= GX_CMD_PORT_BASE && addr < GX_CMD_PORT_END) {
        g->port_writes++;
        /* 命令端口：地址编码命令码，写入值是该命令的第一个参数（melonDS 口径：
           `CmdFIFOEntry{Command = (addr & 0x1FC) >> 2, Param = val}`）。 */
        uint8_t cmd = (uint8_t)((addr - GX_GXFIFO) >> 2);
        gx_entry_push(g, cmd, val);
    }
    gx_entries_process(g);
    gx_update_busy(g);
}

void gx_reset(gx_t *g)
{
    memset(g, 0, sizeof(*g));
    /* 21-B9yi(续34)：默认 POLYGON_ATTR 的 alpha=0（=不透明，见 gx_poly_alpha） */
    g->poly_attr = 0;
    /* 21-B9wy：参考核 GPU3D::Read32(0x04000600) 的读数里，
       fifolevel==0 时同时置 bit25（不足半满）与 bit26（空）。游戏常靠 bit25
       判断“还能往 FIFO 塞命令”，缺了它会一直等，显示列表永远发不出去。 */
    g->gxstat = GXSTAT_FIFO_EMPTY | GXSTAT_FIFO_LESS_HALF;
    gx_mat_identity(g->proj);
    gx_mat_identity(g->pos);
    gx_mat_identity(g->tex);
    gx_mat_identity(g->proj_stack);
    gx_mat_identity(g->tex_stack);
    for (int i = 0; i < 32; i++)
        gx_mat_identity(g->pos_stack[i]);
    g->proj_sp = g->tex_sp = g->pos_sp = 0;
    g->vx2 = GX_SCREEN_W - 1;
    g->vy2 = GX_SCREEN_H - 1;
    g->color = 0x7FFF;
    for (size_t i = 0; i < (size_t)GX_SCREEN_W * GX_SCREEN_H; i++)
        g->zbuf[i] = 0xFFFFFFu;      /* 深度缓冲：最远 */
    g_q_head = g_q_len = g_pending = 0;
    g_e_head = g_e_len = g_need = g_chave = 0;
    g_fifo_word = g_fifo_ncmd = g_fifo_pcnt = g_fifo_need = 0;
}

const uint16_t *gx_framebuffer(const gx_t *g)
{
    return g->fb;
}

/* 21-B9yi(续34)：3D 图层 alpha 平面（0=透明），渲染层合成 3D 图层时用。 */
const uint8_t *gx_framebuffer_alpha(const gx_t *g)
{
    return g->fba;
}

/* 21-B9yi(续34)：装配 bus（纹理/调色板取数）。测试里可不设，此时纹理路径关闭。 */
void gx_set_bus(gx_t *g, struct bus *bus)
{
    if (g != NULL)
        g->bus = bus;
}

/* 21-B9yi(续16)：按系统时钟消耗 3D 引擎的工作周期。
   melonDS 是 `GPU3D::Run()` 用 ARM9 时间戳推进 `CycleCount`，到 0 才清
   `GXSTAT bit27`；本地在 ARM9 的每个时钟片（`io_advance_cart` 处）调用本函数，
   让「等 3D 忙」的轮询循环耗时与参考核同量级。 */
void gx_advance(gx_t *g, uint32_t cycles)
{
    /* 21-B9yi(续24)：按 3D 引擎的工作周期推进命令队列（成本驱动，见 gx_run_due）。 */
    gx_run_due(g, cycles);
}

uint32_t gx_fifo_free_words(const gx_t *g)
{
    /* 21-B9yi(续27/续33)：按 melonDS 的**条目**口径（112 条），不是字数。
       待执行的命令 + 尚未解码完的条目都在占条目位置。 */
    int used = g_q_len + g_e_len + ((g_need > 0) ? 1 : 0);
    int free_entries = (int)GX_FIFO_CAP_ENTRIES - used;
    return (free_entries > 0) ? (uint32_t)free_entries : 0u;
}

/* 21-B9yi(续27)：FIFO 能否再收一个字。
   队列未满 → 可以；队列满但仍有命令缺参数 → 也可以（参数属于已有条目，
   melonDS 一样收）；只有「满且无待填参数」时才挡（此时下一个字是新命令）。 */
int gx_fifo_can_accept(const gx_t *g)
{
    int used = g_q_len + g_e_len + ((g_need > 0) ? 1 : 0);
    if (used < (int)GX_FIFO_CAP_ENTRIES)
        return 1;
    /* 满时仍可继续喂参数（参数属于已有条目），只有「下一条是新命令」才挡。 */
    return (g_need > 0 || g_e_len > 0) ? 1 : 0;
}

/* 21-B9yi(续27)：用 bit27 表达「FIFO 满 / 有未完成工作」——melonDS 里
   FIFO 满会 `GXFIFOStall()` 停住写入方，而游戏正是用「等 bit27 落 0」
   来等引擎腾空；本模型把这两件事合到同一个可观测位上。 */
static void gx_update_busy(gx_t *g)
{
    /* 21-B9yi(续27)：melonDS 全文件只有 `SWAP_BUFFERS` 分支置 bit27
       （`GXStat |= (1<<27)`），并在 `FinishWork()`（管线排空）时清掉。
       本地按同一口径：**只有交换缓冲之后的管线期间才忙**。
       注意若把「FIFO 满」也当成忙，会与游戏「发命令 + 第 1 个参数 → 等 bit27
       → 再发剩余参数」的写法互相锁死（实测 q 卡在 113、pend 744）。 */
    if (g->swap_busy && g->busy_cycles > 0)
        g->gxstat |= GXSTAT_BUSY;
    else
        g->gxstat &= ~GXSTAT_BUSY;
}

/* 21-B9yi(续25) 诊断：把 GX 队列/引擎状态暴露给 runner 的逐帧 trace
   （定位「ARM9 卡在 GXSTAT bit27 忙等、但队列其实没人推进」这类停摆）。 */
void gx_state(const gx_t *g, uint32_t *fifo_words, uint32_t *busy,
              int *qlen, int *pending)
{
    if (fifo_words) *fifo_words = g->fifo_words;
    if (busy) *busy = g->busy_cycles;
    if (qlen) *qlen = g_q_len;
    if (pending) *pending = g_pending;
}
