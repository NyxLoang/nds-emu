#include <stdio.h>
#include <stdlib.h>
#include "io.h"
#include "bus/bus.h"

static void io_gx_fifo_irq_sync(io_t *io);
/* 21-B9wx：RTC/RCnt 读计数（诊断：游戏是否在轮询实时时钟） */
unsigned long long g_rtc_reads = 0;
/* 21-B9xa：ARM7 写声音通道 CNT 的次数（诊断：声音驱动是否在跑） */
unsigned long long g_snd_cnt_writes = 0;
/* 21-B9xi 诊断：ARM7 写 SOUNDBIAS(0x04000504/05) 的次数与最后拼出的值
   （参考核帧 1500 起 SOUNDBIAS=0x200；本地若为 0 说明该处代码没跑到） */
unsigned long long g_snd_bias_writes = 0;
unsigned int g_snd_bias_last = 0;
/* 21-B9xi 诊断：IPC 发送计数与前 32 条报文（与参考核 fifo7/fifo9 对照；
   参考核每帧 = C0240046/C02400C6/C0240006/C0240086 + 0000C187） */
unsigned long long g_ipc_sends[2] = {0, 0};
unsigned long long g_ipc_c187 = 0;          /* 参考核每帧 1 条的 0000C187 计数 */
uint32_t g_ipc_trace[32][3];                /* {core, val, frame} 环形，最近 32 条 */
unsigned g_ipc_trace_n = 0;                 /* 累计发送数 */
unsigned long long g_dbg_frame = 0;         /* 由 runner 每帧更新（诊断用） */
/* 21-B9xj 诊断：VBlank / 扫描线事件计数（每帧应当恰好 1 次 VBlank、263 次扫描线） */
unsigned long long g_vblank_events = 0;
unsigned long long g_scanline_events = 0;
uint32_t g_ipc9_trace[16][2];               /* {val, frame} 最近 16 条 ARM9 发送 */
unsigned g_ipc9_n = 0;

io_t *io_create(void)
{
    io_t *io = calloc(1, sizeof(io_t));
    if (io != NULL) {
        memctl_reset(&io->memctl); /* 直接启动初值：WRAMCNT=3，EXMEMCNT=0xE880 */
        power_reset(&io->power);   /* 直接启动初值：LCD/2D/3D/喇叭全开 */
        cartbus_init(&io->cartbus);
        gx_reset(&io->gx); /* 矩阵置单位阵 + 视口默认 + GXSTAT 置 FIFO 空 */
        key_reset(&io->keypad); /* 21-B9wu：默认所有键松开（0 会被读成全按下） */
        rtc_reset(&io->rtc);   /* 21-B9wx：RTC 初始为 2026-09-12 12:00:00（24 小时制） */
        snd_reset(&io->snd);    /* 21-B9xi：SOUNDBIAS=0x200（真机上电值） */
    }
    return io;
}

void io_destroy(io_t *io)
{
    if (io != NULL)
        cartbus_destroy(&io->cartbus);
    free(io);
}

/* 读 FIFO CNT 的某个字节（16 位寄存器，0=低字节） */
static uint8_t fifo_cnt_read8(ipc_fifo_t *f, uint32_t addr, int is_arm7)
{
    uint16_t cnt = fifo_cnt_read(f, is_arm7);
    if (addr == IO_FIFO_CNT)
        return (uint8_t)(cnt & 0xFFu);
    return (uint8_t)(cnt >> 8);
}

/* 写 FIFO CNT 的某个字节（16 位寄存器） */
static void fifo_cnt_write8(ipc_fifo_t *f, uint32_t addr, uint8_t val, int is_arm7)
{
    uint16_t cnt = fifo_cnt_read(f, is_arm7);
    if (addr == IO_FIFO_CNT)
        cnt = (uint16_t)((cnt & 0xFF00u) | val);
    else
        cnt = (uint16_t)((cnt & 0x00FFu) | ((uint16_t)val << 8));
    fifo_cnt_write(f, is_arm7, cnt);
}

/* FIFO 状态变化会同时影响两核的 IF（一方发送 → 另一方接收非空），
   故任何 FIFO 操作后对两核都做一次中断边沿检测。 */
