#include "dma.h"
#include "bus/bus.h"
#include "io/io.h"
#include <stdio.h>
#include <stdlib.h>

/* 通道内字节偏移（相对本通道基址）：0-3=SAD、4-7=DAD、8-9=CNT_L、10-11=CNT_H */
static uint32_t ch_off(uint32_t addr)
{
    return (addr - IO_DMA0_BASE) % IO_DMA_STRIDE;
}

/* 按地址选通道（0..3） */
static uint32_t ch_index(uint32_t addr)
{
    return (addr - IO_DMA0_BASE) / IO_DMA_STRIDE;
}

int dma_is_addr(uint32_t addr)
{
    return addr >= IO_DMA0_BASE && addr < IO_DMA_END;
}

uint8_t dma_read8(const dma_t *dma, uint32_t addr)
{
    const dma_channel_t *d = &dma->ch[ch_index(addr)];
    uint32_t off = ch_off(addr);
    if (off < 4)      return (uint8_t)(d->sad >> (off * 8));
    if (off < 8)      return (uint8_t)(d->dad >> ((off - 4) * 8));
    if (off < 10)     return (uint8_t)(d->cnt_l >> ((off - 8) * 8));
    return (uint8_t)(d->cnt_h >> ((off - 10) * 8));
}

/* 地址推进：按地址控制位增/减/固定（3=增/重载，repeat 触发时靠外部重置回 sad/dad）。 */
static uint32_t dma_advance(uint32_t a, int mode, uint32_t step)
{
    switch (mode) {
    case 1:  return a - step;  /* 递减 */
    case 2:  return a;         /* 固定 */
    default: return a + step;  /* 0=增 / 3=增/重载 */
    }
}

/* DMA 完成中断（21-B9p）：搬完且 CNT bit14(IRQ) 置位时，把当前核 IF 的
   bit8+通道 置 1（真机 DMA0-3 完成中断 = IF bit8-11）。 */
static void dma_irq_done(const dma_channel_t *dma, struct bus *bus, int ch,
                         int is_arm7)
{
    if (bus == NULL || bus->io == NULL)
        return;
    if ((dma->cnt_h & DMA_CNT_IRQ) == 0)
        return;
    /* 21-B9yi(续20) 诊断：NDS_DMAIRQ=LO-HI → 每次「DMA 完成中断」上挂时打印
       通道/控制字/发起 PC。用于对齐「参考核从不上挂 IF bit11（DMA3），本地约
       1 次/帧」这一差异。 */
    {
        extern unsigned long long g_dbg_frame;
        static int state;
        static long lo = -2, hi = -2;
        if (state == 0) {
            const char *e = getenv("NDS_DMAIRQ");
            state = (e != NULL && e[0] != '0' && e[0] != '\0') ? 1 : -1;
            lo = 0; hi = -1;
            if (state == 1 && e != NULL) {
                long a = 0, b = 0;
                if (sscanf(e, "%ld-%ld", &a, &b) == 2) { lo = a; hi = b; }
            }
        }
        if (state == 1 && (long)g_dbg_frame >= lo && (long)g_dbg_frame <= hi)
            printf("dmairq: f=%llu arm%d ch=%d cnt_h=%04X cnt_l=%u sad=%08X dad=%08X pc=%08X\n",
                   g_dbg_frame, is_arm7 ? 7 : 9, ch, dma->cnt_h, dma->cnt_l,
                   dma->sad, dma->dad,
                   bus->dbg_pc);
    }
    bus->io->irq[is_arm7 ? 1 : 0].ifl |= (uint32_t)(1u << (8 + ch));
}

/* 执行一次拷贝：把 N 个字/半字从源搬到目的。
   源/目的地址控制按增/减/固定处理；搬运走 bus_read/write，源可落在卡带 CARD_DATA。
   非重复搬运搬完自动清使能（真机同款行为）；重复模式保持使能，每次触发都重搬同一块。 */
/* 21-B9yi：诊断用——DMA 搬运期间置 1，供总线侧区分「CPU 读 / DMA 读」。 */
int g_dma_active;

