#ifndef NDS_EMU_STATE_H
#define NDS_EMU_STATE_H

#include <stdint.h>
#include "nds/nds.h"

/* 阶段 21-B9yi(续96)：**即时存档 / 读档**（save state）。

   为什么需要：玩到关键处（战斗前、试菜单前）先存一个点，试错成本就降下来了；
   `--load-state` 也让「同一状态下的重复实验」变得便宜（性能/保真度对照常用）。

   存什么：**整机核心状态** ——
     * 内存：Main RAM(4MB) / ARM7 WRAM(64KB) / Shared WRAM(32KB) / VRAM(656KB) /
             ITCM(32KB) / DTCM(16KB) / 调色板(2KB) / OAM(2KB)
     * 两颗 CPU（寄存器/CPSR/SPSR/模式/周期计数/CP15）
     * IO 寄存器区（中断/定时器/按键/DMA/FIFO/同步/显示/触摸/存储控制/电源/数学/音频/GX/卡带总线）
     * 存档芯片内容（内含 SPI 状态机）
   不存什么：ROM（只读、由启动时的镜像决定）、宿主侧（SDL 窗口/纹理/菜单），
   以及各结构里的**指针**（bus->io、cpu->nds、gx->bus、cartbus->rom、save->data）——
   读档时保留**当前对象**的指针，只覆盖数据字段。

   文件格式：8 字节 magic + u32 版本 + u64 帧号 + u32 ROM 大小 + u32 载荷校验(FNV-1a)，
   之后是各块数据。校验不符 / 版本不符 / 存档芯片大小不符都会拒绝读入。 */

#define STATE_MAGIC   "NDSST001"
#define STATE_VERSION 1u

/* 保存到 path。frame：调用方的当前帧号（读档后可用 state_last_frame 取回）。
   成功返回 0，失败返回 -1。 */
int state_save(const nds_t *nds, uint64_t frame, const char *path);

/* 从 path 读入并覆盖 nds 的核心状态。成功返回 0，失败返回 -1（失败时不改动状态）。 */
int state_load(nds_t *nds, const char *path);

/* 最近一次成功 save/load 的帧号。 */
uint64_t state_last_frame(void);

/* ---- 21-B9yi(续96)：CLI / 主循环用的便利接口 ----
   语义（与命令行一一对应）：
     `--load-state PATH`      → state_cli_set_load(path)；启动装载完 ROM 后调用
                                  state_cli_load_now() 真正读入。
     `--save-state PATH`      → state_cli_set_save(path, 0)：跑完（退出前）保存。
       配合 `--state-save-frame N` → state_cli_set_save(path, N)：跑到第 N 帧时保存，
                                  然后继续跑（用于做「存档→读档」一致性验证）。 */
void state_cli_set_load(const char *path);
void state_cli_set_save(const char *path, uint64_t frame);
int  state_cli_load_now(nds_t *nds);            /* 1 = 已读档，0 = 未配置，-1 = 失败 */
int  state_cli_save_at_frame(const nds_t *nds, uint64_t frame);  /* 1 = 本次保存了 */
void state_cli_save_at_exit(const nds_t *nds, uint64_t frame);
int  state_take_pending_load(void);             /* 读档后取一次「需要重同步时间轴」标志 */

/* 21-B9yi(续96)：**宿主调度时间戳**（runner 的 tm.now 与两颗 CPU 的周期预算）。
   保存前调用 state_set_host_time() 记录，读档后用 state_get_host_time() 取回，
   让调度时间轴精确回到存档时刻（只靠 CPU 指令计数是不够的：时间轴的单位是
   系统时钟，而 cpu->cycles 是**指令条数**，两者不是一回事——实测差 346 次中断）。 */
/* 21-B9yi(续100)：把 runner 的两个**等待标志**也一起存档（`a9_wait/a7_wait`）。
   它们是「该核是否在 WFI/挂起」的调度状态，直接决定读档后**定时器是否继续走**
   （runner 只在核处于等待时推进它那一侧的定时器）——靠 `step_cycles==0` 猜不准。 */
void state_set_host_time(uint64_t now, uint64_t cost9, uint64_t cost7);
int  state_get_host_time(uint64_t *now, uint64_t *cost9, uint64_t *cost7);
void state_set_host_wait(int wait9, int wait7);
int  state_get_host_wait(int *wait9, int *wait7);
/* 21-B9yi(续101)：**上一次推进后的时间戳 `last_now`** —— 它决定下一步的
   `delta = tm.now - last_now`（定时器就是按这个 delta 计费的）。存档点上
   `last_now` 可能比 `tm.now` 少 1 个周期，读档时若直接令 `last_now = now`
   就会让之后每一步的计费都差 1 个周期（实测正是这个量级）。 */
void state_set_host_last_now(uint64_t last_now);
int  state_get_host_last_now(uint64_t *last_now);

#endif /* NDS_EMU_STATE_H */
