#include "state.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>

#include "bus/bus.h"
#include "cpu/cpu.h"
#include "io/io.h"
#include "cart/cartbus.h"
#include "snd/snd.h"
#include "gx/gx.h"

/* 21-B9yi(续96)：载荷校验（FNV-1a 32 位）—— 文件被截断/改坏时能挡住。 */
static uint32_t s_hash = 2166136261u;
static uint64_t s_last_frame;

static void hash_reset(void) { s_hash = 2166136261u; }

static void hash_bytes(const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) {
        s_hash ^= b[i];
        s_hash *= 16777619u;
    }
}

static int wr(FILE *f, const void *p, size_t n)
{
    if (n == 0)
        return 0;
    if (fwrite(p, 1, n, f) != n)
        return -1;
    hash_bytes(p, n);
    return 0;
}

static int rd(FILE *f, void *p, size_t n)
{
    if (n == 0)
        return 0;
    if (fread(p, 1, n, f) != n)
        return -1;
    hash_bytes(p, n);
    return 0;
}

uint64_t state_last_frame(void) { return s_last_frame; }

/* 21-B9yi(续96) 诊断：打印「与中断/时间轴有关」的关键状态，便于对比存档/读档两侧。
   `NDS_STATEDBG=1` 打开。 */
static void state_dbg_dump(const nds_t *nds, const char *what)
{
    if (getenv("NDS_STATEDBG") == NULL)
        return;
    printf("statedbg[%s]: f=%llu cpsr9=%08X cpsr7=%08X  ime9=%d ie9=%08X if9=%08X"
           "  ime7=%d ie7=%08X if7=%08X  timer_on=%02X/%02X cart=%u\n",
           what, (unsigned long long)state_last_frame(),
           nds->cpu->cpsr, nds->cpu7->cpsr,
           (int)nds->io->irq[0].ime, nds->io->irq[0].ie, nds->io->irq[0].ifl,
           (int)nds->io->irq[1].ime, nds->io->irq[1].ie, nds->io->irq[1].ifl,
           nds->io->timer_on[0], nds->io->timer_on[1],
           (unsigned)nds->io->cart_clock_on);
    /* 21-B9yi(续100)：把 ARM7 两个定时器的**全部字段**也打出来（含 reload），
       用于核对「存档时刻 vs 读档之后」的定时器状态是否逐字段一致。 */
    for (int i = 0; i < 2; i++)
        printf("statedbg[%s]: t7%d cnt_l=%04X cnt_h=%04X reload=%04X acc=%u\n",
               what, i, nds->io->timer[1][i].cnt_l, nds->io->timer[1][i].cnt_h,
               nds->io->timer[1][i].reload, nds->io->timer[1][i].acc);
    fflush(stdout);
}

/* ---- CLI 便利接口的状态 ---- */
static const char *s_cli_load_path;
static const char *s_cli_save_path;
static uint64_t s_cli_save_frame;      /* 0 = 退出时保存 */
static int s_cli_save_done;
static int s_pending_load;
/* 21-B9yi(续96)：宿主调度时间戳（见 state.h 说明） */
static uint64_t s_host_now, s_host_cost9, s_host_cost7;
static int s_host_valid;
static int s_host_wait9, s_host_wait7;   /* 21-B9yi(续100)：runner 的等待标志 */
static uint64_t s_host_last_now;         /* 21-B9yi(续101)：runner 的 last_now */
static uint64_t s_host_next_line, s_host_next_frame;   /* 21-B9yi(续106)：事件截止时刻 */

void state_set_host_sched(uint64_t next_line, uint64_t next_frame)
{
    s_host_next_line = next_line;
    s_host_next_frame = next_frame;
}

int state_get_host_sched(uint64_t *next_line, uint64_t *next_frame)
{
    if (next_line != NULL) *next_line = s_host_next_line;
    if (next_frame != NULL) *next_frame = s_host_next_frame;
    return 1;
}

void state_set_host_last_now(uint64_t last_now) { s_host_last_now = last_now; }

int state_get_host_last_now(uint64_t *last_now)
{
    if (last_now != NULL) *last_now = s_host_last_now;
    return 1;
}