static void dma_transfer(dma_channel_t *dma, struct bus *bus, int ch, int is_arm7)
{
    uint32_t n = dma->cnt_l != 0 ? dma->cnt_l : 0x4000u; /* 0 按 GBA/NDS 惯例=0x4000 */
    int is32 = (dma->cnt_h & DMA_CNT_32BIT) != 0;
    uint32_t step = is32 ? 4u : 2u;
    uint32_t src = dma->sad;
    uint32_t dst = dma->dad;
    int src_mode = (dma->cnt_h >> 7) & 3u; /* CNT bit23-24 */
    int dst_mode = (dma->cnt_h >> 5) & 3u; /* CNT bit21-22 */

    /* 21-B9xq 诊断：前 40 次 DMA 搬运的通道/属主/源/目的/长度/单位（bring-up 定位用） */
    {
        static int dma_log = 0;
        if (dma_log < 40) {
            printf("dma: arm%d ch=%d sad=%08X dad=%08X n=%u unit=%d"
                   " srcmode=%d dstmode=%d cnt=%04X\n",
                   is_arm7 ? 7 : 9, ch, dma->sad, dma->dad, n, is32 ? 32 : 16,
                   src_mode, dst_mode, dma->cnt_h);
            dma_log++;
        }
    }

    /* 21-B9wu：搬运期间把「当前访问者」切到 DMA 属主核。总线按 active_is_arm7
       分流 IO：0x04000400 在 ARM9 视角是 GX 命令 FIFO、ARM7 视角是音频寄存器；
       IME/IE/IF 也是两套。不切换的话 ARM9 的显示列表 DMA 会被当成 ARM7 的音频写，
       GX 收不到几何命令（3D 画面全黑），DMA 完成中断也会挂到错的核心。 */
    int prev_arm7 = bus->active_is_arm7;
    bus->active_is_arm7 = is_arm7;
    int prev_dma = g_dma_active;
    g_dma_active = 1;
    /* 21-B9yi(续22)：**模式 7（GX FIFO）按 FIFO 空位分批**。
       melonDS 的 `DMA::Run9` 在 GX-FIFO 模式下受 `CmdFIFO`（112 项）容量限制，
       满了就 `Stall`、等 `GPU3D::CheckFIFODMA()` 再继续，因此这种 DMA 常常
       跨很多周期才把 RemCount 走完；本地此前一次搬完 ⇒ 立刻满足 RemCount==0
       ⇒ 立刻挂 IF bit11（实测多出 ~0.7 次/帧，参考核一次都没有）。
       这里：能搬多少搬多少，搬不完就记在 `dma->rem` 上、保持使能并由
       `dma_gx_resume()` 续跑；只有全部搬完才走原来的收尾（清使能/挂中断）。 */
    int is_gx = (!is_arm7
                 && (((dma->cnt_h & DMA_CNT_MODE_MASK) >> DMA_CNT_MODE_SHIFT)
                     == DMA_START_GXFIFO));
    uint32_t to_do = n;
    if (is_gx && bus != NULL && bus->io != NULL) {
        /* 21-B9yi(续27)：GX-FIFO 的「满」按**条目**算（melonDS CmdPIPE 112 条）；
           参数属于已有条目。每次只推一个字，由 `dma_gx_resume()` 续推。 */
        if (!gx_fifo_can_accept(&bus->io->gx))
            to_do = 0;
        else if (to_do > 1u)
            to_do = 1u;
    }
    if (dma->rem == 0)
        dma->rem = n;
    if (to_do > dma->rem)
        to_do = dma->rem;
    for (uint32_t i = 0; i < to_do; i++) {
        /* 21-B9yi(续12)：**去掉 21-B9zb 的「瞬间推进卡带时钟」hack**。
           真机/melonDS 的卡带 DMA 不推进卡带时钟：数据由卡带侧按自己的节拍
           取进 2 字 FIFO 并拉 DRQ（`ROMReceiveData`），DMA 只是把 FIFO 里
           已经就绪的字搬走。本地旧实现每次 DMA 读都 `cartbus_advance(…,100000)`
           把等待一次性跳过，于是同一笔传输在本地只用参考核 1/4 的帧数就跑完
           （实测 205 块/帧 vs 参考 50 块/帧），游戏侧「哪一块该由谁搬」的
           计数随之错位。数据是否就绪由 `dma_fire_card` 的 DRQ 门控保证。 */
        if (is32)
            bus_write32(bus, dst, bus_read32(bus, src));
        else
            bus_write16(bus, dst, bus_read16(bus, src));
        src = dma_advance(src, src_mode, step);
        dst = dma_advance(dst, dst_mode, step);
    }
    dma->rem -= to_do;

    bus->active_is_arm7 = prev_arm7;
    g_dma_active = prev_dma;
    if (dma->rem != 0) {
        /* 未搬完（FIFO 满）：地址照常写回、保持使能、**不**挂完成中断。 */
        dma->sad = src;
        dma->dad = dst;
        return;
    }
    /* 21-B9yi：把推进后的地址写回通道——重复模式（CNT bit25）下卡带 DMA 会
       反复触发，**地址必须跨轮次保持前进**，否则每个字都写到同一个地址
       （实测：游戏用 `AF000001`（模式 5 + 重复 + 源固定/目的递增）逐字搬 512B，
       本地因地址不前进把整块写到了同一个 dword，游戏数据对不上后走进死循环）。 */
    dma->sad = src;
    dma->dad = dst;
    if ((dma->cnt_h & DMA_CNT_REPEAT) == 0)
        dma->cnt_h &= (uint16_t)~DMA_CNT_ENABLE;
    dma_irq_done(dma, bus, ch, is_arm7);
}