static void io_fifo_update_irq_all(io_t *io)
{
    fifo_update_irq(&io->fifo, 0, &io->irq[0]); /* ARM9 视角 */
    fifo_update_irq(&io->fifo, 1, &io->irq[1]); /* ARM7 视角 */
}

/* 卡带就绪时触发指定核等待卡带数据的 DMA（start mode = card）。
   幂等：dma_fire 只搬已使能且模式匹配的通道，搬完自动清使能。 */
static void io_card_dma_check(io_t *io, int is_arm7)
{
    if (cartbus_ready(&io->cartbus))
        dma_fire_card(&io->dma[is_arm7 ? 1 : 0], io->bus, is_arm7);
}

/* 诊断：记录「首次访问的未知 IO 地址」，避免游戏轮询同一寄存器（如 VCOUNT）刷屏。 */
static uint32_t io_seen[256];
static int io_seen_count = 0;
static int io_addr_first_seen(uint32_t addr)
{
    for (int i = 0; i < io_seen_count; i++)
        if (io_seen[i] == addr)
            return 0;
    if (io_seen_count < (int)(sizeof(io_seen) / sizeof(io_seen[0])))
        io_seen[io_seen_count++] = addr;
    return 1;
}

/* 按地址分发到对应功能文件。未实现的寄存器地址：读 0、写忽略。 */
uint8_t io_read8(const io_t *io, uint32_t addr, int is_arm7)
{
    if (irq_is_addr(addr))
        return irq_read8(&io->irq[is_arm7 ? 1 : 0], addr);
    if (ipc_sync_is_addr(addr))
        return ipc_sync_read8(&io->sync, addr, is_arm7);
    if (!is_arm7 && addr >= BUS_VRAM_CNT_BASE &&
        addr < BUS_VRAM_CNT_BASE + 10u &&
        addr != 0x04000247u && io->bus != NULL) {
        /* VRAMCNT：0x240-246=A..G，0x248-249=H..I（0x247 是 WRAMCNT） */
        unsigned idx = (unsigned)(addr - BUS_VRAM_CNT_BASE);
        if (idx > 6)
            idx--;
        return io->bus->vramcnt[idx];
    }
    if (memctl_is_addr(addr))
        return memctl_read8(&io->memctl, addr, is_arm7);
    /* 21-B9xi：0x04000308/09（BIOS 保护值）只对 ARM7 暴露；NDS9 侧保持未映射（读 0）。 */
    if (power_is_addr(addr) &&
        (is_arm7 || (addr >= IO_POWER_POSTFLG && addr < IO_POWER_END)))
        return power_read8(&io->power, addr, is_arm7);
    if (math_is_addr(addr) && !is_arm7)
        return math_read8(&io->math, addr);
    if (fifo_is_cnt_addr(addr))
        return fifo_cnt_read8((ipc_fifo_t *)&io->fifo, addr, is_arm7);
    if (addr >= IO_TIMER0_BASE && addr < IO_TIMER_END)
        return timer_read8(&io->timer[is_arm7 ? 1 : 0]
                           [(addr - IO_TIMER0_BASE) / IO_TIMER_STRIDE], addr);
    if (addr >= IO_KEYINPUT_ADDR && addr < IO_KEYINPUT_END)
        return key_read8(&io->keypad, addr);
    if (addr >= 0x04000134u && addr < 0x04000140u && is_arm7) {
        g_rtc_reads++;
    }
    if (addr >= IO_KEYCNT_ADDR && addr < IO_KEYCNT_END) {
        uint16_t cnt = io->keycnt[is_arm7 ? 1 : 0];
        return (uint8_t)(cnt >> ((addr - IO_KEYCNT_ADDR) * 8));
    }
    /* 21-B9wx：ARM7 的 RCnt(0x04000134)/键高半字(0x04000136)/RTC(0x04000138) */
    if (is_arm7 && addr >= 0x04000134u && addr < 0x04000140u) {
        switch (addr) {
        case 0x04000134u: return (uint8_t)(io->rcnt & 0xFFu);
        case 0x04000135u: return (uint8_t)(io->rcnt >> 8);
        case 0x04000136u: return 0x7Fu;   /* X/Y 等高位按键：1=松开 */
        case 0x04000137u: return 0x00u;
        case 0x04000138u: return (uint8_t)(rtc_read16(&io->rtc) & 0xFFu);
        case 0x04000139u: return (uint8_t)(rtc_read16(&io->rtc) >> 8);
        default:          return 0x00u;
        }
    }
    if (dma_is_addr(addr))
        return dma_read8(&io->dma[is_arm7 ? 1 : 0], addr);
    if (cartbus_is_addr(addr))
        return cartbus_read8((cartbus_t *)&io->cartbus, addr);
    if (disp_is_addr(addr))
        return disp_read8(&io->disp, addr, is_arm7);
    if (touch_is_addr(addr))
        return touch_read8((touch_t *)&io->touch, addr);
    if (gx_is_addr(addr) && !is_arm7)
        return gx_read8(&io->gx, addr);
    if (snd_is_addr(addr) && is_arm7)
        return snd_read8(&io->snd, addr);
    /* VCOUNT（0x04000006/07 只读）：FFXII ARM7 用它做任务调度截止点；
       未实现时读 0 会让所有任务截止点相同、就绪队列排序错误。 */
    if (addr == 0x04000006u)
        return (uint8_t)(io->vcount & 0xFFu);
    if (addr == 0x04000007u)
        return (uint8_t)(io->vcount >> 8);
    /* 21-B9xy：melonDS 对 0x04000320 硬编码返回 46（GPU3D::Read16/32，TODO）；
       该区间归 GPU3D、只有 ARM9 看得到。本地此前落到未映射返回 0。 */
    if (!is_arm7 && addr >= 0x04000320u && addr < 0x04000324u)
        return (addr == 0x04000320u) ? 0x2Eu : 0x00u;
    if (io->bus != NULL && io->bus->diag && io_addr_first_seen(addr))
        printf("io: read  unknown addr=%08X (arm7=%d)\n", addr, is_arm7);
    return 0;
}