void state_set_host_wait(int wait9, int wait7)
{
    s_host_wait9 = wait9;
    s_host_wait7 = wait7;
}

int state_get_host_wait(int *wait9, int *wait7)
{
    if (wait9 != NULL) *wait9 = s_host_wait9;
    if (wait7 != NULL) *wait7 = s_host_wait7;
    return 1;
}

void state_set_host_time(uint64_t now, uint64_t cost9, uint64_t cost7)
{
    s_host_now = now;
    s_host_cost9 = cost9;
    s_host_cost7 = cost7;
    s_host_valid = 1;
}

int state_get_host_time(uint64_t *now, uint64_t *cost9, uint64_t *cost7)
{
    if (!s_host_valid)
        return 0;
    if (now != NULL) *now = s_host_now;
    if (cost9 != NULL) *cost9 = s_host_cost9;
    if (cost7 != NULL) *cost7 = s_host_cost7;
    return 1;
}

void state_cli_set_load(const char *path) { s_cli_load_path = path; }

void state_cli_set_save(const char *path, uint64_t frame)
{
    s_cli_save_path = path;
    s_cli_save_frame = frame;
    s_cli_save_done = 0;
}

int state_take_pending_load(void)
{
    int v = s_pending_load;
    s_pending_load = 0;
    return v;
}

int state_cli_load_now(nds_t *nds)
{
    if (s_cli_load_path == NULL)
        return 0;
    if (state_load(nds, s_cli_load_path) != 0) {
        fprintf(stderr, "state: 读档失败 %s\n", s_cli_load_path);
        return -1;
    }
    s_pending_load = 1;
    printf("state: 已读档 %s（帧 %llu）\n", s_cli_load_path,
           (unsigned long long)state_last_frame());
    fflush(stdout);
    return 1;
}

int state_cli_save_at_frame(const nds_t *nds, uint64_t frame)
{
    if (s_cli_save_path == NULL || s_cli_save_frame == 0 || s_cli_save_done)
        return 0;
    if (frame < s_cli_save_frame)
        return 0;
    s_cli_save_done = 1;
    if (state_save(nds, frame, s_cli_save_path) != 0) {
        fprintf(stderr, "state: 保存失败 %s\n", s_cli_save_path);
        return 0;
    }
    printf("state: 已保存 %s（帧 %llu）\n", s_cli_save_path,
           (unsigned long long)frame);
    fflush(stdout);
    return 1;
}

void state_cli_save_at_exit(const nds_t *nds, uint64_t frame)
{
    if (s_cli_save_path == NULL || s_cli_save_frame != 0 || s_cli_save_done)
        return;
    s_cli_save_done = 1;
    if (state_save(nds, frame, s_cli_save_path) != 0)
        fprintf(stderr, "state: 保存失败 %s\n", s_cli_save_path);
    else
        printf("state: 已保存 %s（帧 %llu）\n", s_cli_save_path,
               (unsigned long long)frame);
    fflush(stdout);
}

/* 指针字段在这几个结构里，读写时都要「保留当前对象」的那份：
   bus->io、cpu->nds、io->bus、gx.bus、cartbus.rom、cartbus.save.data。 */
/* bus 的「标量/映射表」部分（内存数组单独存，避免写两遍 4MB）。
   这里只覆盖会影响行为的派生状态：VRAMCNT 与各映射表、DTCM 配置。 */
