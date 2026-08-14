#ifndef NDS_EMU_IO_FIFO_H
#define NDS_EMU_IO_FIFO_H

#include <stdint.h>

struct irq; /* 前向声明：fifo 中断边沿检测需置对应 CPU 的 IF 位 */

/* IPC FIFO 寄存器（NDS9/NDS7 同址分流，各自独立 CNT 实例）。
   注意 RECV 地址 0x04100000 不在 0x0400xxxx IO 区间内，由 bus 单独映射。 */
#define IO_FIFO_CNT   0x04000184u   /* 16 位控制/状态 */
#define IO_FIFO_SEND  0x04000188u   /* 32 位发送（写） */
#define IO_FIFO_RECV  0x04100000u   /* 32 位接收（读） */

/* IPCFIFOCNT 位定义 */
#define FIFO_CNT_SEND_EMPTY   (1u << 0)   /* 发送队列空（1=空） */
#define FIFO_CNT_SEND_FULL    (1u << 1)   /* 发送队列满（1=满） */
#define FIFO_CNT_SEND_IRQ     (1u << 2)   /* 发送空 IRQ 使能 */
#define FIFO_CNT_SEND_CLEAR   (1u << 3)   /* 清空发送队列（写 1） */
#define FIFO_CNT_RECV_EMPTY   (1u << 8)   /* 接收队列空（1=空） */
#define FIFO_CNT_RECV_FULL    (1u << 9)   /* 接收队列满（1=满） */
#define FIFO_CNT_RECV_IRQ     (1u << 10)  /* 接收非空 IRQ 使能 */
#define FIFO_CNT_ERROR        (1u << 14)  /* 错误（读空/写满） */
#define FIFO_CNT_ENABLE       (1u << 15)  /* 使能（0 时写 SEND 被忽略） */

#define FIFO_DEPTH 16

/* 环形队列：存 32 位字。head 指向下一个要读的位置，count 是当前元素数。 */
typedef struct fifo_q {
    uint32_t q[FIFO_DEPTH];
    int head;
    int count;
} fifo_q_t;

/* IPC FIFO 完整状态：
   from9 = ARM9 写的队列（ARM7 读）；from7 = ARM7 写的队列（ARM9 读）。
   cnt9/cnt7 = 两核各自的 CNT 实例（存储 IRQ 使能/使能位；空满位实时算）。
   prev9/prev7 = 两核各自上次的「IRQ 触发条件」，用于边沿检测。 */
typedef struct ipc_fifo {
    fifo_q_t from9;
    fifo_q_t from7;
    uint16_t cnt9, cnt7;
    int prev9_send_empty, prev7_send_empty;      /* bit2 & bit0 条件 */
    int prev9_recv_nempty, prev7_recv_nempty;    /* bit10 & !bit8 条件 */
} ipc_fifo_t;

/* 按访问者身份返回其发送/接收队列。ARM9 发=from9、收=from7；ARM7 反之。 */
fifo_q_t *fifo_send_q(ipc_fifo_t *f, int is_arm7);
fifo_q_t *fifo_recv_q(ipc_fifo_t *f, int is_arm7);

int fifo_is_cnt_addr(uint32_t addr);
int fifo_is_send_addr(uint32_t addr);

/* 读 CNT 状态位（按访问者身份；空满位按队列实时计算，IRQ 使能/使能位存 cnt 实例） */
uint16_t fifo_cnt_read(ipc_fifo_t *f, int is_arm7);
/* 写 CNT：更新使能/IRQ 使能位，处理清空与错误应答；返回是否发生了状态变化（供中断边沿检测） */
void fifo_cnt_write(ipc_fifo_t *f, int is_arm7, uint16_t val);

/* 发送一个字（写 SEND）。未使能则忽略。 */
void fifo_send(ipc_fifo_t *f, int is_arm7, uint32_t val);
/* 接收一个字（读 RECV）。空队列返回 0 并置错误位。 */
uint32_t fifo_recv(ipc_fifo_t *f, int is_arm7);

/* 检查 FIFO 中断触发条件的边沿，置对应 CPU 的 IF17/18（由 io 层调用） */
void fifo_update_irq(ipc_fifo_t *f, int is_arm7, struct irq *irq);

#endif /* NDS_EMU_IO_FIFO_H */