void io_write8(io_t *io, uint32_t addr, uint8_t val, int is_arm7)
{
    if (irq_is_addr(addr)) {
        irq_write8(&io->irq[is_arm7 ? 1 : 0], addr, val);
        return;
    }
    if (ipc_sync_is_addr(addr)) {
        /* 请求目标是对端核：ARM9 写请求 → ARM7 的 IF，ARM7 写请求 → ARM9 的 IF */
        ipc_sync_write8(&io->sync, addr, val, is_arm7,
                        &io->irq[is_arm7 ? 0 : 1]);
        return;
    }
    if (!is_arm7 && addr >= BUS_VRAM_CNT_BASE &&
        addr < BUS_VRAM_CNT_BASE + 10u &&
        addr != 0x04000247u && io->bus != NULL) {
        unsigned idx = (unsigned)(addr - BUS_VRAM_CNT_BASE);
        if (idx > 6)
            idx--;
        bus_set_vramcnt(io->bus, (int)idx, val);
        return;
    }
    if (memctl_is_addr(addr)) {
        memctl_write8(&io->memctl, addr, val, is_arm7);
        return;
    }
    if (power_is_addr(addr) &&
        (is_arm7 || (addr >= IO_POWER_POSTFLG && addr < IO_POWER_END))) {
        power_write8(&io->power, addr, val, is_arm7);
        return;
    }
    if (math_is_addr(addr) && !is_arm7) {
        math_write8(&io->math, addr, val);
        return;
    }
    if (fifo_is_cnt_addr(addr)) {
        fifo_cnt_write8(&io->fifo, addr, val, is_arm7);
        io_fifo_update_irq_all(io);
        return;
    }
    if (fifo_is_send_addr(addr)) {
        /* SEND 是 32 位寄存器，字节写语义未定义，忽略（32 位写走 io_send32） */
        return;
    }
    if (addr >= IO_TIMER0_BASE && addr < IO_TIMER_END) {
        timer_write8(&io->timer[is_arm7 ? 1 : 0]
                     [(addr - IO_TIMER0_BASE) / IO_TIMER_STRIDE], addr, val);
        return;
    }
    if (addr >= IO_KEYINPUT_ADDR && addr < IO_KEYINPUT_END) {
        key_write8(&io->keypad, addr, val);
        return;
    }
    if (addr >= IO_KEYCNT_ADDR && addr < IO_KEYCNT_END) {
        uint16_t *cnt = &io->keycnt[is_arm7 ? 1 : 0];
        unsigned shift = (addr - IO_KEYCNT_ADDR) * 8;
        *cnt = (uint16_t)((*cnt & ~(0xFFu << shift)) | ((uint32_t)val << shift));
        return;
    }
    /* 21-B9xa 诊断：ARM7 是否在写声音通道的控制字（声音驱动是否在跑） */
    if (is_arm7 && addr >= SND_BASE && addr < SND_END &&
        ((addr - SND_BASE) & 0x0Fu) == 3u) {
        g_snd_cnt_writes++;
    }
    /* 21-B9wx：RTC 串行接口按字节写（0x04000138 bit0/1/2/4 是数据/时钟/片选/方向） */
    if (is_arm7 && addr >= 0x04000134u && addr < 0x04000140u) {
        switch (addr) {
        case 0x04000134u: io->rcnt = (uint16_t)((io->rcnt & 0xFF00u) | val); return;
        case 0x04000135u: io->rcnt = (uint16_t)((io->rcnt & 0x00FFu) | ((uint16_t)val << 8)); return;
        case 0x04000138u: rtc_write16(&io->rtc, val, 1); return;
        case 0x04000139u: rtc_write16(&io->rtc, (uint16_t)((uint16_t)val << 8), 1); return;
        default: return;   /* 0x04000136/37/3A-3F：只读或无实现 */
        }
    }
    if (dma_is_addr(addr)) {
        /* 写 CNT_H 且使能=1 时在 dma_write8 内同步触发立即搬运；
           若卡带已就绪且本条是卡带触发源，则在写完后补触发。 */
        dma_write8(&io->dma[is_arm7 ? 1 : 0], addr, val, io->bus, is_arm7);
        io_card_dma_check(io, is_arm7);
        return;
    }
    if (cartbus_is_addr(addr)) {
        int was_ready = cartbus_ready(&io->cartbus);
        cartbus_write8(&io->cartbus, addr, val);
        /* 命令刚被激活（卡带由 Busy 变 Ready）：置卡带完成中断并触发卡带 DMA */
        if (!was_ready && cartbus_ready(&io->cartbus)) {
            irq_set_card(&io->irq[0]);
            irq_set_card(&io->irq[1]);
            io_card_dma_check(io, 0);
            io_card_dma_check(io, 1);
        }
        return;
    }
    if (disp_is_addr(addr)) {
        disp_write8(&io->disp, addr, val, is_arm7);
        return;
    }
    if (touch_is_addr(addr)) {
        touch_write8(&io->touch, addr, val);
        return;
    }
    if (gx_is_addr(addr) && !is_arm7) {
        gx_write8(&io->gx, addr, val);
        io_gx_fifo_irq_sync(io);
        return;
    }
    if (snd_is_addr(addr) && is_arm7) {
        if (addr == SND_SOUNDBIAS || addr == SND_SOUNDBIAS + 1u) {
            g_snd_bias_writes++;
            if (addr == SND_SOUNDBIAS)
                g_snd_bias_last = (g_snd_bias_last & 0x0300u) | val;
            else
                g_snd_bias_last = (g_snd_bias_last & 0x00FFu)
                                | ((unsigned)(val & 0x03u) << 8);
        }
        snd_write8(&io->snd, addr, val);
        return;
    }
    /* 其余 IO 地址：写忽略（沿用阶段 2 的桩语义） */
    if (io->bus != NULL && io->bus->diag && io_addr_first_seen(addr))
        printf("io: write unknown addr=%08X val=%02X (arm7=%d)\n", addr, val, is_arm7);
}

