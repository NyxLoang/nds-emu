#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include "bios7_low.h"
#include "bios.h"
#include "cpu/cpu.h"
#include "cpu/exec.h"
#include "bus/bus.h"
#include "io/io.h"
#include "io/power.h"

/* FreeBIOS ARM7 的 SWI 函数表（镜像 0x10B0 起，逐字读出；共 31 项）。
   真机（FreeBIOS 二进制）里 0x1F 落到表外，会被读成代码字；真 BIOS 的
   0x1F 是 CustomHaltPost，这里按真机口径接到 0x1FA4（见 bioslog.md）。 */
static const uint32_t s_swi_table[31] = {
    0x00001FD8u, /* 00 SoftReset        */
    0x0000112Cu, /* 01 无效（直接进 swi_complete） */
    0x0000112Cu, /* 02 无效             */
    0x0000115Cu, /* 03 WaitByLoop       */
    0x00001190u, /* 04 IntrWait         */
    0x00001188u, /* 05 VBlankIntrWait   */
    0x0000114Cu, /* 06 Halt             */
    0x000011B8u, /* 07 Stop             */
    0x000011C8u, /* 08 SoundBias        */
    0x000011E4u, /* 09 Div              */
    0x0000112Cu, /* 0A 无效             */
    0x00001240u, /* 0B CpuSet           */
    0x000012C0u, /* 0C CpuFastSet       */
    0x00001300u, /* 0D Sqrt             */
    0x0000134Cu, /* 0E GetCRC16         */
    0x000013ECu, /* 0F IsDebugger       */
    0x000013F4u, /* 10 BitUnPack        */
    0x00001488u, /* 11 LZ77 WRAM        */
    0x00001504u, /* 12 LZ77 VRAM        */
    0x000015B0u, /* 13 Huffman          */
    0x000015B4u, /* 14 RL WRAM          */
    0x000015B4u, /* 15 RL VRAM          */
    0x0000112Cu, /* 16 无效（ARM9 专用） */
    0x0000112Cu, /* 17 无效             */
    0x0000112Cu, /* 18 无效（ARM9 专用） */
    0x0000112Cu, /* 19 无效             */
    0x0000168Cu, /* 1A GetSineTable     */
    0x00001CA0u, /* 1B GetPitchTable    */
    0x00001FC8u, /* 1C GetVolumeTable   */
    0x00001F88u, /* 1D GetBootProcs     */
    0x00001FA4u, /* 1E CustomHaltPost   */
};

/* 各条被等价实现的指令地址（FreeBIOS ARM7 反汇编逐条对应；switch 需要
   编译期常量，故用宏而不是 static const）。 */
#define A7_VEC_RESET        0x00000000u
#define A7_VEC_UNDEF        0x00000004u
#define A7_VEC_PABT         0x0000000Cu
#define A7_VEC_DABT         0x00000010u
#define A7_VEC_RSV          0x00000014u
#define A7_VEC_FIQ          0x0000001Cu

#define A7_HALT_MOV_R0      0x0000114Cu
#define A7_HALT_MOV_R2      0x00001150u
#define A7_HALT_STORE       0x00001154u
#define A7_HALT_B           0x00001158u
#define A7_LOOP_SUBS        0x0000115Cu
#define A7_LOOP_BGT         0x00001160u
#define A7_LOOP_B           0x00001164u
#define A7_CHK_LDR          0x00001168u
#define A7_CHK_IME0         0x0000116Cu
#define A7_CHK_TST          0x00001170u
#define A7_CHK_BIC          0x00001174u
#define A7_CHK_STORE        0x00001178u
#define A7_CHK_MOV1         0x0000117Cu
#define A7_CHK_IME1         0x00001180u
#define A7_CHK_BXLR         0x00001184u
#define A7_VBL_MOV_R0       0x00001188u
#define A7_VBL_MOV_R1       0x0000118Cu
#define A7_WAIT_MOV_R3      0x00001190u
#define A7_WAIT_CMP         0x00001194u
#define A7_WAIT_BNE         0x00001198u
#define A7_WAIT_MOV_80      0x0000119Cu
#define A7_WAIT_STORE       0x000011A0u
#define A7_WAIT_BL          0x000011A4u
#define A7_WAIT_BEQ         0x000011A8u
#define A7_WAIT_B           0x000011ACu
#define A7_WAITCHK_BL       0x000011B0u
#define A7_WAITCHK_B        0x000011B4u
#define A7_STOP_MOV_R0      0x000011B8u
#define A7_STOP_MOV_R1      0x000011BCu
#define A7_STOP_STORE       0x000011C0u
#define A7_STOP_B           0x000011C4u
#define A7_CHALT_MOV_R1     0x00001FA4u
#define A7_CHALT_STORE      0x00001FA8u
#define A7_CHALT_B          0x00001FACu
/* SWI 0 SoftReset（0x1FD8-0x202C）：清 WRAM 顶 512B → 重设 SVC/IRQ/System 栈
   → ldm r0,{r0-r12}（刚清零 → 全 0）→ movs pc,lr（System 模式跳转）。 */