void dma_write8(dma_t *dma, uint32_t addr, uint8_t val, struct bus *bus,
                int is_arm7)
{
    dma_channel_t *d = &dma->ch[ch_index(addr)];
    uint32_t off = ch_off(addr);
    if (off < 4) {
        uint32_t shift = off * 8;
        d->sad = (d->sad & ~(0xFFu << shift)) | ((uint32_t)val << shift);
    } else if (off < 8) {
        uint32_t shift = (off - 4) * 8;
        d->dad = (d->dad & ~(0xFFu << shift)) | ((uint32_t)val << shift);
    } else if (off < 10) {
        uint32_t shift = (off - 8) * 8;
        d->cnt_l = (uint16_t)((d->cnt_l & ~(0xFFu << shift)) | ((uint32_t)val << shift));
    } else {
        uint32_t shift = (off - 10) * 8;
        d->cnt_h = (uint16_t)((d->cnt_h & ~(0xFFu << shift)) | ((uint32_t)val << shift));
        /* 触发条件：写 CNT_H 高字节（含使能位）且使能位置位；
           模式 0=立即、模式 7=GX FIFO（仅 ARM9）都在这里同步搬运。 */
        if (off == 11 && (d->cnt_h & DMA_CNT_ENABLE) != 0) {
            unsigned mode = ((d->cnt_h & DMA_CNT_MODE_MASK) >> DMA_CNT_MODE_SHIFT);
            if (mode == DMA_START_IMMED ||
                (!is_arm7 && mode == DMA_START_GXFIFO))
                dma_transfer(d, bus, (int)ch_index(addr), is_arm7);
            /* 21-B9yi(续111)：模式 7 武装后可能被 FIFO 空位卡住 ⇒ 置「等 GX」标志，
               让 `dma_gx_resume()` 在时钟推进时被调用（否则它会立刻返回）。 */
            if (!is_arm7 && mode == DMA_START_GXFIFO)
                dma->gx_waiting = 1;
        }
    }
}

void dma_fire(dma_t *dma, struct bus *bus, int start_mode, int is_arm7)
{
    for (int c = 0; c < IO_DMA_COUNT; c++) {
        dma_channel_t *d = &dma->ch[c];
        if ((d->cnt_h & DMA_CNT_ENABLE) == 0)
            continue;
        if (((d->cnt_h & DMA_CNT_MODE_MASK) >> DMA_CNT_MODE_SHIFT) != start_mode)
            continue;
        dma_transfer(d, bus, c, is_arm7);
    }
}

/* 21-B9ws：卡带 DMA 触发。ARM9 用 3 位 start mode（5=DS cart）；
   ARM7 的 DS cart 模式是 CNT 高半字 bits13-12 | 0x10（melonDS: 0x12）。 */
