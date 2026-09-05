#include "fifo.h"
#include "irq.h"

/* ---- 队列基本操作（环形，16 字深） ---- */

static int q_full(const fifo_q_t *q)   { return q->count == FIFO_DEPTH; }
static int q_empty(const fifo_q_t *q)  { return q->count == 0; }

static void q_push(fifo_q_t *q, uint32_t v)
{
    if (q_full(q))
        return; /* 满：调用方负责置错误位 */
    int tail = (q->head + q->count) % FIFO_DEPTH;
    q->q[tail] = v;
    q->count++;
}

static uint32_t q_pop(fifo_q_t *q)
{
    if (q_empty(q))
        return 0; /* 空：调用方负责置错误位 */
    uint32_t v = q->q[q->head];
    q->head = (q->head + 1) % FIFO_DEPTH;
    q->count--;
    return v;
}

static void q_clear(fifo_q_t *q)
{
    q->head = 0;
    q->count = 0;
}

/* ---- 按访问者身份选队列 ---- */

fifo_q_t *fifo_send_q(ipc_fifo_t *f, int is_arm7)
{
    /* ARM9 的发送队列是 from9；ARM7 的发送队列是 from7 */
    return is_arm7 ? &f->from7 : &f->from9;
}

fifo_q_t *fifo_recv_q(ipc_fifo_t *f, int is_arm7)
{
    /* ARM9 收到的是 ARM7 写的（from7）；ARM7 收到的是 ARM9 写的（from9） */
    return is_arm7 ? &f->from9 : &f->from7;
}

static uint16_t *cnt_of(ipc_fifo_t *f, int is_arm7)
{
    return is_arm7 ? &f->cnt7 : &f->cnt9;
}

int fifo_is_cnt_addr(uint32_t addr)
{
    return addr == IO_FIFO_CNT || addr == IO_FIFO_CNT + 1;
}

int fifo_is_send_addr(uint32_t addr)
{
    return addr >= IO_FIFO_SEND && addr < IO_FIFO_SEND + 4;
}

/* ---- CNT 读写 ---- */

uint16_t fifo_cnt_read(ipc_fifo_t *f, int is_arm7)
{
    fifo_q_t *sq = fifo_send_q(f, is_arm7);
    fifo_q_t *rq = fifo_recv_q(f, is_arm7);
    uint16_t base = *cnt_of(f, is_arm7);

    /* 空满位实时算（不在 cnt 实例里存），其余位（IRQ 使能/使能/错误）读回 */
    uint16_t out = base & ~(FIFO_CNT_SEND_EMPTY | FIFO_CNT_SEND_FULL |
                            FIFO_CNT_RECV_EMPTY | FIFO_CNT_RECV_FULL);
    if (q_empty(sq)) out |= FIFO_CNT_SEND_EMPTY;
    if (q_full(sq))  out |= FIFO_CNT_SEND_FULL;
    if (q_empty(rq)) out |= FIFO_CNT_RECV_EMPTY;
    if (q_full(rq))  out |= FIFO_CNT_RECV_FULL;
    return out;
}

void fifo_cnt_write(ipc_fifo_t *f, int is_arm7, uint16_t val)
{
    uint16_t *cnt = cnt_of(f, is_arm7);

    if (val & FIFO_CNT_SEND_CLEAR)
        q_clear(fifo_send_q(f, is_arm7));      /* 清空发送队列 */

    /* 错误位（bit14）写 1 清除（应答）；其余可写位（IRQ 使能 bit2/10、使能 bit15）保留 */
    if (val & FIFO_CNT_ERROR)
        *cnt &= (uint16_t)~FIFO_CNT_ERROR;     /* 应答错误 */
    else {
        *cnt &= (uint16_t)~(FIFO_CNT_SEND_IRQ | FIFO_CNT_RECV_IRQ | FIFO_CNT_ENABLE);
        *cnt |= (uint16_t)(val & (FIFO_CNT_SEND_IRQ | FIFO_CNT_RECV_IRQ | FIFO_CNT_ENABLE));
    }
}

/* ---- 发送 / 接收 ---- */

void fifo_send(ipc_fifo_t *f, int is_arm7, uint32_t val)
{
    if (!(*cnt_of(f, is_arm7) & FIFO_CNT_ENABLE))
        return; /* 未使能：写被忽略，不置错误 */
    fifo_q_t *sq = fifo_send_q(f, is_arm7);
    if (q_full(sq))
        *cnt_of(f, is_arm7) |= FIFO_CNT_ERROR; /* 满：置错误位 */
    else
        q_push(sq, val);
}

