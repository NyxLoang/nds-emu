#include <stdio.h>
#include <stdlib.h>
#include "cpu.h"
#include "exec.h"
#include "thumb.h"
#include "bus/bus.h"
#include "io/io.h"
#include "io/power.h"
#include "bios/bios7_low.h"

/* 21-B9xs：PC 命中计数（bring-up 诊断，最多 4 个地址；由 --pchit 配置）。
   用来对照两边"某段代码每帧执行多少次"，例如 ARM7 的消息循环与槽处理。 */
uint32_t g_pchit_addr[16];
unsigned long long g_pchit_cnt[16];
int g_pchit_n = 0;
/* 21-B9xs：命中时 r0 低 4 位的直方图（用于看“投递索引 0..3 各多少次”） */
unsigned long long g_pchit_hist[16];

arm_cpu_t *cpu_create(nds_t *nds, uint32_t reset_pc, int is_arm7)
{
    arm_cpu_t *cpu = calloc(1, sizeof(arm_cpu_t));
    if (cpu == NULL)
        return NULL;
    cpu->nds = nds;
    cpu->is_arm7 = is_arm7;
    /* r15 = PC，初始指向镜像入口；cpsr 清零（真机复位后是 SVC 模式，此处简化） */
    cpu->r[15] = reset_pc;
    /* 异常向量基址：ARM9 用高向量 0xFFFF0000，ARM7 用低向量 0x00000000（阶段 12） */
    cpu->vector_base = is_arm7 ? 0x00000000u : 0xFFFF0000u;
    return cpu;
}

void cpu_destroy(arm_cpu_t *cpu)
{
    free(cpu);
}

/* 复位：装载镜像完成后把 PC 指到 ARM9 入口，重新开始计数。 */
void cpu_reset(arm_cpu_t *cpu, uint32_t reset_pc)
{
    cpu->r[15] = reset_pc;
    cpu->next_fetch_pc = reset_pc; /* 21-B9yh：复位后第一条按「顺序取指」计费 */
    cpu->cycles = 0;
    cpu->deadloop_reported = 0;
    cpu->irq_count = 0;
}

/* 21-B9ws：直接启动寄存器初值（melonDS NDS::SetupDirectBoot 口径）。
   ARM9：r12=入口、r13=0x03002F7C、r14=入口、sp_irq=0x03003F80、sp_svc=0x03003FC0
   ARM7：r12=入口、r13=0x0380FD80、r14=入口、sp_irq=0x0380FF80、sp_svc=0x0380FFC0
   参考核在跳到卡带入口前就是这么设的；本地旧实现让 sp=0，一旦 BIOS 真的
   往 SVC/IRQ 栈压帧就会写到栈外。 */
void cpu_direct_boot(arm_cpu_t *cpu, uint32_t entry)
{
    cpu->r[12] = entry;
    cpu->r[14] = entry;
    if (cpu->is_arm7) {
        cpu->r13_sys = 0x0380FD80u;
        cpu->r13_bank[1] = 0x0380FF80u; /* [1] = IRQ */
        cpu->r13_bank[2] = 0x0380FFC0u; /* [2] = SVC */
    } else {
        cpu->r13_sys = 0x03002F7Cu;
        cpu->r13_bank[1] = 0x03003F80u;
        cpu->r13_bank[2] = 0x03003FC0u;
    }
    cpu->r[13] = cpu->r13_sys;          /* 当前模式（SVC/System）可见的是主 sp */
}

/* 3a.3 取指：按 PC 从总线读 32 位指令字（小端拼拆已在 bus_read32 内完成）。 */
uint32_t cpu_fetch(const arm_cpu_t *cpu)
{
    return bus_read32(cpu->nds->bus, cpu->r[15]);
}

/* 13.2 Thumb 取指：按 PC 从总线读 16 位半字指令。 */
uint16_t cpu_fetch16(const arm_cpu_t *cpu)
{
    return bus_read16(cpu->nds->bus, cpu->r[15]);
}