#define A7_SOFT_MOV_R0      0x00001FD8u
#define A7_SOFT_CLR         0x00001FE4u
#define A7_SOFT_LDR_SP      0x00001FF0u
#define A7_SOFT_MSR_SVC     0x00001FF4u
#define A7_SOFT_SP_SVC      0x00001FFCu
#define A7_SOFT_LR_SVC      0x00002000u
#define A7_SOFT_SPSR_SVC    0x00002004u
#define A7_SOFT_MSR_IRQ     0x00002008u
#define A7_SOFT_SP_IRQ      0x00002010u
#define A7_SOFT_LR_IRQ      0x00002014u
#define A7_SOFT_SPSR_IRQ    0x00002018u
#define A7_SOFT_MSR_SYS     0x0000201Cu
#define A7_SOFT_SP_SYS      0x00002024u
#define A7_SOFT_LDM          0x00002028u
#define A7_SOFT_MOVS_PC      0x0000202Cu
#define A7_COMP_POP_LR      0x0000112Cu
#define A7_COMP_MOV_D3      0x00001130u
#define A7_COMP_MSR_CPSR    0x00001134u
#define A7_COMP_POP_SPSR    0x00001138u
#define A7_COMP_MSR_SPSR    0x0000113Cu
#define A7_COMP_POP3        0x00001140u
#define A7_COMP_MOVS_PC     0x00001144u
#define A7_IRQ_PUSH         0x00001FB0u
#define A7_IRQ_MOV_R0       0x00001FB4u
#define A7_IRQ_MOV_LR       0x00001FB8u
#define A7_IRQ_LDR_PC       0x00001FBCu
#define A7_IRQ_POP          0x00001FC0u
#define A7_IRQ_SUBS_PC      0x00001FC4u

/* 诊断：低地址入口/异常/返回桩的日志只打有限次，避免长跑刷屏。 */
static int g_diag_count;
static int g_swi_log;

/* 诊断环：记录最近 16 次低地址步入的 (PC, CPSR)，异常/死循环时回放，
   便于定位「是哪条 return/IRQ 出入口把 PC 带进了向量表」。 */
static uint32_t s_ring_pc[16];
static uint32_t s_ring_cpsr[16];
static uint32_t s_ring_lr[16];
static uint32_t s_ring_sp[16];
static unsigned s_ring_n;

static void diag(const char *fmt, ...)
{
    if (g_diag_count >= 128)
        return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    g_diag_count++;
}

/* SWI 表项：真机口径（n>=0x20 折到 1；0x1F 接 CustomHaltPost，见文件头说明）。 */
static uint32_t swi_body_addr(uint32_t n)
{
    if (n == 0x1Fu)
        return BIOS7_ADDR_SWI_CUSTOMHALT;
    if (n >= 0x20u)
        n = 1u;
    return s_swi_table[n];
}

/* 本模块“逐条等价实现”的函数体地址（其它函数交给 C 版 HLE 计算，再走尾段）。 */
static int is_modeled_body(uint32_t addr)
{
    return addr == BIOS7_ADDR_SWI_HALT
        || addr == BIOS7_ADDR_SWI_WAITLOOP
        || addr == BIOS7_ADDR_SWI_VBLANK
        || addr == BIOS7_ADDR_SWI_INTRWAIT
        || addr == BIOS7_ADDR_SWI_STOP
        || addr == A7_SOFT_MOV_R0          /* SWI 0 SoftReset */
        || addr == BIOS7_ADDR_SWI_CUSTOMHALT;
}

