#include <stdio.h>
#include <stdlib.h>
#include "cpu.h"
#include "exec.h"
#include "thumb.h"
#include "bus/bus.h"
#include "io/io.h"

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
    cpu->cycles = 0;
    cpu->deadloop_reported = 0;
    cpu->irq_hle.active = 0;
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

/* 21-B9f：IRQ HLE 桩的“返回恢复”。FFXII 的中断分发器用
   `stmdb sp!,{lr}; ...; ldmfd sp!,{pc}` 返回——它压入/弹出的返回地址是模拟桩
   设置的“被打断指令 PC”（等价于真 BIOS 把 stub 地址放进 LR，stub 再 SUBS 返回）。
   由于没有可执行 BIOS 桩，返回后由本函数在取指前恢复现场并重执行被打断指令。 */
static void irq_hle_restore(arm_cpu_t *cpu)
{
    if (!cpu->irq_hle.active)
        return;
    if ((cpu->cpsr & CPSR_MODE_MASK) != ARM_MODE_IRQ)
        return;
    if (cpu->r[15] != cpu->irq_hle.ret_pc)
        return;
    for (int i = 0; i < 4; i++)
        cpu->r[i] = cpu->irq_hle.r[i];
    cpu->r[12] = cpu->irq_hle.ip;
    if (!cpu->is_arm7)
        cpu->r[13] = cpu->irq_hle.saved_irq_sp;
    /* 恢复 CPSR 也要切回 User/System：经模式同步把 IRQ 私有 r13/r14 存回槽 */
    exec_apply_cpsr(cpu, cpu->irq_hle.saved_cpsr);
    /* 21-B9s：ARM7 的 Halt（SWI 0x06，Thumb 编码 DF06）在任意中断到来后应
       “被唤醒并继续 SWI 之后”，而不是重新执行 Halt——否则中断里做完的事
       （如 FIFO 请求处理）永远不会落到后续代码，CPU 会再次睡死。
       这里只在被打断指令确实是 Thumb SWI 6 时跳过该指令；IntrWait 等带条件
       重试的等待仍走“重执行被打断指令”的旧语义。 */
    if (cpu->is_arm7 && (cpu->cpsr & CPSR_T) &&
        cpu_fetch16(cpu) == 0xDF06u) {
        cpu->r[15] = cpu->irq_hle.ret_pc + 2u;
    }
    cpu->irq_hle.active = 0;
    if (cpu->nds->bus->diag && cpu->irq_hle.log_count < 16) {
        printf("irq: #%d %s restore ret_pc=%08X cpsr=%08X\n",
               cpu->irq_hle.log_count, cpu->is_arm7 ? "arm7" : "arm9",
               cpu->r[15], cpu->cpsr);
    }
}

/* 单步执行一条指令：
   框架只负责「取指 + 指令计数」，指令语义全部委托给 exec_step（见 exec.c）。
   返回 0 表示停机（本阶段总是返回 1，停机由死循环达成）。 */