void dma_fire_card(dma_t *dma, struct bus *bus, int is_arm7)
{
    /* 21-B9yi：melonDS 的 `NDSCartSlot::Interface::CheckDMA()` 第一行就是
       `if (!(ROMCnt & (1<<23))) return;` ——DRQ 未置位时不触发卡带 DMA。
       本地旧实现无条件触发，于是「武装了卡带 DMA 但数据还没就绪」时会读到
       FIFO 里的残留值（实测游戏缓冲被写进 0xFFFFFFFF，随后逻辑走飞）。 */
    if (bus == NULL || bus->io == NULL)
        return;
    /* 21-B9yi(续12) 诊断：NDS_DMAFBG=LO-HI → 每次卡带 DMA 触发点打印
       （DRQ、每个通道的使能/模式、最终是否真的搬了），用于定位
       「武装了但没人搬 / 触发了但 FIFO 是空」这类停摆。 */
    static int fbg_state;
    static long fbg_lo = -2, fbg_hi = -2;
    if (fbg_state == 0) {
        extern unsigned long long g_dbg_frame;
        const char *e = getenv("NDS_DMAFBG");
        fbg_state = (e != NULL && e[0] != '0' && e[0] != '\0') ? 1 : -1;
        fbg_lo = 0; fbg_hi = -1;
        if (fbg_state == 1 && e != NULL) {
            long lo = 0, hi = 0;
            if (sscanf(e, "%ld-%ld", &lo, &hi) == 2) { fbg_lo = lo; fbg_hi = hi; }
        }
    }
    int fbg_on = 0;
    if (fbg_state == 1) {
        extern unsigned long long g_dbg_frame;
        fbg_on = ((long)g_dbg_frame >= fbg_lo && (long)g_dbg_frame <= fbg_hi);
    }
    if (fbg_on) {
        extern unsigned long long g_dbg_frame;
        printf("dmafbg: f=%llu arm%d drq=%d cnt3=%04X/%04X cnt2=%04X/%04X\n",
               g_dbg_frame, is_arm7 ? 7 : 9,
               (bus->io->cartbus.romctrl & CART_ROMCTRL_DRQ) ? 1 : 0,
               dma->ch[3].cnt_h, dma->ch[3].cnt_l,
               dma->ch[2].cnt_h, dma->ch[2].cnt_l);
    }
    /* 21-B9yi(续12)：**电平敏感的搬完再检查**。melonDS 的 `DMA::Run9()` 收尾是
       `if (StartMode == 0x05) NDSCartSlots[0]->CheckDMA();`，而 `CheckDMA()`
       第一行就是「DRQ 未置位直接返回」——于是「FIFO 里还有字」这件事会立刻
       把同一通道再拉起来搬下一个字，直到 FIFO 被读空、DRQ 落下为止。
       本地旧实现只在「卡带又取到一个字」的边沿触发一次，FIFO 装满 2 字后
       卡带不再取数（`data_late`）、DMA 也不再被拉起 ⇒ 最后 1 个字没人读、
       传输永远不结束（bit31 不清、完成中断不来）。这里按参考核的语义补上
       循环重检；每轮至少搬 1 个字，FIFO 最多 2 字 ⇒ 循环有界。 */
    for (int guard = 0; guard < 8; guard++) {
        int fired = 0;
        if ((bus->io->cartbus.romctrl & CART_ROMCTRL_DRQ) == 0)
            break;
        for (int c = 0; c < IO_DMA_COUNT; c++) {
            dma_channel_t *d = &dma->ch[c];
            if ((d->cnt_h & DMA_CNT_ENABLE) == 0)
                continue;
            unsigned mode = is_arm7
                ? ((((unsigned)d->cnt_h >> 12) & 3u) | 0x10u)
                : (((unsigned)d->cnt_h & DMA_CNT_MODE_MASK) >> DMA_CNT_MODE_SHIFT);
            if ((is_arm7 && mode == DMA_START_CARD7) ||
                (!is_arm7 && mode == DMA_START_CARD)) {
                dma_transfer(d, bus, c, is_arm7);
                fired = 1;
            }
        }
        if (!fired)
            break;
    }
}

/* 21-B9yi(续22)：GX（模式 7）DMA 的续跑。
   被 FIFO 空位卡住的通道保持 enable=1、rem>0；3D 引擎消费掉一些 FIFO 字后
   （`gx_advance()`）由 io 层调用本函数继续搬。对齐 melonDS
   `GPU3D::CheckFIFODMA()` 的角色。 */
void dma_gx_resume(dma_t *dma, struct bus *bus, int is_arm7)
{
    if (dma == NULL || bus == NULL || is_arm7)
        return;
    /* 21-B9yi(续111)：快速门控 —— 没有任何通道在等 GX FIFO 时直接返回。
       战斗场景每步都会走到这里（GX 时钟常开），省下的是「扫 8 个通道」的固定开销。 */
    if (!dma->gx_waiting)
        return;
    int any = 0;
    for (int c = 0; c < IO_DMA_COUNT; c++) {
        dma_channel_t *d = &dma->ch[c];
        if (d->rem == 0)
            continue;
        if ((d->cnt_h & DMA_CNT_ENABLE) == 0) {
            d->rem = 0;      /* 被软件撤下武装：丢弃未完成的搬运 */
            continue;
        }
        unsigned mode = (d->cnt_h & DMA_CNT_MODE_MASK) >> DMA_CNT_MODE_SHIFT;
        if (mode != DMA_START_GXFIFO)
            continue;
        any = 1;              /* 该通道仍在等 GX FIFO（无论本轮是否搬得动） */
        if (!gx_fifo_can_accept(&bus->io->gx))
            continue;
        dma_transfer(d, bus, c, is_arm7);
    }
    dma->gx_waiting = (uint8_t)any;
}