/* SUBS/CMP 的标志：N/Z 由结果给，C=无借位（old>=imm），V=有符号溢出。 */
static void flags_sub(arm_cpu_t *cpu, uint32_t old, uint32_t imm, uint32_t res)
{
    cpu->cpsr &= ~(CPSR_N | CPSR_Z | CPSR_C | CPSR_V);
    if (res & 0x80000000u)
        cpu->cpsr |= CPSR_N;
    if (res == 0)
        cpu->cpsr |= CPSR_Z;
    if (old >= imm)
        cpu->cpsr |= CPSR_C;
    if (((old ^ imm) & (old ^ res)) & 0x80000000u)
        cpu->cpsr |= CPSR_V;
}

/* TST（逻辑与）只更新 N/Z；C/V 保持原值。 */
static void flags_tst(arm_cpu_t *cpu, uint32_t res)
{
    cpu->cpsr &= ~(CPSR_N | CPSR_Z);
    if (res & 0x80000000u)
        cpu->cpsr |= CPSR_N;
    if (res == 0)
        cpu->cpsr |= CPSR_Z;
}

static void push32(arm_cpu_t *cpu, uint32_t val)
{
    cpu->r[13] -= 4u;
    bus_write32(cpu->nds->bus, cpu->r[13], val);
}

static uint32_t pop32(arm_cpu_t *cpu)
{
    uint32_t v = bus_read32(cpu->nds->bus, cpu->r[13]);
    cpu->r[13] += 4u;
    return v;
}