/* FIFO 32 位收发（bus 对 0x04000188 写 / 0x04100000 读整体转发） */
uint32_t io_recv32(io_t *io, int is_arm7)
{
    uint32_t v = fifo_recv(&io->fifo, is_arm7);
    io_fifo_update_irq_all(io);
    return v;
}

void io_send32(io_t *io, int is_arm7, uint32_t val)
{
    g_ipc_sends[is_arm7 ? 1 : 0]++;
    if (val == 0x0000C187u)
        g_ipc_c187++;
    g_ipc_trace[g_ipc_trace_n % 32u][0] = is_arm7 ? 7u : 9u;
    g_ipc_trace[g_ipc_trace_n % 32u][1] = val;
    g_ipc_trace[g_ipc_trace_n % 32u][2] = (uint32_t)g_dbg_frame;
    g_ipc_trace_n++;
    if (!is_arm7) {
        g_ipc9_trace[g_ipc9_n % 16u][0] = val;
        g_ipc9_trace[g_ipc9_n % 16u][1] = (uint32_t)g_dbg_frame;
        g_ipc9_n++;
    }
    fifo_send(&io->fifo, is_arm7, val);
    io_fifo_update_irq_all(io);
}

/* GXSTAT bits30-31 配置后，FIFO 空/低水位要反映到 ARM9 IF bit21（melonDS
   GPU3D::CheckFIFOIRQ）。本阶段命令同步执行，简化：只要模式开启且 FIFO 空就挂起。 */