/* 21-B9wa（实验）：ARM9 FreeBIOS IRQ 尾部。
   参考核心在 ITCM 0x01FF81A0 弹 PC 后不是直接回旧任务，而是先到
   0xFFFF06F0（FreeBIOS IRQ 入口设置的“返回桩”），由
     ldmia sp!, {r0-r3,r12,r14}
     subs pc, r14, #4
   从当前 IRQ 栈帧恢复寄存器并用 SPSR_irq 恢复 CPSR。
   FFXII 的 ITCM 上下文切换把“新任务”的六字帧压回栈后弹到该桩，
   本地此前直接把桩地址错设成被打断 PC，导致只恢复旧 sleep 上下文。 */
static int bios_irq_tail9(arm_cpu_t *cpu)
{
    if (cpu->is_arm7)
        return 0;
    if ((cpu->cpsr & CPSR_MODE_MASK) != ARM_MODE_IRQ)
        return 0;
    if (cpu->r[15] != 0xFFFF06F0u)
        return 0;

    uint32_t sp = cpu->r[13];
    cpu->r[0]  = bus_read32(cpu->nds->bus, sp + 0x00u);
    cpu->r[1]  = bus_read32(cpu->nds->bus, sp + 0x04u);
    cpu->r[2]  = bus_read32(cpu->nds->bus, sp + 0x08u);
    cpu->r[3]  = bus_read32(cpu->nds->bus, sp + 0x0Cu);
    cpu->r[12] = bus_read32(cpu->nds->bus, sp + 0x10u);
    cpu->r[14] = bus_read32(cpu->nds->bus, sp + 0x14u);
    cpu->r[13] = sp + 0x18u;

    uint32_t ret = cpu->r[14] - 4u;
    uint32_t saved = cpu->spsr[1];
    cpu->r[15] = ret;
    exec_apply_cpsr(cpu, saved);
    cpu->step_cycles = 2;
    if (cpu->nds->bus->diag && cpu->irq_count < 16) {
        printf("irq: arm9 bios tail ret=%08X cpsr=%08X\n", ret, cpu->cpsr);
    }
    return 1;
}

/* 单步执行一条指令：
   框架只负责「取指 + 指令计数」，指令语义全部委托给 exec_step（见 exec.c）。
   返回 0 表示停机（本阶段总是返回 1，停机由死循环达成）。 */
/* 21-B9yh：按取指区域计费（只对「非顺序取指」加价）。

   实测背景（第 26 帧 / 第 1900 帧，本地 vs 参考核 melonDS+FreeBIOS）：

   | 口径 | 26 帧 ARM9 指令 | 1900 帧 ARM9 指令 |
   |---|---|---|
   | 参考核 | 8,328,123（14.56M 单位 ⇒ 1.75 单位/条） | 720,588,787 |
   | 本地：全部 1 单位/条 | 12,065,443（+45%） | 784,957,288（+8.9%） |
   | 本地：主存一律 2 单位/条 | 7,112,902（−15%） | 618,299,382（−14.2%） |

   ⇒ 差额集中在**启动期（代码在主存）**，而长程只差 8.9%；「主存一律翻倍」
   会两头都过度惩罚。melonDS 的真实口径是「顺序取指便宜、非顺序取指贵」
   （ARM9 主存：S16=1 / N16=8，ARM 态顺序 S32=2、非顺序 N32=9）。本地按
   比例缩小成 **顺序 1 / 非顺序 3**（ITCM 与其它区域都 1；ARM7 不额外计费，
   它的代码绝大多数跑在 WRAM，且实测本地 ARM7 指令数本就低于参考核）。 */
static int s_nonseq_cost = -1; /* -1=未初始化；NDS_ARM9_NOSEQ 可调（默认 1） */

void cpu_set_nonseq_cost(int v)
{
    if (v >= 1 && v <= 8)
        s_nonseq_cost = v;
}

static uint32_t cpu_fetch_cost(const arm_cpu_t *cpu, uint32_t pc, int nonseq)
{
    if (s_nonseq_cost < 0) {
        const char *e = getenv("NDS_ARM9_NOSEQ");
        s_nonseq_cost = 1;   /* 实测默认值：见 docs/21-rom-bringup.md 的 21-B9yi */
        if (e != NULL) {
            int v = atoi(e);
            if (v >= 1 && v <= 8)
                s_nonseq_cost = v;
        }
    }
    if (cpu->is_arm7)
        return 1u;
    if (pc - BUS_ARM9_ITCM_BASE < BUS_ARM9_ITCM_SIZE)
        return 1u;                           /* ITCM：melonDS 恒 1 */
    if ((pc >> 24) != 0x02u)
        return 1u;                           /* WRAM/IO/VRAM/BIOS 等 */
    return nonseq ? (uint32_t)s_nonseq_cost : 1u; /* 主存：非顺序取指更贵 */
}

