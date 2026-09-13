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

/* 21-B9yi(续52)：诊断开关的一次性初始化（见下方定义）。 */
static void cpu_diag_init(void);

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
    /* 21-B9yi(续52)：把诊断开关的 getenv/解析**一次性**做完（幂等）。
       此前它们散在热路径里，每条指令都要判一次「是否已初始化」。 */
    cpu_diag_init();
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
    bus_t *bus = cpu->nds->bus;
    int prev = bus->in_code_fetch;
    bus->in_code_fetch = 1;            /* 21-B9yi(续108g)：取指不计数据代价 */
    uint32_t v = bus_read32(bus, cpu->r[15]);
    bus->in_code_fetch = prev;
    return v;
}

/* 13.2 Thumb 取指：按 PC 从总线读 16 位半字指令。 */
uint16_t cpu_fetch16(const arm_cpu_t *cpu)
{
    bus_t *bus = cpu->nds->bus;
    int prev = bus->in_code_fetch;
    bus->in_code_fetch = 1;
    uint16_t v = bus_read16(bus, cpu->r[15]);
    bus->in_code_fetch = prev;
    return v;
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

/* 21-B9yi(续108g)：**ARM9 访存代价模型**（`NDS_MEMTIM=1`，默认关）。

   为什么：跨核对账量出「CPU 密集相（开机/装载/重场景）本地 ARM9 的指令吞吐是参考核的
   ~1.45 倍」——本机对每条指令只计 1 个系统单位（=2 个 ARM9 周期）且**完全不建模数据
   访问代价**，而参考核 melonDS 按 `MemTimings` 给「取指 + 数据访问」按区域计费。

   本模型按 melonDS 的表折算成 **ARM9 周期**（runner 侧用 ÷2 折成系统单位，见
   `s_arm9_shift`）：

     取指：**2 周期**（ITCM/DTCM、以及被 I-cache 命中的主存代码；
           未命中时才贵——melonDS 主存非顺序取指 N32=9 ⇒ 18 周期）。
     数据：由 bus 侧按访问地址累加（`bus_data_cost_add`）：**1 周期/次**
           （melonDS 的 CP15 缓存命中路径就是 `DataCycles = 1`）。

   本步合计 = max(取指, 数据, 取指+数据-6)（melonDS 的 AddCycles_CDI 公式，
   「取指与数据部分流水重叠」）。
   注：真实 ARM9 有 I/D cache（melonDS 用 CP15 的 ICacheLookup 建模）；本模拟器
   不建 cache，于是**用「命中口径的常数代价」近似**，再用
   `REF_INSTRSTAT` 的每帧指令数校准（见 docs/21-rom-bringup.md 续108g）。
   仅 ARM9；ARM7 保持 1 单位/条（其代码大多在 WRAM，且实测本机 ARM7 指令数本就偏低）。 */
static int s_memtim = -1;

int cpu_memtim_enabled(void)
{
    if (s_memtim < 0) {
        const char *e = getenv("NDS_MEMTIM");
        s_memtim = (e != NULL && e[0] != '0' && e[0] != '\0') ? 1 : 0;
    }
    return s_memtim;
}

void cpu_set_nonseq_cost(int v)
{
    if (v >= 1 && v <= 8)
        s_nonseq_cost = v;
}

static uint32_t cpu_fetch_cost(const arm_cpu_t *cpu, uint32_t pc, int nonseq)
{
    /* 21-B9yi(续52)：惰性初始化已挪到 cpu_diag_init()（cpu_create 时做一次），
       这里直接读全局值，热路径不再有「是否初始化过」的判断。 */
    if (cpu->is_arm7)
        return 1u;
    if (cpu_memtim_enabled()) {
        /* 21-B9yi(续108g)：取指按「cache 命中」口径计 2 个 ARM9 周期（=1 系统单位）。
           未命中/冷启动的 18 周期惩罚**不在这里**建模（实测直接按 18 计会把吞吐压到
           参考核的 0.47 倍——因为本模拟器没有 I-cache，命中率恒为「冷」）。
           这个常数的合理性由 `REF_INSTRSTAT` 的每帧指令数校准。 */
        (void)nonseq;
        return 2u;
    }
    if (pc - BUS_ARM9_ITCM_BASE < BUS_ARM9_ITCM_SIZE)
        return 1u;                           /* ITCM：melonDS 恒 1 */
    if ((pc >> 24) != 0x02u)
        return 1u;                           /* WRAM/IO/VRAM/BIOS 等 */
    return nonseq ? (uint32_t)s_nonseq_cost : 1u; /* 主存：非顺序取指更贵 */
}

/* ---------------------------------------------------------------------------
   21-B9yi(续50/续54) 阶段剖析（**编译期**诊断设施，`-DNDS_PROF=ON` 才编进去）。

   为什么需要它：本机没有可用的采样 profiler —— gprof 在这套 MinGW 上连
   「hello + 忙循环」都采不到样本（flat profile 恒为空），而 LTO 又让符号消失。
   于是用 rdtsc 手工把每条指令的时间拆成两块：
     · IO 推进：`io_advance_timers`（4 个定时器）+ `io_advance_cart`
       （GX 时钟、卡带时钟、DRQ→DMA 检查）
     · 其余：取指 / 译码 / 执行 / 访存 / IRQ 检查
   回答「下一步该优化解释器还是该优化 IO 推进」这个方向性问题。
   关闭时只多一次全局 int 判断；开启时每条指令多两次 rdtsc（~30 周期，
   对占比判断影响很小；但**绝对值不能当性能基线**）。
   --------------------------------------------------------------------------- */
/* 21-B9yi(续54)：只有 `-DNDS_PROF=ON` 的构建才在热路径上插桩（见 CMakeLists 注释）。 */
#ifdef NDS_PROF_BUILD
static unsigned long long s_prof_steps, s_prof_io, s_prof_all;
/* 21-B9yi(续53)：更细的归因（取指 / 译码执行），配合 s_prof_io 决定下一刀砍哪里。 */
static unsigned long long s_prof_fetch, s_prof_exec;
/* 21-B9yi(续55)：把 IO 推进再拆成「定时器」与「卡带/GX 时钟」两块
   （两者优化手段完全不同：前者可做事件化，后者可做活动位守卫）。 */
static unsigned long long s_prof_tmr, s_prof_cart;
/* 21-B9yi(续63)：**抽样**开关。此前每条指令都插 rdtsc（一进一出共 8 次以上），
   而每条指令本身才 ~100 个周期 —— 测量开销把「other」桶撑到 50%+，占比失真。
   改成每 64 条指令只测一条：单条测量误差仍在，但**占比可信**（误差被摊薄 64 倍）。 */
static unsigned s_prof_seq;
static int s_prof_meas;
static unsigned long long s_prof_first_tsc;   /* 首次进入 cpu_step 的 TSC（算占墙钟比例） */
static int s_prof_on = -1;
/* 插桩宏：`NDS_PROF_BUILD` 构建里按开关决定是否读时钟；
   默认构建里两个宏都退化成「什么都不做 / 常量 0」，编译器会把整段分支消掉。 */
#define PROF_T0() ((s_prof_meas) ? cpu_rdtsc() : 0ULL)
#define PROF_ADD(counter, t0) \
    do { if (s_prof_meas) (counter) += cpu_rdtsc() - (t0); } while (0)
#define PROF_STEP_END(t0) \
    do { if (s_prof_meas) { s_prof_steps++; s_prof_all += cpu_rdtsc() - (t0); } } while (0)
#else
#define PROF_T0() 0ULL
#define PROF_ADD(counter, t0) do { (void)(t0); } while (0)
#define PROF_STEP_END(t0) do { (void)(t0); } while (0)
#endif

/* 21-B9yi(续52)：`NDS_PCSAMPLE=LO-HI@N` 解析结果（原来在 cpu_step 里惰性解析）。 */
static int s_ps_on;
static long s_ps_lo, s_ps_hi;
static unsigned s_ps_period = 200, s_ps_seen;

#ifdef NDS_PROF_BUILD
static unsigned long long cpu_rdtsc(void)
{
    unsigned int lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | lo;
}

static void cpu_prof_report(void)
{
    if (s_prof_steps == 0 || s_prof_all == 0)
        return;
    double fetch_pct = 100.0 * (double)s_prof_fetch / (double)s_prof_all;
    double exec_pct = 100.0 * (double)s_prof_exec / (double)s_prof_all;
    double tmr_pct = 100.0 * (double)s_prof_tmr / (double)s_prof_all;
    double cart_pct = 100.0 * (double)s_prof_cart / (double)s_prof_all;
    /* 21-B9yi(续63)：**自校准** —— 先量一次「一组 rdtsc 进出」的固有成本，
       再从每个桶里扣掉，否则测出来的占比会被测量本身撑大（实测能撑到 2.3 倍）。 */
    unsigned long long cal = 0;
    for (int i = 0; i < 1000; i++) {
        unsigned long long a = cpu_rdtsc();
        unsigned long long b = cpu_rdtsc();
        cal += b - a;
    }
    cal /= 1000;   /* 一组「进出」的固有成本（周期） */
    double steps = (double)s_prof_steps;
    double per_tmr  = ((double)s_prof_tmr  / steps) - (double)cal;
    double per_cart = ((double)s_prof_cart / steps) - (double)cal;
    double per_fetch= ((double)s_prof_fetch/ steps) - (double)cal;
    double per_exec = ((double)s_prof_exec / steps) - (double)cal;
    double per_all  = ((double)s_prof_all  / steps) - (double)cal;
    if (per_tmr  < 0) per_tmr  = 0;
    if (per_cart < 0) per_cart = 0;
    if (per_fetch< 0) per_fetch= 0;
    if (per_exec < 0) per_exec = 0;
    double per_other = per_all - per_tmr - per_cart - per_fetch - per_exec;
    if (per_other < 0) per_other = 0;
    printf("prof: 抽样 1/64，rdtsc 自校准=%llu 周期/组；每条指令（校正后）：\n"
           "      定时器 %.1f  卡带/GX %.1f  取指 %.1f  译码执行 %.1f  其余(检查/记账/调用) %.1f"
           "  合计 %.1f 周期\n"
           "      占比：定时器 %.1f%%  卡带/GX %.1f%%  取指 %.1f%%  执行 %.1f%%  其余 %.1f%%\n"
           "      steps=%llu  raw: tmr=%.1f%% cart=%.1f%% fetch=%.1f%% exec=%.1f%%\n",
           cal, per_tmr, per_cart, per_fetch, per_exec, per_other, per_all,
           100.0 * per_tmr / per_all, 100.0 * per_cart / per_all,
           100.0 * per_fetch / per_all, 100.0 * per_exec / per_all,
           100.0 * per_other / per_all,
           s_prof_steps, tmr_pct, cart_pct, fetch_pct, exec_pct);
    /* 21-B9yi(续63)：`cpu_step` 占整个运行墙钟的比例。
       抽样是 1/64，所以把测到的步时间 ×64 近似成全部步时间。
       这个数字回答「就算把解释器优化到 0，最多能快多少」。 */
    if (s_prof_first_tsc != 0) {
        unsigned long long total = cpu_rdtsc() - s_prof_first_tsc;
        if (total != 0)
            printf("prof: cpu_step 占墙钟≈%.1f%%（解释器优化上限）\n",
                   100.0 * (double)(s_prof_all * 64ull) / (double)total);
    }
}
#endif /* NDS_PROF_BUILD */

/* 21-B9yi(续52)：诊断开关的一次性初始化（由 cpu_create 调用，幂等）。

   动机：这些开关原本在**热路径**里用「静态变量 == -1 就解析一次」的写法，
   等于每条指令都要多判断一次「初始化过没有」。挪到创建 CPU 时做一次后，
   热路径只剩普通全局变量读取（`s_nonseq_cost` / `s_ps_on` / `s_prof_on`）。

   注意顺序：`s_prof_on` 初始化时若为真，要注册 `atexit(cpu_prof_report)`。 */
static void cpu_diag_init(void)
{
    static int done;
    if (done)
        return;
    done = 1;

    if (s_nonseq_cost < 0) {
        const char *e = getenv("NDS_ARM9_NOSEQ");
        s_nonseq_cost = 1;   /* 实测默认值：见 docs/21-rom-bringup.md 的 21-B9yi */
        if (e != NULL) {
            int v = atoi(e);
            if (v >= 1 && v <= 8)
                s_nonseq_cost = v;
        }
    }

    {
        const char *e = getenv("NDS_PCSAMPLE");
        s_ps_on = (e != NULL && e[0] != '0' && e[0] != '\0') ? 1 : 0;
        s_ps_lo = 0;
        s_ps_hi = -1;
        s_ps_period = 200;
        if (s_ps_on && e != NULL) {
            long lo = 0, hi = 0;
            unsigned per = 0;
            if (sscanf(e, "%ld-%ld@%u", &lo, &hi, &per) == 3) {
                s_ps_lo = lo;
                s_ps_hi = hi;
                if (per > 0)
                    s_ps_period = per;
            }
        }
    }

#ifdef NDS_PROF_BUILD
    {
        const char *pf = getenv("NDS_PROF");
        s_prof_on = (pf != NULL && pf[0] != '0' && pf[0] != '\0') ? 1 : 0;
        if (s_prof_on)
            atexit(cpu_prof_report);
    }
#endif
}

int cpu_step(arm_cpu_t *cpu)
{
    /* 21-B9yi(续52)：诊断开关已在 cpu_diag_init()（cpu_create）里解析完，
       热路径只剩一次普通全局变量读取。 */
    /* 21-B9yi(续63)：抽样决策（每 64 条指令测一条，见 s_prof_seq 的注释）。 */
#ifdef NDS_PROF_BUILD
    if (s_prof_on && s_prof_first_tsc == 0)
        s_prof_first_tsc = cpu_rdtsc();
    s_prof_meas = s_prof_on && ((s_prof_seq++ & 63u) == 0u);
#endif
    unsigned long long prof_t0 = PROF_T0();
    /* 21-B9yi(续53)：把热路径反复用到的两个指针/标志提到局部变量。
       原来每步要写 `cpu->nds->bus->...` / `cpu->nds->io->...` 这类三级链式取址
       七八次（每条指令都做），现在只取一次。 */
    bus_t *bus = cpu->nds->bus;
    io_t *io = cpu->nds->io;
    /* 21-B9yi(续108g)：访存代价模型开启时，本步的数据访问代价从这里重新累加。 */
    if (s_memtim == 1 || (s_memtim < 0 && cpu_memtim_enabled()))
        bus_data_cost_reset();
    /* 21-B9yi：上一条指令消耗的周期数（本步用于推进卡带时钟，使其与
       参考核一样按系统时钟节奏取数；见 io_advance_cart 注释） */
    uint32_t prev_cost = cpu->step_cycles;
    cpu->step_cycles = 1;
    /* 8.x：设置当前访问者身份，供 bus 对中断/FIFO 等按 CPU 分流 */
    bus->active_is_arm7 = cpu->is_arm7;
    /* 21-B9xj：写监视用 PC。取值口径与 melonDS 解释器一致（ARM=当前指令+8、
       Thumb=+4），这样本地与参考 harness 打出来的 pc 可直接对照。
       21-B9yi(续41)：四个字段合并到**一次**条件判断里（原先四次读
       `bus->diag` 的链式指针，编译器无法合并）。 */
    /* 21-B9yi(续53)：诊断块只判一次 `bus->diag`（原来是两次独立判断），
       PC 命中计数再在其内部判一次「有没有配置」。 */
    if (bus->diag) {
        bus->dbg_pc = cpu->r[15] + ((cpu->cpsr & CPSR_T) ? 4u : 8u);
        bus->dbg_lr = cpu->r[14];
        bus->dbg_sp = cpu->r[13];
        bus->dbg_cpsr = cpu->cpsr;
      if (g_pchit_n > 0) {
        uint32_t hit_pc = cpu->r[15];
        static unsigned g_pchit_log[16];
        for (int i = 0; i < g_pchit_n; i++) {
            if (g_pchit_addr[i] == hit_pc) {
                g_pchit_cnt[i]++;
                g_pchit_hist[cpu->r[0] & 0xFu]++;
                if (g_pchit_log[i] < 3) {
                    g_pchit_log[i]++;
                    uint32_t sp = cpu->r[13];
                    printf("pchit-hit: %08X arm%d r0=%08X r7=%08X lr=%08X sp=%08X"
                           " st=%08X/%08X/%08X/%08X\n",
                           hit_pc, cpu->is_arm7 ? 7 : 9, cpu->r[0], cpu->r[7],
                           cpu->r[14], sp,
                           bus_read32(bus, sp), bus_read32(bus, sp + 4u),
                           bus_read32(bus, sp + 8u), bus_read32(bus, sp + 12u));
                }
            }
        }
      }
    }
    irq_t *irq = &io->irq[cpu->is_arm7 ? 1 : 0];
    /* 21-B9wt：ARM7 的 HALTCNT 暂停（BIOS SWI 6 Halt / SWI 7 Stop / 游戏
       直接写 0x04000301 都走这里）。唤醒口径与 melonDS HaltInterrupted(1)
       一致：(IF & IE) != 0 即唤醒，不看 IME；唤醒后清请求，让下面的 IRQ
       检查先决定“进 handler”还是“继续执行 BIOS 下一条”。 */
    if (cpu->is_arm7 && power_halt_pending(&io->power)) {
        if (!(irq->ie & irq->ifl)) {
            cpu->step_cycles = 0;
            return 1;
        }
        power_halt_wake(&io->power);
    }
    /* 21-B9yi(续52)：先把 bios_irq_tail9 自己的前置条件写在调用点上，
       绝大多数指令（ARM9 不在 IRQ 模式）因此连函数调用都省了。 */
    if (!cpu->is_arm7 && (cpu->cpsr & CPSR_MODE_MASK) == ARM_MODE_IRQ &&
        cpu->r[15] == 0xFFFF06F0u && bios_irq_tail9(cpu))
        return 1;
    /* 6.5：按本步消耗的周期推进当前核定时器（分频在 timer.c 内处理）。
       21-B9yi：传上一条指令的**实际周期数**（此前固定 1/指令，ARM9 平均 ~1.2，
       定时器会系统性偏慢；melonDS 定时器是挂在系统时钟上的）。 */
    unsigned long long prof_io_t0 = PROF_T0();
    io_advance_timers(io, cpu->is_arm7, prev_cost ? prev_cost : 1u);
    PROF_ADD(s_prof_tmr, prof_io_t0);
    unsigned long long prof_cart_t0 = PROF_T0();
    /* 21-B9yi(续56)：GX/卡带都不忙时整段跳过（门控由 io 模块维护，见 io.h）。 */
    if (io->cart_clock_on)
        io_advance_cart(io, cpu->is_arm7, prev_cost);
    PROF_ADD(s_prof_cart, prof_cart_t0);
    /* 21-B9yi(续53)：IF/IE/IME 在这一步里最多被查 3 次（WFI 唤醒、屏蔽提示、
       受理 IRQ）。这里算一次存起来复用 —— timer/card 的 IF 位在上面两行
       （io_advance_*）之后就已经定下来，所以放在这里取是准确的。 */
    int pend = irq_pending(irq);
    /* 21-B9yi(续12) 诊断：NDS_PCSAMPLE=LO-HI@N → 帧区间内每 N 条 ARM9 指令打印
       一次 PC/lr/cpsr（粗粒度执行轨迹，用来判断「某段等待循环是不是被跳过了」）。
       例：NDS_PCSAMPLE=1903-1905@200 */
    /* 21-B9yi(续52)：PSAMPLE 的解析已挪到 cpu_diag_init()，这里只留一个开关判断。 */
    if (s_ps_on && !cpu->is_arm7) {
        extern unsigned long long g_dbg_frame;
        if ((long)g_dbg_frame >= s_ps_lo && (long)g_dbg_frame <= s_ps_hi) {
            if ((s_ps_seen++ % s_ps_period) == 0)
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
    /* 21-B9yi(续52)：WFI（ARM 编码 0xEE070F90）检测的成本削减。
       旧写法是**每条 ARM9 指令都额外 `cpu_fetch()` 一次内存**去比对 WFI，
       当指令数到十亿量级时这笔开销很可观。现在：
         · 只在 ARM 态判断（Thumb 的 WFI 是 0xBF30，旧写法还可能把两个相邻
           Thumb 指令拼出的字误判成 WFI）；
         · 读到的指令字**缓存下来给下面的 exec 复用**，ARM 态每条指令只读一次指令。 */
    uint32_t pre_insn = 0;
    int pre_insn_valid = 0;
    if (!cpu->is_arm7 && !(cpu->cpsr & CPSR_T)) {
        unsigned long long ft0 = PROF_T0();
        pre_insn = cpu_fetch(cpu);
        PROF_ADD(s_prof_fetch, ft0);
        pre_insn_valid = 1;
        if (pre_insn == 0xEE070F90u) {
            if (!pend) {
                cpu->step_cycles = 0;
                return 1;
            }
            cpu->cpsr &= ~CPSR_I;
            /* 21-B9s：WFI 被中断唤醒时指令先“完成”再进 IRQ——PC 前进到下一条，
               否则 IRQ 返回后又停在 WFI 上重执行，空闲任务永远走不到后续代码。 */
            cpu->r[15] += 4;
            pre_insn_valid = 0;   /* PC 已改变，缓存的指令字作废 */
        }
    }
    /* 21-B9h：IF&IE 已挂起却被 CPSR.I 屏蔽时只提示一次 */
    if (pend && (cpu->cpsr & CPSR_I) && !cpu->irq_mask_logged) {
        cpu->irq_mask_logged = 1;
        if (bus->diag)
            printf("irq: %s IF&IE pending but masked-by-cpsr-I"
                   " cpsr=%08X pc=%08X\n",
                   cpu->is_arm7 ? "arm7" : "arm9", cpu->cpsr, cpu->r[15]);
    }
    if (pend && !(cpu->cpsr & CPSR_I)) {
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
            /* 21-B9yi(续19)：两核都记（NDS_IRQLOG=lo-hi，arm7 行用 locirq7 前缀），
               用来对比「驱动游戏调度节拍的 ARM7 中断率」。 */
            if (irqlog_state == 1 && irqlog_n < 400000 &&
                (long)g_dbg_frame >= irqlog_lo && (long)g_dbg_frame <= irqlog_hi) {
                irqlog_n++;
                printf("%s: f=%llu pc=%08X cpsr=%08X lr=%08X if=%08X ie=%08X t=%llu\n",
                       cpu->is_arm7 ? "locirq7" : "locirq",
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
    /* 21-B9yi(续65)：定向实验 —— 预取下一批指令所在的缓存行。
       依据：续64 的结论是「瓶颈在访存（缓存未命中延迟），不在算术」。
       预取只在两种最常见代码区（ARM9 ITCM / Main RAM）做，其它区域直接跳过。 */
    {
        uint32_t npc = cpu->next_fetch_pc;
        if (!cpu->is_arm7 && npc - BUS_ARM9_ITCM_BASE < BUS_ARM9_ITCM_SIZE)
            __builtin_prefetch(&bus->arm9_itcm[npc - BUS_ARM9_ITCM_BASE]);
        else if (npc >= BUS_MAIN_RAM_BASE &&
                 npc - BUS_MAIN_RAM_BASE < BUS_MAIN_RAM_SIZE)
            __builtin_prefetch(&bus->main_ram[npc - BUS_MAIN_RAM_BASE]);
    }
    if (is_thumb) {
        unsigned long long ft0 = PROF_T0();
        uint16_t insn16 = cpu_fetch16(cpu);
        PROF_ADD(s_prof_fetch, ft0);
        cpu->cycles++;
        unsigned long long xt0 = PROF_T0();
        int r = thumb_step(cpu, insn16);
        PROF_ADD(s_prof_exec, xt0);
        /* 21-B9yh：本步取指代价（见 cpu_fetch_cost；step_cycles=0 表示在等待） */
        uint32_t fc = cpu_fetch_cost(cpu, ipc, fetch_nonseq);
        if (cpu_memtim_enabled() && !cpu->is_arm7) {
            /* 21-B9yi(续108g)：取指 + 数据（melonDS 的 AddCycles_CDI 口径：
               取指与数据部分流水重叠 ⇒ max(code, data, code+data-6)） */
            uint32_t dc = bus_data_cost_take();
            uint32_t total = (fc > dc) ? fc : dc;
            if (fc + dc > 6u && fc + dc - 6u > total)
                total = fc + dc - 6u;
            if (cpu->step_cycles != 0)
                cpu->step_cycles = total;
        } else if (cpu->step_cycles != 0 && cpu->step_cycles < fc) {
            cpu->step_cycles = fc;
        }
        PROF_STEP_END(prof_t0);
        return r;
    }
    /* 21-B9yi(续52)：ARM 态复用上面 WFI 检测时读到的指令字（只读一次指令）。 */
    uint32_t insn = pre_insn_valid ? pre_insn : cpu_fetch(cpu);
    cpu->cycles++;
    unsigned long long xt0 = PROF_T0();
    int r = exec_step(cpu, insn);
    PROF_ADD(s_prof_exec, xt0);
    {
        uint32_t fc = cpu_fetch_cost(cpu, ipc, fetch_nonseq);
        if (cpu_memtim_enabled() && !cpu->is_arm7) {
            uint32_t dc = bus_data_cost_take();
            uint32_t total = (fc > dc) ? fc : dc;
            if (fc + dc > 6u && fc + dc - 6u > total)
                total = fc + dc - 6u;
            if (cpu->step_cycles != 0)
                cpu->step_cycles = total;
        } else if (cpu->step_cycles != 0 && cpu->step_cycles < fc) {
            cpu->step_cycles = fc;
        }
    }
    PROF_STEP_END(prof_t0);
    return r;
}