static void io_gx_fifo_irq_sync(io_t *io)
{
    if ((io->gx.gxstat & GXSTAT_IRQ_MODE) &&
        (io->gx.gxstat & GXSTAT_FIFO_EMPTY))
        io->irq[0].ifl |= IO_IF_GXFIFO;
    else
        io->irq[0].ifl &= ~IO_IF_GXFIFO;
}

void io_gx_write32(io_t *io, uint32_t addr, uint32_t val)
{
    gx_write32(&io->gx, addr, val);
    io_gx_fifo_irq_sync(io);
}

uint32_t io_card_data_read32(io_t *io)
{
    return cartbus_read32(&io->cartbus);
}

void io_card_data_write32(io_t *io, uint32_t val)
{
    (void)val; /* 读 ROM 用不到写端口；EEPROM 等写路径留待后续阶段 */
}

void io_attach_cart(io_t *io, const uint8_t *rom, size_t rom_size)
{
    cartbus_attach(&io->cartbus, rom, rom_size);
}

void io_attach_save(io_t *io, save_type_t type)
{
    cartbus_attach_save(&io->cartbus, type);
}

save_t *io_get_save(io_t *io)
{
    return &io->cartbus.save;
}

void io_set_vblank(io_t *io)
{
    g_vblank_events++;
    /* 21-B9j：VBlank 是 LCD 信号，两套中断控制器都会收到；
       FFXII 的 ARM7 IE bit0 也开着，只有 ARM7 也能被帧事件唤醒，
      service6 的完成状态机才会推进。 */
    /* 21-B9xc：真机 VBlank 从第 192 扫描线开始（不是帧边界）。本函数现在
       在 VCOUNT==192 时被调用；VCOUNT 归零改由 io_frame_boundary() 处理。 */
    irq_set_vblank(&io->irq[0]);
    irq_set_vblank(&io->irq[1]);
    /* DISPSTAT bit0 = VBlank 标志（第 192-262 行期间为 1） */
    io->disp.dispstat = (uint16_t)(io->disp.dispstat | 1u);
    io->disp.dispstat7 = (uint16_t)(io->disp.dispstat7 | 1u);
    io->disp.dispstat_sub = (uint16_t)(io->disp.dispstat_sub | 1u);
    dma_fire(&io->dma[0], io->bus, DMA_START_VBLANK, 0); /* 双核各自 VBlank DMA */
    dma_fire(&io->dma[1], io->bus, DMA_START_VBLANK, 1);
}

/* 21-B9xc：帧边界（VCOUNT 回 0）——离开 VBlank 区间。 */
void io_frame_boundary(io_t *io)
{
    io->vcount = 0;
    io->disp.dispstat = (uint16_t)(io->disp.dispstat & ~1u);
    io->disp.dispstat7 = (uint16_t)(io->disp.dispstat7 & ~1u);
    io->disp.dispstat_sub = (uint16_t)(io->disp.dispstat_sub & ~1u);
}