uint32_t fifo_recv(ipc_fifo_t *f, int is_arm7)
{
    fifo_q_t *rq = fifo_recv_q(f, is_arm7);
    if (q_empty(rq)) {
        *cnt_of(f, is_arm7) |= FIFO_CNT_ERROR; /* 空：置错误位，返回 0 */
        return 0;
    }
    return q_pop(rq);
}

/* ---- FIFO 中断（边沿触发，IF17/18） ---- */

void fifo_update_irq(ipc_fifo_t *f, int is_arm7, struct irq *irq)
{
    uint16_t cnt = fifo_cnt_read(f, is_arm7);
    /* send empty 条件 = CNT.2 & CNT.0；recv not empty 条件 = CNT.10 & !CNT.8 */
    int send_empty = (cnt & FIFO_CNT_SEND_IRQ) && (cnt & FIFO_CNT_SEND_EMPTY);
    int recv_nempty = (cnt & FIFO_CNT_RECV_IRQ) && !(cnt & FIFO_CNT_RECV_EMPTY);

    int *psend = is_arm7 ? &f->prev7_send_empty : &f->prev9_send_empty;
    int *precv = is_arm7 ? &f->prev7_recv_nempty : &f->prev9_recv_nempty;

    /* 边沿 0→1 时置对应 IF 位（IF17 送空、IF18 收非空） */
    if (send_empty && !*psend)
        irq->ifl |= IO_IF_FIFO_SEND_EMPTY;
    if (recv_nempty && !*precv)
        irq->ifl |= IO_IF_FIFO_RECV_NOT_EMPTY;

    *psend = send_empty;
    *precv = recv_nempty;
}

/* ---- IPCSYNC（阶段 21-B2） ---- */

int ipc_sync_is_addr(uint32_t addr)
{
    return addr == IO_IPCSYNC || addr == IO_IPCSYNC + 1;
}

/* 按访问者拼 16 位读值：
   低 4 位 = 对端 out（对端存的 bit8-11 右移到 bit0-3）；
   高字节 = 本核存的 out/enable（bit13 只写，读回恒 0）。 */
static uint16_t ipc_sync_read16(const ipc_sync_t *s, int is_arm7)
{
    const uint16_t *loc = is_arm7 ? &s->v[1] : &s->v[0];
    const uint16_t *rem = is_arm7 ? &s->v[0] : &s->v[1];
    return (uint16_t)(((*rem & IPCSYNC_OUT_MASK) >> 8)
                    | (*loc & (IPCSYNC_OUT_MASK | IPCSYNC_ENABLE)));
}

uint8_t ipc_sync_read8(const ipc_sync_t *s, uint32_t addr, int is_arm7)
{
    uint16_t v = ipc_sync_read16(s, is_arm7);
    if (addr == IO_IPCSYNC)
        return (uint8_t)(v & 0xFFu);
    return (uint8_t)(v >> 8);
}

void ipc_sync_write8(ipc_sync_t *s, uint32_t addr, uint8_t val, int is_arm7,
                     struct irq *remote)
{
    uint16_t *loc = is_arm7 ? &s->v[1] : &s->v[0];
    const uint16_t *rem = is_arm7 ? &s->v[0] : &s->v[1];
    /* 用当前可见读值做字节合并（与 FIFO CNT 同样按 16 位小端读写） */
    uint16_t merged = ipc_sync_read16(s, is_arm7);
    if (addr == IO_IPCSYNC)
        merged = (uint16_t)((merged & 0xFF00u) | val);
    else
        merged = (uint16_t)((merged & 0x00FFu) | ((uint16_t)val << 8));

    /* 本核视图只落可写位：out(bit8-11) + enable(bit14)。
       低 4 位是对端数据（只读）、bit13 是请求（只写），都不许落盘。 */
    *loc = (uint16_t)((*loc & ~(IPCSYNC_OUT_MASK | IPCSYNC_ENABLE))
                    | (merged & (IPCSYNC_OUT_MASK | IPCSYNC_ENABLE)));

    /* 请求位在 16 位寄存器里是 bit13 → 高字节第 5 位（0x20）。
       写 1 且对端 enable=1 时置对端 IF bit16（沿触发，类似 FIFO 中断门控）。 */
    if (addr == IO_IPCSYNC + 1 && (val & 0x20u) && (*rem & IPCSYNC_ENABLE))
        remote->ifl |= IO_IF_IPC_SYNC;
}