static void bus_scalars_write(FILE *f, const bus_t *bus, int *ok)
{
    if (wr(f, bus->vramcnt, sizeof bus->vramcnt) != 0) *ok = -1;
    if (wr(f, bus->vram_map_abg, sizeof bus->vram_map_abg) != 0) *ok = -1;
    if (wr(f, bus->vram_map_aobj, sizeof bus->vram_map_aobj) != 0) *ok = -1;
    if (wr(f, bus->vram_map_bbg, sizeof bus->vram_map_bbg) != 0) *ok = -1;
    if (wr(f, bus->vram_map_bobj, sizeof bus->vram_map_bobj) != 0) *ok = -1;
    if (wr(f, bus->vram_map_tex, sizeof bus->vram_map_tex) != 0) *ok = -1;
    if (wr(f, bus->vram_map_texpal, sizeof bus->vram_map_texpal) != 0) *ok = -1;
    if (wr(f, bus->vram_map_abg_ext, sizeof bus->vram_map_abg_ext) != 0) *ok = -1;
    if (wr(f, bus->vram_map_bbg_ext, sizeof bus->vram_map_bbg_ext) != 0) *ok = -1;
    if (wr(f, &bus->vram_map_aobj_ext, sizeof bus->vram_map_aobj_ext) != 0) *ok = -1;
    if (wr(f, &bus->vram_map_bobj_ext, sizeof bus->vram_map_bobj_ext) != 0) *ok = -1;
    if (wr(f, &bus->arm9_dtcm_on, sizeof bus->arm9_dtcm_on) != 0) *ok = -1;
    if (wr(f, &bus->arm9_dtcm_base, sizeof bus->arm9_dtcm_base) != 0) *ok = -1;
    if (wr(f, &bus->arm9_dtcm_size, sizeof bus->arm9_dtcm_size) != 0) *ok = -1;
}

static void bus_scalars_read(FILE *f, bus_t *bus, int *ok)
{
    if (rd(f, bus->vramcnt, sizeof bus->vramcnt) != 0) *ok = -1;
    if (rd(f, bus->vram_map_abg, sizeof bus->vram_map_abg) != 0) *ok = -1;
    if (rd(f, bus->vram_map_aobj, sizeof bus->vram_map_aobj) != 0) *ok = -1;
    if (rd(f, bus->vram_map_bbg, sizeof bus->vram_map_bbg) != 0) *ok = -1;
    if (rd(f, bus->vram_map_bobj, sizeof bus->vram_map_bobj) != 0) *ok = -1;
    if (rd(f, bus->vram_map_tex, sizeof bus->vram_map_tex) != 0) *ok = -1;
    if (rd(f, bus->vram_map_texpal, sizeof bus->vram_map_texpal) != 0) *ok = -1;
    if (rd(f, bus->vram_map_abg_ext, sizeof bus->vram_map_abg_ext) != 0) *ok = -1;
    if (rd(f, bus->vram_map_bbg_ext, sizeof bus->vram_map_bbg_ext) != 0) *ok = -1;
    if (rd(f, &bus->vram_map_aobj_ext, sizeof bus->vram_map_aobj_ext) != 0) *ok = -1;
    if (rd(f, &bus->vram_map_bobj_ext, sizeof bus->vram_map_bobj_ext) != 0) *ok = -1;
    if (rd(f, &bus->arm9_dtcm_on, sizeof bus->arm9_dtcm_on) != 0) *ok = -1;
    if (rd(f, &bus->arm9_dtcm_base, sizeof bus->arm9_dtcm_base) != 0) *ok = -1;
    if (rd(f, &bus->arm9_dtcm_size, sizeof bus->arm9_dtcm_size) != 0) *ok = -1;
}

static void cpu_snapshot(const arm_cpu_t *src, arm_cpu_t *out)
{
    *out = *src;
    out->nds = NULL;
}

static void cpu_restore(arm_cpu_t *dst, const arm_cpu_t *in)
{
    nds_t *keep = dst->nds;
    *dst = *in;
    dst->nds = keep;
}

static void io_snapshot(const io_t *src, io_t *out)
{
    *out = *src;
    out->bus = NULL;                 /* 指针：保留 */
    out->cartbus.rom = NULL;         /* 指针：保留（ROM 不存档） */
    out->cartbus.save.data = NULL;   /* 指针：保留（内容单独存） */
    out->gx.bus = NULL;
}

static void io_restore(io_t *dst, const io_t *in)
{
    struct bus *keep_bus = dst->bus;
    const uint8_t *keep_rom = dst->cartbus.rom;
    uint8_t *keep_save = dst->cartbus.save.data;
    struct bus *keep_gxbus = dst->gx.bus;
    *dst = *in;
    dst->bus = keep_bus;
    dst->cartbus.rom = keep_rom;
    dst->cartbus.save.data = keep_save;
    dst->gx.bus = keep_gxbus;
}