int cpu_step(arm_cpu_t *cpu)
{
    /* 21-B9yi：上一条指令消耗的周期数（本步用于推进卡带时钟，使其与
       参考核一样按系统时钟节奏取数；见 io_advance_cart 注释） */
    uint32_t prev_cost = cpu->step_cycles;
    cpu->step_cycles = 1;
    /* 8.x：设置当前访问者身份，供 bus 对中断/FIFO 等按 CPU 分流 */
    cpu->nds->bus->active_is_arm7 = cpu->is_arm7;
    /* 21-B9xj：写监视用 PC。取值口径与 melonDS 解释器一致（ARM=当前指令+8、
       Thumb=+4），这样本地与参考 harness 打出来的 pc 可直接对照。 */
    if (cpu->nds->bus->diag)
        cpu->nds->bus->dbg_pc = cpu->r[15] + ((cpu->cpsr & CPSR_T) ? 4u : 8u);
    if (cpu->nds->bus->diag)
        cpu->nds->bus->dbg_lr = cpu->r[14];
    if (cpu->nds->bus->diag)
        cpu->nds->bus->dbg_sp = cpu->r[13];
    if (cpu->nds->bus->diag)
        cpu->nds->bus->dbg_cpsr = cpu->cpsr;
    /* 21-B9xs：PC 命中计数（只在开启诊断且有配置时执行） */
    if (cpu->nds->bus->diag && g_pchit_n > 0) {
        uint32_t hit_pc = cpu->r[15];
        static unsigned g_pchit_log[16];
        for (int i = 0; i < g_pchit_n; i++) {
            if (g_pchit_addr[i] == hit_pc) {
                g_pchit_cnt[i]++;
                g_pchit_hist[cpu->r[0] & 0xFu]++;
                if (g_pchit_log[i] < 3) {
                    g_pchit_log[i]++;
                    const bus_t *b = cpu->nds->bus;
                    uint32_t sp = cpu->r[13];
                    printf("pchit-hit: %08X arm%d r0=%08X r7=%08X lr=%08X sp=%08X"
                           " st=%08X/%08X/%08X/%08X\n",
                           hit_pc, cpu->is_arm7 ? 7 : 9, cpu->r[0], cpu->r[7],
                           cpu->r[14], sp,
                           bus_read32(b, sp), bus_read32(b, sp + 4u),
                           bus_read32(b, sp + 8u), bus_read32(b, sp + 12u));
                }
            }
        }
    }
    irq_t *irq = &cpu->nds->io->irq[cpu->is_arm7 ? 1 : 0];
    /* 21-B9wt：ARM7 的 HALTCNT 暂停（BIOS SWI 6 Halt / SWI 7 Stop / 游戏
       直接写 0x04000301 都走这里）。唤醒口径与 melonDS HaltInterrupted(1)
       一致：(IF & IE) != 0 即唤醒，不看 IME；唤醒后清请求，让下面的 IRQ
       检查先决定“进 handler”还是“继续执行 BIOS 下一条”。 */
    if (cpu->is_arm7 && power_halt_pending(&cpu->nds->io->power)) {
        if (!(irq->ie & irq->ifl)) {
            cpu->step_cycles = 0;
            return 1;
        }
        power_halt_wake(&cpu->nds->io->power);
    }
    if (bios_irq_tail9(cpu))
        return 1;
    /* 6.5：按本步消耗的周期推进当前核定时器（分频在 timer.c 内处理）。
       21-B9yi：传上一条指令的**实际周期数**（此前固定 1/指令，ARM9 平均 ~1.2，
       定时器会系统性偏慢；melonDS 定时器是挂在系统时钟上的）。 */
    io_advance_timers(cpu->nds->io, cpu->is_arm7, prev_cost ? prev_cost : 1u);
    io_advance_cart(cpu->nds->io, cpu->is_arm7, prev_cost);
    /* 21-B9yi(续12) 诊断：NDS_PCSAMPLE=LO-HI@N → 帧区间内每 N 条 ARM9 指令打印
       一次 PC/lr/cpsr（粗粒度执行轨迹，用来判断「某段等待循环是不是被跳过了」）。
       例：NDS_PCSAMPLE=1903-1905@200 */
    {
        extern unsigned long long g_dbg_frame;
        static int ps_state;
        static long ps_lo = -2, ps_hi = -2;
        static unsigned long ps_period, ps_seen;
        if (ps_state == 0) {
            const char *e = getenv("NDS_PCSAMPLE");
            ps_state = (e != NULL && e[0] != '0' && e[0] != '\0') ? 1 : -1;
            ps_lo = 0; ps_hi = -1; ps_period = 200;
            if (ps_state == 1 && e != NULL) {
                long lo = 0, hi = 0; unsigned per = 0;
                if (sscanf(e, "%ld-%ld@%u", &lo, &hi, &per) == 3) {
                    ps_lo = lo; ps_hi = hi; if (per > 0) ps_period = per;
                }
            }
        }
        if (ps_state == 1 && !cpu->is_arm7 &&
            (long)g_dbg_frame >= ps_lo && (long)g_dbg_frame <= ps_hi) {
            if ((ps_seen++ % ps_period) == 0)
                printf("pcsample: f=%llu pc=%08X lr=%08X cpsr=%08X sp=%08X\n",
                       g_dbg_frame, cpu->r[15], cpu->r[14], cpu->cpsr, cpu->r[13]);
        }
    }
    /* 12.5：取指前检查 IRQ。条件 = 该核 IF&IE&IME 挂起，且 CPSR 的 I 位未禁止。
       满足则进 IRQ 异常向量（0x18），PC 跳到 handler；被打断指令地址留作返回点。 */
    /* 21-B9m：ARM9 的 CP15 WFI（MCR p15,0,r0,c7,c0,4）等价于 NDS7 的 HALTCNT
       ——NDS9 没有 HALTCNT 寄存器，游戏/OS 空闲任务直接执行 WFI 指令。
       NDS9 的 CP15 Halt 只受 IME 门控（IME=0 会永久锁死），不会因 CPSR.I 屏蔽
       而卡住；挂起未到 → PC 不动等待，挂起到 → 清 I 后走正常 IRQ 入口。 */
    if (!cpu->is_arm7 && cpu_fetch(cpu) == 0xEE070F90u) {
        if (!irq_pending(irq)) {
            cpu->step_cycles = 0;
            return 1;
        }
        cpu->cpsr &= ~CPSR_I;
        /* 21-B9s：WFI 被中断唤醒时指令先“完成”再进 IRQ——PC 前进到下一条，
           否则 IRQ 返回后又停在 WFI 上重执行，空闲任务永远走不到后续代码。 */
        cpu->r[15] += 4;
    }
    /* 21-B9h：IF&IE 已挂起却被 CPSR.I 屏蔽时只提示一次 */
    if (irq_pending(irq) && (cpu->cpsr & CPSR_I) && !cpu->irq_mask_logged) {
        cpu->irq_mask_logged = 1;
        if (cpu->nds->bus->diag)
            printf("irq: %s IF&IE pending but masked-by-cpsr-I"
                   " cpsr=%08X pc=%08X\n",
                   cpu->is_arm7 ? "arm7" : "arm9", cpu->cpsr, cpu->r[15]);
    }
    if (irq_pending(irq) && !(cpu->cpsr & CPSR_I)) {
        cpu->cycles++;
        /* 21-B9yi 诊断：NDS_IRQLOG=1 → 打印每次 IRQ 受理时的
           （帧, 被打断 PC, CPSR, lr），与参考核 `refirq:` 行逐条对照。 */
        {
            extern unsigned long long g_dbg_frame;
            static int irqlog_state, irqlog_n;
            static long irqlog_lo = -2, irqlog_hi = -2;
            if (irqlog_state == 0) {
                const char *e = getenv("NDS_IRQLOG");
                irqlog_state = (e != NULL && e[0] != '0' && e[0] != '\0') ? 1 : -1;
                irqlog_lo = 0; irqlog_hi = -1;   /* 默认全覆盖 */
                if (irqlog_state == 1 && e != NULL) {
                    long lo = 0, hi = 0;
                    if (sscanf(e, "%ld-%ld", &lo, &hi) == 2) {
                        irqlog_lo = lo; irqlog_hi = hi;  /* 21-B9yi(续12)：支持帧区间 */
                    }
                }
            }
            if (irqlog_state == 1 && !cpu->is_arm7 && irqlog_n < 400000 &&
                (long)g_dbg_frame >= irqlog_lo && (long)g_dbg_frame <= irqlog_hi) {
                irqlog_n++;
                printf("locirq: f=%llu pc=%08X cpsr=%08X lr=%08X if=%08X ie=%08X t=%llu\n",
                       g_dbg_frame, cpu->r[15], cpu->cpsr, cpu->r[14],
                       irq->ifl, irq->ie, (unsigned long long)cpu->cycles);
            }
        }
        /* 21-B9b：诊断首中断现场——FFXII 的 ARM9 第一次 IRQ 会跳到高向量 0xFFFF0018，
           真 BIOS 在那里从“可读写 RAM 的约定槽”取用户 handler。先看游戏把 handler
           装到哪：ARM9 槽在 DTCM 末 8 字节（0x3FF8=等待标志/0x3FFC=handler 指针），
           ARM7 槽在 WRAM 高地址 0x03FFFFF8/FC。只打一次，避免轮询刷屏。 */
        if (cpu->nds->bus->diag && !cpu->irq_dump_done) {
            cpu->irq_dump_done = 1;
            uint32_t slot_f8 = 0, slot_fc = 0;
            if (cpu->is_arm7) {
                slot_f8 = bus_read32(cpu->nds->bus, 0x0380FFF8u);
                slot_fc = bus_read32(cpu->nds->bus, 0x0380FFFCu);
            } else {
                /* 用 bus 保存的 DTCM 基址计算槽位，游戏改配 DTCM 时也跟得上 */
                uint32_t base = cpu->nds->bus->arm9_dtcm_on
                                    ? cpu->nds->bus->arm9_dtcm_base
                                    : 0x027E0000u; /* 未使能时回退 FFXII 的已知配置 */
                slot_f8 = bus_read32(cpu->nds->bus, base + 0x3FF8u);
                slot_fc = bus_read32(cpu->nds->bus, base + 0x3FFCu);
            }
            printf("irq: first %s IRQ pc=%08X cpsr=%08X ime=%08X ie=%08X ifl=%08X"
                   " slot+3FF8=%08X slot+3FFC=%08X\n",
                   cpu->is_arm7 ? "arm7" : "arm9", cpu->r[15], cpu->cpsr,
                   irq->ime, irq->ie, irq->ifl, slot_f8, slot_fc);
        }
        if (cpu->nds->bus->diag && cpu->irq_dump_done &&
            cpu->irq_count < 16) {
            printf("irq: #%d %s trigger pc=%08X cpsr=%08X\n",
                   cpu->irq_count + 1, cpu->is_arm7 ? "arm7" : "arm9",
                   cpu->r[15], cpu->cpsr);
        }
        /* ARM9 高向量跳板要在切模式前取被打断 PC（r0-r3/r12 仍是被中断值）。 */
        uint32_t ret_pc = cpu->r[15];
        arm_exception(cpu, EXC_IRQ_OFF, ARM_MODE_IRQ, 4);
        cpu->irq_count++;
        /* 21-B9c：模拟 ARM9 BIOS 高向量跳板——真机 0xFFFF0018 处的 BIOS 代码会从
           DTCM 末 4 字节（0x3FFC，用户 IRQ handler 指针槽）取地址再跳转。本模拟器
          没有 BIOS ROM，异常现场建好后直接读槽并改写 PC；FFXII 在复位时把
           handler 装到 ITCM 0x01FF8000（见 21-B9b 证据）。槽为 0 或 DTCM 未配置
          时保持旧行为（停在向量区便于诊断）。
           ARM7 的对应路径已在 21-B9wt 移进 bios7_low.c：异常先落到低向量
           0x00000018 → 0x1FB0（六字帧 + lr=0x1FC0）→ [0x03FFFFFC] 用户 handler。 */
        if (!cpu->is_arm7 && cpu->nds->bus->arm9_dtcm_on) {
            uint32_t slot_fc = bus_read32(cpu->nds->bus,
                                          cpu->nds->bus->arm9_dtcm_base + 0x3FFCu);
            if (slot_fc != 0) {
                /* 21-B9x + 21-B9wa：等价执行 FreeBIOS 0xFFFF06D8 入口——
                   先 stmdb sp!,{r0-r3,r12,lr} 压六字帧，再设 lr=0xFFFF06F0
                   （BIOS 返回桩）后跳用户 handler。ITCM 上下文切换靠这个
                   桩地址把新任务接回调度器；旧实现把 lr 设成被打断 PC，
                   使弹栈直接落回旧任务而非 BIOS 桩。 */
                uint32_t isp = cpu->r[13];
                bus_write32(cpu->nds->bus, isp - 0x18u + 0x00u, cpu->r[0]);
                bus_write32(cpu->nds->bus, isp - 0x18u + 0x04u, cpu->r[1]);
                bus_write32(cpu->nds->bus, isp - 0x18u + 0x08u, cpu->r[2]);
                bus_write32(cpu->nds->bus, isp - 0x18u + 0x0Cu, cpu->r[3]);
                bus_write32(cpu->nds->bus, isp - 0x18u + 0x10u, cpu->r[12]);
                bus_write32(cpu->nds->bus, isp - 0x18u + 0x14u, ret_pc + 4u);
                cpu->r[13] = isp - 0x18u;
                /* 目标地址 LSB=1 表示 Thumb 入口：按 BX 规则清 PC 最低位并置 T */
                if (slot_fc & 1u)
                    cpu->cpsr |= CPSR_T;
                else
                    cpu->cpsr &= ~CPSR_T;
                cpu->r[15] = slot_fc & ~1u;
                cpu->r[14] = 0xFFFF06F0u;
            }
        }
        return 1;
    }
    /* 21-B9wt：ARM7 低地址 BIOS 路径（SWI 分发、等待、IRQ 出入口）按地址等价执行。
       放在 IRQ 检查之后：真机每步先看中断，再执行当前指令；Halt 暂停点因此在
       “写 HALTCNT 的下一条”处被 IRQ 打断，与参考核一致。 */
    if (cpu->is_arm7 && cpu->r[15] < 0x4000u && bios7_low_step(cpu))
        return 1;
    /* 13.2：按 CPSR.T 位分发——Thumb 取 16 位半字，ARM 取 32 位字。 */
    uint32_t ipc = cpu->r[15];   /* 本步指令地址（取指区域计费用） */
    int is_thumb = (cpu->cpsr & CPSR_T) != 0;
    int fetch_nonseq = (cpu->next_fetch_pc != ipc); /* 是否非顺序取指（分支/跳转后） */
    /* 下一条「顺序」指令地址（本条指令长度由执行前的 T 位决定） */
    cpu->next_fetch_pc = ipc + (is_thumb ? 2u : 4u);
    if (is_thumb) {
        uint16_t insn16 = cpu_fetch16(cpu);
        cpu->cycles++;
        int r = thumb_step(cpu, insn16);
        /* 21-B9yh：本步取指代价（见 cpu_fetch_cost；step_cycles=0 表示在等待） */
        uint32_t fc = cpu_fetch_cost(cpu, ipc, fetch_nonseq);
        if (cpu->step_cycles != 0 && cpu->step_cycles < fc)
            cpu->step_cycles = fc;
        return r;
    }
    uint32_t insn = cpu_fetch(cpu);
    cpu->cycles++;
    int r = exec_step(cpu, insn);
    {
        uint32_t fc = cpu_fetch_cost(cpu, ipc, fetch_nonseq);
        if (cpu->step_cycles != 0 && cpu->step_cycles < fc)
            cpu->step_cycles = fc;
    }
    return r;
}