int cpu_step(arm_cpu_t *cpu)
{
    /* 8.x：设置当前访问者身份，供 bus 对中断/FIFO 等按 CPU 分流 */
    cpu->nds->bus->active_is_arm7 = cpu->is_arm7;
    /* 21-B9f：先检查 IRQ handler 是否刚弹出返回地址（见函数注释） */
    irq_hle_restore(cpu);
    /* 6.5：一条指令 ≈ 一个周期，推进所有使能定时器（分频在 timer.c 内处理） */
    io_advance_timers(cpu->nds->io, cpu->is_arm7);
    /* 12.5：取指前检查 IRQ。条件 = 该核 IF&IE&IME 挂起，且 CPSR 的 I 位未禁止。
       满足则进 IRQ 异常向量（0x18），PC 跳到 handler；被打断指令地址留作返回点。 */
    irq_t *irq = &cpu->nds->io->irq[cpu->is_arm7 ? 1 : 0];
    /* 21-B9m：ARM9 的 CP15 WFI（MCR p15,0,r0,c7,c0,4）等价于 NDS7 的 HALTCNT
       ——NDS9 没有 HALTCNT 寄存器，游戏/OS 空闲任务直接执行 WFI 指令。
       NDS9 的 CP15 Halt 只受 IME 门控（IME=0 会永久锁死），不会因 CPSR.I 屏蔽
       而卡住；挂起未到 → PC 不动等待，挂起到 → 清 I 后走正常 IRQ 入口。 */
    if (!cpu->is_arm7 && cpu_fetch(cpu) == 0xEE070F90u) {
        if (!irq_pending(irq))
            return 1;
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
            cpu->irq_hle.log_count < 16) {
            printf("irq: #%d %s trigger pc=%08X cpsr=%08X\n",
                   cpu->irq_hle.log_count + 1, cpu->is_arm7 ? "arm7" : "arm9",
                   cpu->r[15], cpu->cpsr);
        }
        /* HLE 桩现场：arm_exception 只改 CPSR/r14/r15，r0-r3/r12 仍是被中断值，
           saved_cpsr/ret_pc 必须在切模式前取。 */
        uint32_t saved_cpsr = cpu->cpsr;
        uint32_t ret_pc = cpu->r[15];
        arm_exception(cpu, EXC_IRQ_OFF, ARM_MODE_IRQ, 4);
        /* 21-B9c：模拟 ARM9 BIOS 高向量跳板——真机 0xFFFF0018 处的 BIOS 代码会从
           DTCM 末 4 字节（0x3FFC，用户 IRQ handler 指针槽）取地址再跳转。本模拟器
           没有 BIOS ROM，异常现场建好后直接读槽并改写 PC；FFXII 在复位时把
           handler 装到 ITCM 0x01FF8000（见 21-B9b 证据）。槽为 0 或 DTCM 未配置
           时保持旧行为（停在向量区便于诊断）。ARM7 尚未观察到 IRQ 触发，暂不
           按 0x03FFFFFC 槽跳转，待有真机证据再对称实现。 */
        if (!cpu->is_arm7 && cpu->nds->bus->arm9_dtcm_on) {
            uint32_t slot_fc = bus_read32(cpu->nds->bus,
                                          cpu->nds->bus->arm9_dtcm_base + 0x3FFCu);
            if (slot_fc != 0) {
                /* 21-B9x：真机 ARM9 BIOS 的 IRQ 入口会先压 r0-r3/r12/lr 六字帧，
                   再跳用户 handler（FreeBIOS interrupt_handler 同款）。FFXII 的
                   ITCM dispatcher 在 0x01FF8164 附近从 IRQ 栈弹这 6 个字保存现场；
                   此前没有压帧会弹到栈底 0，把空闲任务上下文写坏。 */
                uint32_t isp = cpu->r[13];
                cpu->irq_hle.saved_irq_sp = isp;
                bus_write32(cpu->nds->bus, isp - 0x18u + 0x00u, cpu->r[0]);
                bus_write32(cpu->nds->bus, isp - 0x18u + 0x04u, cpu->r[1]);
                bus_write32(cpu->nds->bus, isp - 0x18u + 0x08u, cpu->r[2]);
                bus_write32(cpu->nds->bus, isp - 0x18u + 0x0Cu, cpu->r[3]);
                bus_write32(cpu->nds->bus, isp - 0x18u + 0x10u, cpu->r[12]);
                bus_write32(cpu->nds->bus, isp - 0x18u + 0x14u, ret_pc + 4u);
                cpu->r[13] = isp - 0x18u;
                /* 激活 HLE 桩：把返回点设成被打断指令 PC，dispatcher 的
                   pop {pc} 会回到这里，随后 irq_hle_restore 恢复现场 */
                cpu->irq_hle.active = 1;
                cpu->irq_hle.saved_cpsr = saved_cpsr;
                cpu->irq_hle.ret_pc = ret_pc;
                cpu->irq_hle.r[0] = cpu->r[0];
                cpu->irq_hle.r[1] = cpu->r[1];
                cpu->irq_hle.r[2] = cpu->r[2];
                cpu->irq_hle.r[3] = cpu->r[3];
                cpu->irq_hle.ip = cpu->r[12];
                cpu->irq_hle.log_count++;
                /* 目标地址 LSB=1 表示 Thumb 入口：按 BX 规则清 PC 最低位并置 T */
                if (slot_fc & 1u)
                    cpu->cpsr |= CPSR_T;
                else
                    cpu->cpsr &= ~CPSR_T;
                cpu->r[15] = slot_fc & ~1u;
                /* dispatcher 以 stmdb/pop 成对使用 lr：给它“返回被中断 PC”的
                   地址（真 BIOS 桩会给 stub 地址再 SUBS 回这里，效果等价） */
                cpu->r[14] = ret_pc;
            }
        }
        /* 21-B9i：ARM7 IRQ 槽在 ARM7 WRAM 顶 0x0380FFFC（FFXII 实测指向
           0x037FB8F4 的中断分发代码），与 ARM9 同样做 HLE 现场保存/恢复。 */
        if (cpu->is_arm7) {
            uint32_t slot_fc = bus_read32(cpu->nds->bus, 0x0380FFFCu);
            if (slot_fc != 0) {
                /* 21-B9j：ARM7 调度器在 0x37FBA10 用 ldmib sp!,{...} 从 IRQ 栈
                   上方 0x380FF80..0x94 取旧任务 r0-r3/r12/lr。真机该区由 BIOS
                   入口帧预填；这里在跳用户 handler 前等价预填（lr=被打断PC+4）。 */
                uint32_t isp = cpu->r[13];
                bus_write32(cpu->nds->bus, isp + 0x00u, cpu->r[0]);
                bus_write32(cpu->nds->bus, isp + 0x04u, cpu->r[1]);
                bus_write32(cpu->nds->bus, isp + 0x08u, cpu->r[2]);
                bus_write32(cpu->nds->bus, isp + 0x0Cu, cpu->r[3]);
                bus_write32(cpu->nds->bus, isp + 0x10u, cpu->r[12]);
                bus_write32(cpu->nds->bus, isp + 0x14u, ret_pc + 4u);
                cpu->irq_hle.active = 1;
                cpu->irq_hle.saved_cpsr = saved_cpsr;
                cpu->irq_hle.ret_pc = ret_pc;
                cpu->irq_hle.r[0] = cpu->r[0];
                cpu->irq_hle.r[1] = cpu->r[1];
                cpu->irq_hle.r[2] = cpu->r[2];
                cpu->irq_hle.r[3] = cpu->r[3];
                cpu->irq_hle.ip = cpu->r[12];
                cpu->irq_hle.log_count++;
                if (slot_fc & 1u)
                    cpu->cpsr |= CPSR_T;
                else
                    cpu->cpsr &= ~CPSR_T;
                cpu->r[15] = slot_fc & ~1u;
                cpu->r[14] = ret_pc;
            }
        }
        return 1;
    }
    /* 13.2：按 CPSR.T 位分发——Thumb 取 16 位半字，ARM 取 32 位字。 */
    if (cpu->cpsr & CPSR_T) {
        uint16_t insn16 = cpu_fetch16(cpu);
        cpu->cycles++;
        return thumb_step(cpu, insn16);
    }
    uint32_t insn = cpu_fetch(cpu);
    cpu->cycles++;
    return exec_step(cpu, insn);
}