int state_save(const nds_t *nds, uint64_t frame, const char *path)
{
    if (nds == NULL || nds->bus == NULL || nds->io == NULL ||
        nds->cpu == NULL || nds->cpu7 == NULL || path == NULL)
        return -1;

    FILE *f = fopen(path, "wb");
    if (f == NULL)
        return -1;

    io_t *io_snap = (io_t *)malloc(sizeof(io_t));
    if (io_snap == NULL) {
        free(io_snap);
        fclose(f);
        return -1;
    }
    io_snapshot(nds->io, io_snap);

    save_t *sv = &nds->io->cartbus.save;
    uint32_t rom_size = (uint32_t)nds->io->cartbus.rom_size;
    uint32_t save_size = (uint32_t)sv->size;

    hash_reset();
    int ok = 0;
    if (fwrite(STATE_MAGIC, 1, 8, f) != 8)
        ok = -1;
    if (ok == 0 && wr(f, &(uint32_t){ STATE_VERSION }, 4) != 0) ok = -1;
    if (ok == 0 && wr(f, &frame, 8) != 0) ok = -1;
    if (ok == 0 && wr(f, &rom_size, 4) != 0) ok = -1;
    if (ok == 0 && wr(f, &save_size, 4) != 0) ok = -1;
    /* 宿主调度时间戳（续96）：时间轴精确还原用 */
    if (ok == 0 && wr(f, &s_host_now, 8) != 0) ok = -1;
    if (ok == 0 && wr(f, &s_host_cost9, 8) != 0) ok = -1;
    if (ok == 0 && wr(f, &s_host_cost7, 8) != 0) ok = -1;
    if (ok == 0 && wr(f, &s_host_wait9, 4) != 0) ok = -1;
    if (ok == 0 && wr(f, &s_host_wait7, 4) != 0) ok = -1;
    if (ok == 0 && wr(f, &s_host_last_now, 8) != 0) ok = -1;
    if (ok == 0 && wr(f, &s_host_next_line, 8) != 0) ok = -1;
    if (ok == 0 && wr(f, &s_host_next_frame, 8) != 0) ok = -1;
    /* 21-B9yi(续100)：GX 的**文件级静态状态**（命令队列 + 条目流）——
       它们不是 gx_t 的成员，此前没进存档，读档后 GPU 侧与存档时刻不一致。 */
    {
        static uint8_t gxblob[128 * 1024];
        size_t gn = gx_state_size();
        if (ok == 0 && (gn == 0 || gn > sizeof gxblob)) ok = -1;
        if (ok == 0 && wr(f, &gn, sizeof gn) != 0) ok = -1;
        if (ok == 0) {
            gx_state_save(gxblob);
            if (wr(f, gxblob, gn) != 0) ok = -1;
        }
    }
    /* 内存块 */
    if (ok == 0 && wr(f, nds->bus->main_ram, sizeof nds->bus->main_ram) != 0) ok = -1;
    if (ok == 0 && wr(f, nds->bus->arm9_itcm, sizeof nds->bus->arm9_itcm) != 0) ok = -1;
    if (ok == 0 && wr(f, nds->bus->arm9_dtcm, sizeof nds->bus->arm9_dtcm) != 0) ok = -1;
    if (ok == 0 && wr(f, nds->bus->arm7_wram, sizeof nds->bus->arm7_wram) != 0) ok = -1;
    if (ok == 0 && wr(f, nds->bus->shared_wram, sizeof nds->bus->shared_wram) != 0) ok = -1;
    if (ok == 0 && wr(f, nds->bus->vram, sizeof nds->bus->vram) != 0) ok = -1;
    if (ok == 0 && wr(f, nds->bus->palette, sizeof nds->bus->palette) != 0) ok = -1;
    if (ok == 0 && wr(f, nds->bus->oam, sizeof nds->bus->oam) != 0) ok = -1;
    /* 总线标量 + 两颗 CPU + IO 区 + 存档芯片内容 */
    if (ok == 0) bus_scalars_write(f, nds->bus, &ok);
    if (ok == 0 && wr(f, nds->cpu, offsetof(arm_cpu_t, nds)) != 0) ok = -1;   /* nds 指针之前的字段 */
    if (ok == 0 && wr(f, (const uint8_t *)nds->cpu + offsetof(arm_cpu_t, nds) + sizeof(void *),
                      sizeof(arm_cpu_t) - offsetof(arm_cpu_t, nds) - sizeof(void *)) != 0) ok = -1;
    if (ok == 0 && wr(f, nds->cpu7, offsetof(arm_cpu_t, nds)) != 0) ok = -1;
    if (ok == 0 && wr(f, (const uint8_t *)nds->cpu7 + offsetof(arm_cpu_t, nds) + sizeof(void *),
                      sizeof(arm_cpu_t) - offsetof(arm_cpu_t, nds) - sizeof(void *)) != 0) ok = -1;
    if (ok == 0 && wr(f, io_snap, sizeof *io_snap) != 0) ok = -1;
    if (ok == 0 && save_size != 0 && sv->data != NULL &&
        wr(f, sv->data, save_size) != 0) ok = -1;

    free(io_snap);

    /* 校验和回填（放在 magic 之后、版本之前的 4 字节） */
    uint32_t sum = s_hash;
    if (ok == 0 && fseek(f, 8 + 4 + 8 + 4 + 4, SEEK_SET) == 0) {
        /* 位置在载荷开头；真正的校验和写在文件头之后 */
    }
    if (ok == 0 && fseek(f, 0, SEEK_END) != 0) ok = -1;
    if (ok == 0 && fwrite(&sum, 1, 4, f) != 4) ok = -1;   /* 校验和追加在末尾 */
    fclose(f);
    if (ok == 0)
        s_last_frame = frame;
    if (ok == 0)
        state_dbg_dump(nds, "save");
    return ok;
}