int bios7_low_step(arm_cpu_t *cpu)
{
    struct bus *bus = cpu->nds->bus;
    io_t *io = cpu->nds->io;
    uint32_t pc = cpu->r[15];

    s_ring_pc[s_ring_n & 15u] = pc;
    s_ring_cpsr[s_ring_n & 15u] = cpu->cpsr;
    s_ring_lr[s_ring_n & 15u] = cpu->r[14];
    s_ring_sp[s_ring_n & 15u] = cpu->r[13];
    s_ring_n++;

    switch (pc) {
    /* ---------- 异常向量（低地址 0x0000-0x001C）---------- */
    case BIOS7_ADDR_SWI_VEC:                 /* b swi_handler */
        cpu->r[15] = BIOS7_ADDR_SWI_HANDLER;
        cpu->step_cycles = 3;
        return 1;
    case BIOS7_ADDR_IRQ_VEC:                 /* b interrupt_handler */
        cpu->r[15] = BIOS7_ADDR_IRQ_HANDLER;
        cpu->step_cycles = 3;
        return 1;
    case A7_VEC_RESET:                       /* b boot_handler（b 自己） */
        cpu->r[15] = BIOS7_ADDR_BOOT_LOOP;
        cpu->step_cycles = 3;
        return 1;
    case A7_VEC_UNDEF:                       /* b unhandled_exception */
    case A7_VEC_PABT:
    case A7_VEC_DABT:
    case A7_VEC_RSV:
    case A7_VEC_FIQ:
        if (bus->diag) {
            int idx = exec_spsr_index(cpu->cpsr & CPSR_MODE_MASK);
            uint32_t fault_cpsr = (idx >= 0) ? cpu->spsr[idx] : 0;
            diag("bios7: unhandled exception vector=%08X fault_pc=%08X"
                 " cpsr=%08X\n", pc, cpu->r[14] - 4u, fault_cpsr);
            unsigned base = (s_ring_n >= 16u) ? s_ring_n - 16u : 0u;
            for (unsigned i = 0; i < 16u && i < s_ring_n; i++) {
                unsigned k = (base + i) & 15u;
                diag("bios7:   ring[%2u] pc=%08X cpsr=%08X lr=%08X sp=%08X\n",
                     i, s_ring_pc[k], s_ring_cpsr[k], s_ring_lr[k], s_ring_sp[k]);
            }
        }
        cpu->r[15] = BIOS7_ADDR_UNHANDLED;
        cpu->step_cycles = 3;
        return 1;
    case BIOS7_ADDR_BOOT_LOOP:
    case BIOS7_ADDR_UNHANDLED:
        /* 真机是 `b .` 死循环：PC 不变（IRQ 仍可在每步边界插入）。 */
        if (bus->diag)
            diag("bios7: low-address trap loop pc=%08X cpsr=%08X (ref: b .)\n",
                 pc, cpu->cpsr);
        cpu->step_cycles = 2;
        return 1;

    /* ---------- SWI 分发器（0x1080）----------
       真机指令序列：
         push {r4,r12,lr} / mrs r4,SPSR / push {r4}
         r4=(SPSR&0x80)|0x1F / r12=byte[lr-2] / msr CPSR_fc,r4
         push {lr}(System) / if(r12>=0x20) r12=1 / ldr pc,[pc,r12,lsl#2] */
    case BIOS7_ADDR_SWI_HANDLER: {
        uint32_t spsr = cpu->spsr[2];        /* SPSR_svc = 调用方 CPSR */
        uint32_t lr_in = cpu->r[14];         /* SVC lr = SWI 之后返回地址 */
        uint32_t sp = cpu->r[13];            /* SVC 栈 */
        /* stmdb sp!, {r4, r12, lr}：低编号寄存器放低地址 */
        bus_write32(bus, sp - 12u, cpu->r[4]);
        bus_write32(bus, sp - 8u,  cpu->r[12]);
        bus_write32(bus, sp - 4u,  lr_in);
        bus_write32(bus, sp - 16u, spsr);
        cpu->r[13] = sp - 16u;

        /* SWI 编号 = 指令字节（ARM: imm24>>16 / Thumb: imm8），真机用
           ldrb r12,[lr,#-2] 取；lr-2 正好落在两种编码的编号字节上。 */
        uint32_t comment = bus_read8(bus, lr_in - 2u);
        if (bus->diag && g_swi_log < 24) {
            g_swi_log++;
            diag("bios7: swi 0x%02X caller=%08X lr=%08X cpsr=%08X\n",
                 comment, lr_in - ((spsr & CPSR_T) ? 2u : 4u), lr_in, spsr);
        }

        /* msr CPSR_fc, (SPSR&0x80)|0x1F：只写控制位+标志位
           → 模式 System、T/F=0、I 取自 SPSR、N/Z/C/V 清 0 */
        cpu->r[4] = (spsr & CPSR_I) | ARM_MODE_SYS;
        exec_apply_cpsr(cpu, cpu->r[4]);

        push32(cpu, cpu->r[14]);             /* System 模式的 lr */

        /* 真机 `cmp r12,#0x20`：标志要按 (n - 0x20) 更新（参考核在函数体入口
           看到 N=1，因为 SWI 编号 < 0x20 使差为负）；随后的 movge 用它选 1。 */
        flags_sub(cpu, comment, 0x20u, comment - 0x20u);
        if (comment >= 0x20u)                /* 真机 movge r12,#1 */
            comment = 1u;
        cpu->r[12] = comment;

        uint32_t body = swi_body_addr(comment);
        cpu->step_cycles = 6;
        if (body == BIOS7_ADDR_SWI_COMPLETE) {
            /* 无效号：真机表项直接指向 swi_complete，等于空操作 */
            cpu->r[15] = body;
            return 1;
        }
        if (is_modeled_body(body)) {
            cpu->r[15] = body;
            return 1;
        }
        /* 其它函数：交给 C 版 HLE 算出 r0-r3 结果，再走 swi_complete 收尾，
           这样入栈/切模式/恢复 SPSR 的副作用与真机一致。 */
        (void)bios_dispatch(comment, cpu);
        cpu->r[15] = BIOS7_ADDR_SWI_COMPLETE;
        return 1;
    }

    /* ---------- SWI 6 Halt：mov r0,#0x04000000 / mov r2,#0x80 / strb ---------- */
    case A7_HALT_MOV_R0:
        cpu->r[0] = 0x04000000u;
        cpu->r[15] = A7_HALT_MOV_R2;
        return 1;
    case A7_HALT_MOV_R2:
        cpu->r[2] = 0x80u;
        cpu->r[15] = A7_HALT_STORE;
        return 1;
    case A7_HALT_STORE:
        /* 写 HALTCNT=0x80：本指令执行完 CPU 暂停，下一条（b swi_complete）
           还没取。IRQ 若在此刻到来，被打断 PC 就是 0x1158——参考核的
           IRQ 六字帧里保存的正是这个值。 */
        power_halt_request(&io->power, (uint8_t)(cpu->r[2] & 0xFFu));
        cpu->r[15] = A7_HALT_B;
        cpu->step_cycles = 2;
        return 1;
    case A7_HALT_B:                          /* b swi_complete */
        cpu->r[15] = BIOS7_ADDR_SWI_COMPLETE;
        cpu->step_cycles = 2;
        return 1;

    /* ---------- SWI 3 WaitByLoop：subs r0,#1 / bgt / b swi_complete ---------- */
    case A7_LOOP_SUBS: {
        uint32_t old = cpu->r[0];
        uint32_t res = old - 1u;
        cpu->r[0] = res;
        flags_sub(cpu, old, 1u, res);
        cpu->r[15] = A7_LOOP_BGT;
        return 1;
    }
    case A7_LOOP_BGT: {
        int taken = ((cpu->cpsr & CPSR_Z) == 0)
                 && (((cpu->cpsr & CPSR_N) != 0) == ((cpu->cpsr & CPSR_V) != 0));
        cpu->r[15] = taken ? A7_LOOP_SUBS : A7_LOOP_B;
        cpu->step_cycles = 2;                /* 参考核：subs 1 + bgt 2 = 3 周期/轮 */
        return 1;
    }
    case A7_LOOP_B:
        cpu->r[15] = BIOS7_ADDR_SWI_COMPLETE;
        cpu->step_cycles = 2;
        return 1;

    /* ---------- interrupt_check 子程序（0x1168，由 bl 进入）----------
       读软件中断标志（0x03FFFFF8）→ IME=0 → tst/bic 清标志 → 写回 → IME=1
       → bx lr。Z 标志由调用方的 beq 使用。 */
    case A7_CHK_LDR:
        cpu->r[0] = bus_read32(bus, cpu->r[3] - 8u);
        cpu->r[15] = A7_CHK_IME0;
        return 1;
    case A7_CHK_IME0:
        io->irq[1].ime = 0;                  /* str r3,[r3,#0x208] → bit0=0 */
        cpu->r[15] = A7_CHK_TST;
        return 1;
    case A7_CHK_TST:
        flags_tst(cpu, cpu->r[1] & cpu->r[0]);
        cpu->r[15] = A7_CHK_BIC;
        return 1;
    case A7_CHK_BIC:
        cpu->r[0] &= ~cpu->r[1];
        cpu->r[15] = A7_CHK_STORE;
        return 1;
    case A7_CHK_STORE:
        bus_write32(bus, cpu->r[3] - 8u, cpu->r[0]);
        cpu->r[15] = A7_CHK_MOV1;
        return 1;
    case A7_CHK_MOV1:
        cpu->r[12] = 1u;
        cpu->r[15] = A7_CHK_IME1;
        return 1;
    case A7_CHK_IME1:
        io->irq[1].ime = 1;
        cpu->r[15] = A7_CHK_BXLR;
        return 1;
    case A7_CHK_BXLR: {                      /* bx lr（返回调用方，Z 保留） */
        uint32_t lr = cpu->r[14];
        if (lr & 1u)
            cpu->cpsr |= CPSR_T;
        else
            cpu->cpsr &= ~CPSR_T;
        cpu->r[15] = lr & ~1u;
        cpu->step_cycles = 3;
        return 1;
    }

    /* ---------- SWI 5 VBlankIntrWait：mov r0,#1 / mov r1,#1（落进 IntrWait）---------- */
    case A7_VBL_MOV_R0:
        cpu->r[0] = 1u;
        cpu->r[15] = A7_VBL_MOV_R1;
        return 1;
    case A7_VBL_MOV_R1:
        cpu->r[1] = 1u;
        cpu->r[15] = A7_WAIT_MOV_R3;
        return 1;

    /* ---------- SWI 4 IntrWait / SWI 5 主循环 ---------- */
    case A7_WAIT_MOV_R3:
        cpu->r[3] = 0x04000000u;
        cpu->r[15] = A7_WAIT_CMP;
        return 1;
    case A7_WAIT_CMP:
        flags_sub(cpu, cpu->r[0], 0u, cpu->r[0]);
        cpu->r[15] = A7_WAIT_BNE;
        return 1;
    case A7_WAIT_BNE:                        /* bne swi_interrupt_check_first */
        cpu->r[15] = (cpu->cpsr & CPSR_Z) ? A7_WAIT_MOV_80 : A7_WAITCHK_BL;
        cpu->step_cycles = 3;
        return 1;
    case A7_WAIT_MOV_80:
        cpu->r[0] = 0x80u;
        cpu->r[15] = A7_WAIT_STORE;
        return 1;
    case A7_WAIT_STORE:                      /* strb r0,[r3,#0x301] → 暂停 */
        power_halt_request(&io->power, (uint8_t)(cpu->r[0] & 0xFFu));
        cpu->r[15] = A7_WAIT_BL;
        cpu->step_cycles = 2;
        return 1;
    case A7_WAIT_BL:                         /* bl interrupt_check */
        cpu->r[14] = A7_WAIT_BEQ;
        cpu->r[15] = BIOS7_ADDR_INTR_CHECK;
        return 1;
    case A7_WAIT_BEQ:                        /* beq 循环（Z=标志未命中） */
        cpu->r[15] = (cpu->cpsr & CPSR_Z) ? A7_WAIT_MOV_80 : A7_WAIT_B;
        cpu->step_cycles = 3;
        return 1;
    case A7_WAIT_B:                          /* b swi_complete */
        cpu->r[15] = BIOS7_ADDR_SWI_COMPLETE;
        cpu->step_cycles = 2;
        return 1;
    case A7_WAITCHK_BL:                      /* check_first: bl + b 主循环 */
        cpu->r[14] = A7_WAITCHK_B;
        cpu->r[15] = BIOS7_ADDR_INTR_CHECK;
        return 1;
    case A7_WAITCHK_B:
        cpu->r[15] = A7_WAIT_MOV_80;
        cpu->step_cycles = 2;
        return 1;

    /* ---------- SWI 7 Stop（sleep 请求，0xC0）---------- */
    case A7_STOP_MOV_R0:
        cpu->r[0] = 0x04000000u;
        cpu->r[15] = A7_STOP_MOV_R1;
        return 1;
    case A7_STOP_MOV_R1:
        cpu->r[1] = 0xC0u;
        cpu->r[15] = A7_STOP_STORE;
        return 1;
    case A7_STOP_STORE:
        power_halt_request(&io->power, (uint8_t)(cpu->r[1] & 0xFFu));
        cpu->r[15] = A7_STOP_B;
        cpu->step_cycles = 2;
        return 1;
    case A7_STOP_B:
        cpu->r[15] = BIOS7_ADDR_SWI_COMPLETE;
        cpu->step_cycles = 2;
        return 1;

    /* ---------- CustomHaltPost（HALTCNT = r0）---------- */
    case A7_CHALT_MOV_R1:
        cpu->r[1] = 0x04000000u;
        cpu->r[15] = A7_CHALT_STORE;
        return 1;
    case A7_CHALT_STORE:
        power_halt_request(&io->power, (uint8_t)(cpu->r[0] & 0xFFu));
        cpu->r[15] = A7_CHALT_B;
        cpu->step_cycles = 2;
        return 1;
    case A7_CHALT_B:
        cpu->r[15] = BIOS7_ADDR_SWI_COMPLETE;
        cpu->step_cycles = 2;
        return 1;

    /* ---------- SWI 0 SoftReset（0x1FD8）----------
       FFXII 的 ARM7 用它在任务切换时“重设 BIOS 状态”：清 WRAM 顶 512 字节
       （含 0x03FFFFF8 软件中断标志与 0x03FFFFFC handler 槽）、把 SVC/IRQ/System
       三套栈设回 FreeBIOS 值（0x0380FFDC/0x0380FFB0/0x0380FF00）、r0-r12 全清 0，
       最后 `movs pc,lr` 按 System 模式 lr 跳转（不是 swi_complete 返回）。
       旧实现把它当未知号空操作，恢复 BIOS 内部现场时会顺着旧栈帧弹到野地址。 */
    case A7_SOFT_MOV_R0:                     /* mov r0,#0x04000000 / r1=0x80 / r2=0 */
        cpu->r[0] = 0x04000000u;
        cpu->r[1] = 0x80u;
        cpu->r[2] = 0u;
        cpu->r[15] = A7_SOFT_CLR;
        cpu->step_cycles = 3;
        return 1;
    case A7_SOFT_CLR: {                      /* str r2,[r0,#-4]! / subs / bne */
        for (uint32_t i = 1; i <= 0x80u; i++)
            bus_write32(bus, 0x04000000u - 4u * i, 0u);
        cpu->r[0] = 0x03FFFE00u;             /* 循环结束时 r0 停在清空区底 */
        cpu->r[1] = 0u;
        cpu->r[15] = A7_SOFT_LDR_SP;
        cpu->step_cycles = 0x80u * 3u;
        return 1;
    }
    case A7_SOFT_LDR_SP:                     /* ldr r1,[0x1FD4] = 0x0380FFDC */
        cpu->r[1] = 0x0380FFDCu;
        cpu->r[15] = A7_SOFT_MSR_SVC;
        return 1;
    case A7_SOFT_MSR_SVC:                    /* mov r3,#0xD3 / msr cpsr_fsxc,r3 */
        exec_apply_cpsr(cpu, 0xD3u);
        cpu->r[15] = A7_SOFT_SP_SVC;
        cpu->step_cycles = 3;
        return 1;
    case A7_SOFT_SP_SVC:                     /* mov sp,r1 */
        cpu->r[13] = cpu->r[1];
        cpu->r[15] = A7_SOFT_LR_SVC;
        return 1;
    case A7_SOFT_LR_SVC:                     /* mov lr,r2 */
        cpu->r[14] = cpu->r[2];
        cpu->r[15] = A7_SOFT_SPSR_SVC;
        return 1;
    case A7_SOFT_SPSR_SVC:                   /* msr spsr_fsxc,r2 */
        cpu->spsr[2] = cpu->r[2];
        cpu->r[15] = A7_SOFT_MSR_IRQ;
        return 1;
    case A7_SOFT_MSR_IRQ:                    /* mov r3,#0xD2 / msr cpsr_fsxc,r3 */
        exec_apply_cpsr(cpu, 0xD2u);
        cpu->r[15] = A7_SOFT_SP_IRQ;
        cpu->step_cycles = 3;
        return 1;
    case A7_SOFT_SP_IRQ:                     /* sub sp,r1,#0x2C */
        cpu->r[13] = cpu->r[1] - 0x2Cu;
        cpu->r[15] = A7_SOFT_LR_IRQ;
        return 1;
    case A7_SOFT_LR_IRQ:                     /* mov lr,r2 */
        cpu->r[14] = cpu->r[2];
        cpu->r[15] = A7_SOFT_SPSR_IRQ;
        return 1;
    case A7_SOFT_SPSR_IRQ:                   /* msr spsr_fsxc,r2 */
        cpu->spsr[1] = cpu->r[2];
        cpu->r[15] = A7_SOFT_MSR_SYS;
        return 1;
    case A7_SOFT_MSR_SYS:                    /* mov r3,#0x5F / msr cpsr_fsxc,r3 */
        exec_apply_cpsr(cpu, 0x5Fu);
        cpu->r[15] = A7_SOFT_SP_SYS;
        cpu->step_cycles = 3;
        return 1;
    case A7_SOFT_SP_SYS:                     /* sub sp,r1,#0xDC */
        cpu->r[13] = cpu->r[1] - 0xDCu;
        cpu->r[15] = A7_SOFT_LDM;
        return 1;
    case A7_SOFT_LDM: {                      /* ldm r0,{r0-r12}：清空区读回 0 */
        uint32_t base = cpu->r[0];           /* LDM 的基址在执行开始时取一次 */
        for (int i = 0; i <= 12; i++)
            cpu->r[i] = bus_read32(bus, base + 4u * (uint32_t)i);
        cpu->r[15] = A7_SOFT_MOVS_PC;
        cpu->step_cycles = 13;
        return 1;
    }
    case A7_SOFT_MOVS_PC: {                  /* movs pc,lr：System 模式普通 MOVS */
        cpu->r[15] = cpu->r[14];
        cpu->step_cycles = 3;
        return 1;
    }

    /* ---------- swi_complete（0x112C）：恢复 System lr → SVC → SPSR → 返回 ---------- */
    case A7_COMP_POP_LR:                     /* ldm sp!,{lr}（System 栈） */
        cpu->r[14] = pop32(cpu);
        cpu->r[15] = A7_COMP_MOV_D3;
        return 1;
    case A7_COMP_MOV_D3:
        cpu->r[4] = 0xD3u;
        cpu->r[15] = A7_COMP_MSR_CPSR;
        return 1;
    case A7_COMP_MSR_CPSR:                   /* msr cpsr_fc,r4：控制位=0xD3、标志字段=0 */
        exec_apply_cpsr(cpu, (cpu->cpsr & ~0xF00000FFu) | (cpu->r[4] & 0xF00000FFu));
        cpu->r[15] = A7_COMP_POP_SPSR;
        cpu->step_cycles = 3;
        return 1;
    case A7_COMP_POP_SPSR:                   /* ldm sp!,{r4}（SVC 栈） */
        cpu->r[4] = pop32(cpu);
        cpu->r[15] = A7_COMP_MSR_SPSR;
        return 1;
    case A7_COMP_MSR_SPSR:                   /* msr spsr_fc,r4（把原 SPSR 写回） */
        cpu->spsr[2] = (cpu->spsr[2] & ~0xF00000FFu)
                     | (cpu->r[4] & 0xF00000FFu);
        cpu->r[15] = A7_COMP_POP3;
        return 1;
    case A7_COMP_POP3:                       /* ldm sp!,{r4,r12,lr} */
        cpu->r[4]  = pop32(cpu);
        cpu->r[12] = pop32(cpu);
        cpu->r[14] = pop32(cpu);
        cpu->r[15] = A7_COMP_MOVS_PC;
        return 1;
    case A7_COMP_MOVS_PC: {                  /* movs pc,lr：用 SPSR_svc 恢复 CPSR */
        uint32_t pc_out = cpu->r[14];
        exec_apply_cpsr(cpu, cpu->spsr[2]);
        cpu->r[15] = pc_out;
        cpu->step_cycles = 3;
        return 1;
    }

    /* ---------- IRQ 入口（0x1FB0）---------- */
    case A7_IRQ_PUSH: {                      /* push {r0-r3,r12,lr} */
        push32(cpu, cpu->r[14]);
        push32(cpu, cpu->r[12]);
        push32(cpu, cpu->r[3]);
        push32(cpu, cpu->r[2]);
        push32(cpu, cpu->r[1]);
        push32(cpu, cpu->r[0]);
        cpu->r[15] = A7_IRQ_MOV_R0;
        cpu->step_cycles = 6;
        return 1;
    }
    case A7_IRQ_MOV_R0:
        cpu->r[0] = 0x04000000u;
        cpu->r[15] = A7_IRQ_MOV_LR;
        return 1;
    case A7_IRQ_MOV_LR:                      /* mov lr,pc → lr = 0x1FC0（返回桩） */
        cpu->r[14] = BIOS7_ADDR_IRQ_RETURN;
        cpu->r[15] = A7_IRQ_LDR_PC;
        return 1;
    case A7_IRQ_LDR_PC: {                    /* ldr pc,[r0,#-4] → 用户 handler */
        uint32_t handler = bus_read32(bus, cpu->r[0] - 4u);
        if (handler == 0) {
            /* 真机就是一条 ldr pc，值为 0 时跳到 0（随后沿复位向量进 boot 死循环）——
               照抄参考行为，不要在这里“停住等待”：停在原地会让 ARM7 永远不推进。 */
            if (bus->diag)
                diag("bios7: IRQ handler slot empty (%08X): jump to 0 (ref)\n",
                     BIOS7_IRQ_HANDLER_SLOT, 0);
        }
        if (handler & 1u)
            cpu->cpsr |= CPSR_T;
        else
            cpu->cpsr &= ~CPSR_T;
        cpu->r[15] = handler & ~1u;
        cpu->step_cycles = 3;
        return 1;
    }
    case A7_IRQ_POP:                         /* pop {r0-r3,r12,lr} */
        cpu->r[0]  = pop32(cpu);
        cpu->r[1]  = pop32(cpu);
        cpu->r[2]  = pop32(cpu);
        cpu->r[3]  = pop32(cpu);
        cpu->r[12] = pop32(cpu);
        cpu->r[14] = pop32(cpu);
        cpu->r[15] = A7_IRQ_SUBS_PC;
        cpu->step_cycles = 6;
        return 1;
    case A7_IRQ_SUBS_PC: {                   /* subs pc,lr,#4：异常返回（SPSR_irq） */
        uint32_t ret = cpu->r[14] - 4u;
        exec_apply_cpsr(cpu, cpu->spsr[1]);
        cpu->r[15] = ret;
        cpu->step_cycles = 3;
        return 1;
    }

    default:
        return 0;                            /* 不在本模块覆盖范围内 */
    }
}