void io_advance_scanline(io_t *io)
{
    g_scanline_events++;
    io->vcount = (uint16_t)((io->vcount + 1u) % 263u);
    /* 21-B9xj：DISPSTAT 每核一套（melonDS DispStat[0]=ARM9 / DispStat[1]=ARM7），
       VCount 比较值 = bit8-15 | (bit7<<8)，匹配是**边沿触发**（标志已置位不再触发）。
       副引擎（0x04001004）的匹配中断也挂到 ARM9。 */
    uint16_t v = (uint16_t)(io->vcount & 0x1FFu);
    uint16_t *stat[3] = { &io->disp.dispstat, &io->disp.dispstat7,
                          &io->disp.dispstat_sub };
    irq_t *own[3] = { &io->irq[0], &io->irq[1], &io->irq[0] };
    for (int s = 0; s < 3; s++) {
        uint16_t setting = (uint16_t)(((*stat[s] >> 8) & 0xFFu)
                                      | ((*stat[s] & 0x80u) << 1));
        if (setting == v) {
            if (!(*stat[s] & 4u)) {
                *stat[s] |= 4u;
                if (*stat[s] & 0x20u)
                    own[s]->ifl |= 4u;
            }
        } else {
            *stat[s] = (uint16_t)(*stat[s] & ~4u);
        }
    }
}

int io_irq_pending(const io_t *io)
{
    return irq_pending(&io->irq[0]); /* 主循环关心 ARM9 是否发生中断 */
}

/* 21-B9wr：KEYCNT 按键中断检查（melonDS CheckKeyIRQ 口径）。
   bit15=1：掩码内所有键都按下才匹配；bit15=0：任一键按下即匹配。
   只在“上次不匹配 → 现在匹配”的上升沿置 IF bit12。 */
static void io_key_irq_check(io_t *io, int is_arm7, uint16_t oldkey,
                             uint16_t newkey)
{
    uint16_t cnt = io->keycnt[is_arm7 ? 1 : 0];
    if (!(cnt & KEYCNT_IRQ_ENABLE))
        return;
    uint16_t mask = cnt & 0x03FFu;
    uint16_t ok = oldkey & mask;
    uint16_t nk = newkey & mask;
    int oldmatch, newmatch;
    if (cnt & KEYCNT_IRQ_AND) {
        oldmatch = (ok == 0);
        newmatch = (nk == 0);
    } else {
        oldmatch = (ok != mask);
        newmatch = (nk != mask);
    }
    if (!oldmatch && newmatch)
        io->irq[is_arm7 ? 1 : 0].ifl |= IO_IF_KEY;
}

void io_set_keyinput(io_t *io, uint16_t pressed)
{
    uint16_t old = io->keypad.input;
    key_set_pressed(&io->keypad, pressed);
    uint16_t nw = io->keypad.input;
    if (nw != old) {
        io_key_irq_check(io, 0, old, nw);
        io_key_irq_check(io, 1, old, nw);
    }
}

void io_set_touch(io_t *io, uint16_t adc_x, uint16_t adc_y, int down)
{
    touch_set_pos(&io->touch, adc_x, adc_y, down);
}

void io_advance_timers(io_t *io, int is_arm7, uint32_t cycles)
{
    /* 21-B9h：TM0-TM3 溢出对应 IF bit3-bit6，仅 cnt_h bit6（IRQ 使能）时置位 */
    int idx = is_arm7 ? 1 : 0;
    for (int i = 0; i < IO_TIMER_COUNT; i++) {
        if (timer_advance(&io->timer[is_arm7 ? 1 : 0][i], cycles) &&
            (io->timer[is_arm7 ? 1 : 0][i].cnt_h & TIMER_CNT_IRQ))
            io->irq[idx].ifl |= (uint32_t)(1u << (3 + i));
    }
}
void io_advance_cart(io_t *io, int is_arm7)
{
    /* 21-B9zb: 卡带时钟只在 ARM9 指令周期推进；数据就绪边沿触发卡带 IRQ/DMA */
    if (is_arm7)
        return;
    if (cartbus_advance(&io->cartbus, 1u)) {
        irq_set_card(&io->irq[0]);
        irq_set_card(&io->irq[1]);
        io_card_dma_check(io, 0);
        io_card_dma_check(io, 1);
    }
    /* 21-B9yi：一次卡带传输在 FIFO 取空后结束——AUXSPICNT bit14 使能时
       挂两核的卡带完成中断（melonDS `ROMEndTransfer`）。 */
    if (io->cartbus.end_irq) {
        io->cartbus.end_irq = 0;
        irq_set_card(&io->irq[0]);
        irq_set_card(&io->irq[1]);
    }
}