int state_load(nds_t *nds, const char *path)
{
    if (nds == NULL || nds->bus == NULL || nds->io == NULL ||
        nds->cpu == NULL || nds->cpu7 == NULL || path == NULL)
        return -1;

    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return -1;

    io_t *io_new = (io_t *)malloc(sizeof(io_t));
    uint8_t *save_data = NULL;
    if (io_new == NULL) {
        free(io_new);
        fclose(f);
        return -1;
    }

    char magic[8];
    uint32_t version = 0, rom_size = 0, save_size = 0;
    uint64_t frame = 0;
    int ok = 0;
    hash_reset();
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, STATE_MAGIC, 8) != 0)
        ok = -1;
    if (ok == 0 && (rd(f, &version, 4) != 0 || version != STATE_VERSION)) ok = -1;
    if (ok == 0 && rd(f, &frame, 8) != 0) ok = -1;
    if (ok == 0 && rd(f, &rom_size, 4) != 0) ok = -1;
    if (ok == 0 && rd(f, &save_size, 4) != 0) ok = -1;
    uint64_t host_now = 0, host_cost9 = 0, host_cost7 = 0;
    if (ok == 0 && rd(f, &host_now, 8) != 0) ok = -1;
    if (ok == 0 && rd(f, &host_cost9, 8) != 0) ok = -1;
    if (ok == 0 && rd(f, &host_cost7, 8) != 0) ok = -1;
    int host_wait9 = 0, host_wait7 = 0;
    if (ok == 0 && rd(f, &host_wait9, 4) != 0) ok = -1;
    if (ok == 0 && rd(f, &host_wait7, 4) != 0) ok = -1;
    uint64_t host_last_now = 0;
    if (ok == 0 && rd(f, &host_last_now, 8) != 0) ok = -1;
    uint64_t host_next_line = 0, host_next_frame = 0;
    if (ok == 0 && rd(f, &host_next_line, 8) != 0) ok = -1;
    if (ok == 0 && rd(f, &host_next_frame, 8) != 0) ok = -1;
    /* GX 静态状态（续100）：长度不符就拒绝 */
    uint8_t gxblob[128 * 1024];
    size_t gn = 0;
    if (ok == 0 && rd(f, &gn, sizeof gn) != 0) ok = -1;
    if (ok == 0 && (gn == 0 || gn > sizeof gxblob)) ok = -1;
    if (ok == 0 && rd(f, gxblob, gn) != 0) ok = -1;
    if (ok == 0 && gx_state_load(gxblob, gn) != 0) ok = -1;
    if (ok == 0 && rom_size != (uint32_t)nds->io->cartbus.rom_size) {
        fprintf(stderr, "state: ROM 大小不符（存档 %u / 当前 %u）\n",
                rom_size, (uint32_t)nds->io->cartbus.rom_size);
        ok = -1;
    }
    if (ok == 0 && save_size != (uint32_t)nds->io->cartbus.save.size) {
        fprintf(stderr, "state: 存档芯片大小不符（存档 %u / 当前 %u）\n",
                save_size, (uint32_t)nds->io->cartbus.save.size);
        ok = -1;
    }

    if (ok == 0) {
        save_data = (uint8_t *)malloc(save_size ? save_size : 1u);
        if (save_data == NULL)
            ok = -1;
    }
    if (ok == 0 && rd(f, nds->bus->main_ram, sizeof nds->bus->main_ram) != 0) ok = -1;
    if (ok == 0 && rd(f, nds->bus->arm9_itcm, sizeof nds->bus->arm9_itcm) != 0) ok = -1;
    if (ok == 0 && rd(f, nds->bus->arm9_dtcm, sizeof nds->bus->arm9_dtcm) != 0) ok = -1;
    if (ok == 0 && rd(f, nds->bus->arm7_wram, sizeof nds->bus->arm7_wram) != 0) ok = -1;
    if (ok == 0 && rd(f, nds->bus->shared_wram, sizeof nds->bus->shared_wram) != 0) ok = -1;
    if (ok == 0 && rd(f, nds->bus->vram, sizeof nds->bus->vram) != 0) ok = -1;
    if (ok == 0 && rd(f, nds->bus->palette, sizeof nds->bus->palette) != 0) ok = -1;
    if (ok == 0 && rd(f, nds->bus->oam, sizeof nds->bus->oam) != 0) ok = -1;
    if (ok == 0) bus_scalars_read(f, nds->bus, &ok);   /* 注意：失败时可能已部分覆盖（见下） */
    if (ok == 0) {
        /* CPU：按「指针之前的字段 + 指针之后的字段」两段读回 */
        arm_cpu_t tmp;
        memset(&tmp, 0, sizeof tmp);
        if (rd(f, &tmp, offsetof(arm_cpu_t, nds)) != 0) ok = -1;
        if (ok == 0 && rd(f, (uint8_t *)&tmp + offsetof(arm_cpu_t, nds) + sizeof(void *),
                          sizeof tmp - offsetof(arm_cpu_t, nds) - sizeof(void *)) != 0) ok = -1;
        if (ok == 0) cpu_restore(nds->cpu, &tmp);
        memset(&tmp, 0, sizeof tmp);
        if (ok == 0 && rd(f, &tmp, offsetof(arm_cpu_t, nds)) != 0) ok = -1;
        if (ok == 0 && rd(f, (uint8_t *)&tmp + offsetof(arm_cpu_t, nds) + sizeof(void *),
                          sizeof tmp - offsetof(arm_cpu_t, nds) - sizeof(void *)) != 0) ok = -1;
        if (ok == 0) cpu_restore(nds->cpu7, &tmp);
    }
    if (ok == 0 && rd(f, io_new, sizeof *io_new) != 0) ok = -1;
    if (ok == 0 && save_size != 0 && rd(f, save_data, save_size) != 0) ok = -1;

    uint32_t want = 0, got = s_hash;
    if (ok == 0 && fread(&want, 1, 4, f) != 4) ok = -1;
    if (ok == 0 && want != got) {
        fprintf(stderr, "state: 校验和不符（存档 %08X / 计算 %08X）\n", want, got);
        ok = -1;
    }
    fclose(f);

    if (ok == 0) {
        io_restore(nds->io, io_new);
        if (save_size != 0 && save_data != NULL)
            memcpy(nds->io->cartbus.save.data, save_data, save_size);
        s_last_frame = frame;
        s_host_now = host_now;
        s_host_cost9 = host_cost9;
        s_host_cost7 = host_cost7;
        s_host_valid = 1;
        s_host_wait9 = host_wait9;
        s_host_wait7 = host_wait7;
        s_host_last_now = host_last_now;
        s_host_next_line = host_next_line;
        s_host_next_frame = host_next_frame;
        state_dbg_dump(nds, "load");
    }
    free(io_new);
    free(save_data);
    return ok;
}
