#ifndef NDS_EMU_BIOS7_LOW_H
#define NDS_EMU_BIOS7_LOW_H

#include <stdint.h>

typedef struct arm_cpu arm_cpu_t; /* 前向声明：只操作 CPU 寄存器/总线 */

/* 21-B9wt：ARM7 FreeBIOS 低地址（0x0000-0x3FFF）路径的参考级 HLE。

   真机 ARM7 的 SWI / IRQ 不是“模拟器直接调 C 函数”，而是走 BIOS 低地址
   里的真实机器码：

     0x00000008  SWI 向量  → b 0x1080
     0x00001080  SWI 分发器：压 SVC 栈三字 + SPSR、切 System 模式、
                 取 SWI 编号、按 0x10B0 的函数表跳转
     0x0000112C  swi_complete：恢复 System lr、切回 SVC、msr SPSR、
                 弹回调用方（movs pc,lr 用 SPSR 恢复 CPSR）
     0x0000114C  Halt（写 HALTCNT=0x80 后暂停，等 IF&IE 唤醒）
     0x0000115C  WaitByLoop（subs r0,#1; bgt 循环）
     0x00001168  interrupt_check（软件中断标志 0x03FFFFF8 + IME 开关）
     0x00001188  VBlankIntrWait / 0x1190 IntrWait（HALTCNT + 标志轮询）
     0x00000018  IRQ 向量  → b 0x1FB0
     0x00001FB0  IRQ 入口：压六字帧、lr=0x1FC0、跳到 [0x03FFFFFC] 的用户 handler
     0x00001FC0  IRQ 返回：弹六字帧 + subs pc,lr,#4（用 SPSR_irq 恢复）

   本模块不执行 BIOS 字节码，而是按地址逐条等价实现上述指令的效果：
   同样的栈帧、同样的模式切换、同样的 PC/lr 值。这样游戏侧（FFXII ARM7 的
   任务调度器会把 BIOS 出入口地址当任务现场保存/恢复）看到的现场与参考核心
   （melonDS + FreeBIOS）一致；分步实现也让 IRQ 能在与真机相同的边界插入
   （尤其是 Halt 暂停点：IRQ 打断的是 BIOS 内部 PC，而不是调用方 PC）。 */

/* ARM7 取指前调用：PC 命中低地址路径返回 1（已等价执行一格），否则返回 0。 */
int bios7_low_step(arm_cpu_t *cpu);

/* 21-B9yd：低地址路径统计（单测断言与长跑诊断共用）。 */
typedef struct {
    uint32_t swi_count[32]; /* SWI n 的调用次数 */
    uint32_t real_body;     /* 交给真地址真实执行的函数体次数 */
    uint32_t hle_body;      /* 镜像缺失时兜底走 C 版 HLE 的次数（应为 0） */
    uint32_t unmodeled;     /* 低地址取指但未逐条建模、交给真实执行的次数 */
} bios7_low_stats_t;

void bios7_low_reset_stats(void);
void bios7_low_get_stats(bios7_low_stats_t *out);

/* 21-B9ye：低地址取指直方图（`NDS_BIOS7_HIST=1` 开启；与参考核 g_hist7 对照）。 */
void bios7_low_dump_hist(const char *tag);

/* 低地址路径里用到的地址（也供单测/日志引用）。 */
#define BIOS7_ADDR_SWI_VEC       0x00000008u
#define BIOS7_ADDR_IRQ_VEC       0x00000018u
#define BIOS7_ADDR_BOOT_LOOP     0x00001078u  /* boot_handler：b 自己 */
#define BIOS7_ADDR_UNHANDLED     0x0000107Cu  /* unhandled_exception：b 自己 */
#define BIOS7_ADDR_SWI_HANDLER   0x00001080u
#define BIOS7_ADDR_SWI_TABLE     0x000010B0u
#define BIOS7_ADDR_SWI_COMPLETE  0x0000112Cu
#define BIOS7_ADDR_SWI_HALT      0x0000114Cu
#define BIOS7_ADDR_SWI_WAITLOOP  0x0000115Cu
#define BIOS7_ADDR_INTR_CHECK    0x00001168u
#define BIOS7_ADDR_SWI_VBLANK    0x00001188u
#define BIOS7_ADDR_SWI_INTRWAIT  0x00001190u
#define BIOS7_ADDR_SWI_STOP      0x000011B8u
#define BIOS7_ADDR_SWI_CUSTOMHALT 0x00001FA4u
#define BIOS7_ADDR_IRQ_HANDLER   0x00001FB0u
#define BIOS7_ADDR_IRQ_RETURN    0x00001FC0u

/* 用户 IRQ handler 指针槽（ARM7 WRAM 顶）与软件中断标志字：
   真机 BIOS 的 IRQ 入口用 [0x04000000-4] 取 handler；IntrWait 轮询的是
   同一块 WRAM 顶上 -8 的字（0x03FFFFF8），由游戏自己的 handler 置位。 */
#define BIOS7_IRQ_HANDLER_SLOT   0x03FFFFFCu
#define BIOS7_SOFT_IRQ_FLAG      0x03FFFFF8u

#endif /* NDS_EMU_BIOS7_LOW_H */
