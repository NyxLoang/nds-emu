# bios 模块日志

## 11.1 — 短文：BIOS / SWI / HLE

- 新增 `docs/12-bios-hle.md`：BIOS 是双核各自的内置 ROM（ARM9 32KB / ARM7 16KB）；
  `SWI <n>` 是游戏调用系统服务的方式（ARM 态 `swi n<<16`，函数号 = `swi_num>>16`）；
  HLE 是「不执行 BIOS ROM、用 C 直接实现同名函数」。
- 附 NDS SWI 编号表（ARM9 视角，标注 NDS7 差异）与寄存器约定；并注明 `DivArm` 是 GBA 专属。

## 11.2 — SWI 分发框架（src/bios/ 模块）

- 按模块结构规则新建 `src/bios/`：
  - `bios.h/.c`：对外接口——SWI 函数号宏、返回值宏（HANDLED/UNKNOWN/WAIT）、`bios_dispatch` 分发。
  - `bios_arith.h/.c`：Div/Sqrt；`bios_mem.h/.c`：CpuSet/CpuFastSet；
    `bios_decompress.h/.c`：BitUnPack/LZ77/RL/Huffman；`bios_wait.h/.c`：Halt/IntrWait/VBlankIntrWait/WaitByLoop。
- `exec.c` 的 SWI 处理改为：`swi_num = insn & 0xFFFFFF` → `bios_dispatch(swi_num>>16, cpu)`。
  - 返回 HANDLED/UNKNOWN → `PC += 4`；返回 WAIT → `PC` 不动，重跑本 SWI（等价忙等）。
- 未知号只打印一条日志、不崩溃（`test_swi` 里 SWI 0x123 → 号 0 → 走未知分支仍通过）。

## 11.3 — 除法 / 开方（bios_arith.c）

- `bios_div`（0x09）：`r0/r1` 有符号除法；出 `r0=商`、`r1=余数`、`r3=|商|`。除零真机死循环，
  这里防御性返回 `0/被除数` 避免 C 的 UB。
- `bios_sqrt`（0x0D）：逐位试商整数开方，结果 16 位向下取整（`sqrt(2)=1`、`sqrt(max)=65535`）。
- 注：`DivArm`（GBA 0x07）在 NDS 已移除，未实现。

## 11.4 — 内存搬移（bios_mem.c）

- `bios_cpuset`（0x0B）：按 `r2` 控制字——bit0-20 计数、bit24 固定源、bit26 数据宽度（0=16 位/1=32 位）；
  支持递增拷贝与固定填充。
- `bios_cpufastset`（0x0C）：32 位字为单位拷贝/填充（32 字节块优化是性能细节，语义等同逐字拷贝）。

## 11.5 — 解压（bios_decompress.c）

- `bios_bitunpack`（0x10）：按 UnPack 信息（源/目的位宽、偏移、零标志）把窄位宽源单元展开为宽位宽
  目的单元；源位流按 MSB-first 读取，目的按 32 位单位写入。
- `bios_lz77`（0x11/0x12）：LZ77 解压（标志字节 MSB 先、8 块；字面量/回溯 disp·len）。
- `bios_rl`（0x14/0x15）：RLE 解压（标志 bit7 区分字面量/重复）。
- `bios_huff`（0x13）：Huffman 解压（树节点偏移寻址 + 位流 bit31 先出）。
- Wram(8bit)/Vram(16bit) 两个变体在本模拟器都走 8 位写（VRAM 支持 8 位写，输出字节一致）。

## 11.6 — 等待（bios_wait.c）

- `bios_halt`（0x06）：等 `(IE & IF) != 0`（IME/CPSR 忽略位无关）。
- `bios_intr_wait`（0x04）：强制 `IME=1`，等 `r1` 指定中断位，返回前清位。
- `bios_vblank_intr_wait`（0x05）：等 VBlank 位（IF bit3），返回前清位。
- 未满足时返回 `BIOS_RET_WAIT` → `exec.c` 不前进 PC，等价低功耗等待（真机此刻视频/定时器继续跑）。
- `bios_wait_by_loop`（0x03）：本模拟器不建模精确时序，按空操作处理。

## 2026-09-06 · 21-B9wf — ARM7 FreeBIOS 低地址等待路径 HLE

- **做了什么**：
  - 修正 0x115C 的语义：它是 SWI 3 WaitByLoop（`subs r0,#1; bgt`）的循环体，
    不是 Halt。ARM7 SWI 3 不再空操作：进入低地址状态后每步减 r0、每轮约
    3 周期（step_cycles=3），减到 0 后等价执行 BIOS 0x112C 尾部并恢复调用方
    CPSR/PC；r0 归零，供调用方判断。
  - ARM7 SWI 6 Halt 改走暂停态：无 (IF&IE) 时停住且不消耗周期；唤醒不要求
    IME（melonDS HaltInterrupted 口径），唤醒后先走 BIOS 0x1158→0x112C 尾部，
    IRQ 在尾部恢复现场后才接管，避免把 BIOS 内部 PC 当任务现场保存。
  - `bios_dispatch` 新增 REDIR 返回值：HLE 已改写 PC/CPSR 时 exec/thumb 不再
    机械前进。
- **怎么验证**：新增 `[case 21-B9wf]`（delay 计数/成本/尾部恢复 + Halt 暂停/
    唤醒/尾部恢复共 20 项）；ARM7 行为不影响 ARM9 既有 Halt/IntrWait 用例。
- **结果**：全量 **790 项检查 0 失败**。真 ROM headless 中 ARM7 首次稳定进入
  0x1158/0x115C 低地址暂停/延迟态；事件驱动的“等待核唤醒后补跑积压指令”
  跑飞已修复，时间/事件调度仍待继续完善。

## 2026-09-05 · 21-B9 — SWI 0x0E GetCRC16（bios_crc16.c）

- 新建 `bios_crc16.h/.c`：实现 NDS9/NDS7 通用的 GetCRC16。
- 寄存器约定（与 libnds `swiCRC16` 一致）：`r0=CRC 初值`、`r1=数据地址`、
  `r2=数据字节数`，返回 `r0=CRC16`。
- 算法：标准 CRC-16/IBM（输入/结果反射、生成多项式 0x8005，反射多项式 0xA001），
  每字节先异或进 CRC 低 8 位，再逐位右移 8 次、移出进位时异或 0xA001；末尾不反转。
  GBATEK 的逐位伪代码与之等价；真机常见初值 0xFFFF。
- `bios.c` 分发器登记 `BIOS_SWI_GET_CRC16 → bios_crc16`，不再是 unknown SWI；
  `CMakeLists.txt` 加入新源文件。
- 单测 `[case 21-B9a]`：ASCII `"123456789"` 以 0xFFFF 为初值得 0x4B37，覆盖
  ARM/Thumb/ARM7 三种执行路径与长度 0 返回初值；全量 **600 项检查 0 失败**。
- 真机 headless：`bios: unknown SWI 0x0E` 消失；ARM7 终点由 0x000366A4
  前进到 0x037FC89C（进入自研 boot 代码轮询区），新卡点为 ARM9 高向量/IRQ
  与一批未知 IO 桩。

## 2026-09-06 · 21-B9l — SWI 0x08 SoundBias（仅 NDS7）

- FFXII ARM7 在 0x038043FA 调用 `SWI 0x08`；此前 0x08 未登记，unknown SWI 把
  ARM7 送去 0x00000008 向量，随后在低地址未映射区逐 2 字节漂移。
- 新增 `bios_snd.h/.c`：SoundBias 把 SOUNDBIAS 电平调到 r0 指定目标
  （0=000h，其它=200h），保留寄存器高 6 位；r1 延迟在瞬时模型下忽略。
- `bios.c` 只在 ARM7 核登记 0x08（ARM9 无此函数，仍走未知号异常路径）。
- 新增 `[case 21-B9l]` Thumb 双参数断言；全量 **704 项检查 0 失败**。真 ROM
  ARM7 不再漂移，稳定回到 0x038043E2 的 Halt 服务入口。

## 2026-09-06 · 21-B9wi — ARM7 SWI 0x1A-0x1D 音频/启动查表

- **证据**：ARM7 逐指令日志在 4,478,972 周期走到 0x0380443C 的 Thumb
  `swi 0x1C`（DF1C），随后本地把未知号送去 0x00000008 向量，在低地址全零
  区逐 2 字节漂移到 0x3FCE、0x570CC…。FreeBIOS 低地址 SWI 表把 0x1C 定义
  为 ARM7 专用 `GetVolumeTable`（r0=0x2A0 时返回 0x47），0x1A/0x1B 是
  正弦/音高表，0x1D 返回三个启动处理器句柄。
- **做了什么**：
  - 新增 `bios_audio_tables.h`：从 melonDS 使用的 FreeBIOS
    `bios_common.s` 机械抽取正弦 64 半字、音高 768 半字、音量 724 字节
    三张表；
  - `bios_snd.c` 实现 `bios_get_sine/pitch/volume_table` 与
    `bios_get_boot_procs`；`bios.c` 只在 ARM7 核登记 0x1A-0x1D；
  - runner 事件驱动 headless 结束处补帧号/VRAM 非零统计，并让
    `--screenshot` 真正可输出事件驱动画面。
- **验证**：新增 `[case 21-B9wi]` 10 项断言（三种表的关键索引 + 启动句柄），
  全量 **805 项检查 0 失败**。真 ROM 事件驱动 1.5 亿步内 ARM7 不再出现
  0x00003xxx/0x00xxxxxx 漂移，终点稳定停 0x1158；4 亿/9 亿步同样全程
  无跑飞，ARM7 终点 PC=0x1158。

## 2026-09-12 · 21-B9ws — ARM7 FreeBIOS 低地址 Halt/调度路径参考级 HLE

把 ARM7 的「SWI/IRQ 进出 BIOS 低地址」从模拟器私有桩改成与参考核
（melonDS + FreeBIOS）逐地址等价的 HLE。新增 `src/bios/bios7_low.{h,c}`
（低地址路径执行器）与 `src/bios/bios7_image.{h,c}`（可读字节影子），
`src/bios/bios_wait.c` 的 ARM7 分支全部撤掉（ARM9 保持原样）。

**真机路径（FreeBIOS ARM7 镜像反汇编，地址逐字核对）**：

```
0x00000008 SWI 向量 → b 0x1080
0x00001080 SWI 分发器：push {r4,r12,lr} / mrs r4,SPSR / push {r4}
           r4=(SPSR&0x80)|0x1F / r12=byte[lr-2] / msr CPSR_fc,r4
           push {lr}(System) / cmp r12,#0x20 / ldr pc,[pc,r12,lsl#2]
0x000010B0 SWI 函数表（31 项，逐字读出；0x1F 在 FreeBIOS 里落到表外）
0x0000112C swi_complete：pop System lr → msr CPSR_fc,#0xD3 → pop SPSR →
           msr SPSR_fc → pop {r4,r12,lr} → movs pc,lr
0x0000114C Halt（mov r0,#0x04000000 / mov r2,#0x80 / strb → HALTCNT=0x80）
0x0000115C WaitByLoop（subs r0,#1; bgt，3 周期/轮）
0x00001168 interrupt_check（软件中断标志 0x03FFFFF8 + IME 0/1 开关）
0x00001188/0x1190 VBlankIntrWait / IntrWait（HALTCNT + 标志轮询循环）
0x00001FD8 SoftReset（清 WRAM 顶 512B、重设三套栈、r0-r12=0、movs pc,lr）
0x00000018 IRQ 向量 → b 0x1FB0
0x00001FB0 IRQ 入口：push {r0-r3,r12,lr} / ldr pc,[0x04000000-4]=[0x03FFFFFC]
0x00001FC0 IRQ 返回：pop {r0-r3,r12,lr} + subs pc,lr,#4（SPSR_irq 恢复）
```

**参考核实测对照**（melonDS 参考树加低地址事件钩子，6000 帧）：

| 事件 | 参考核 | 本地（本步之后） |
|------|--------|------------------|
| SWI 入口 | `pc=00000008 cpsr=60000093 lr=038043CA sp=0380FFC0`（I=1、T 已清） | 同样走异常入口，CPSR 低 8 位口径已对齐 `0x93` |
| 分发器 | `0x1080` → SVC 栈 4 字 → System 模式 → `0x115C/0x114C` | HLE 逐条等价（同栈帧、同模式、同 PC） |
| WaitByLoop | `0x115C/0x1160` 每轮 3 周期（r0=0xFA 起） | 每轮 subs 1 + bgt 2 周期 |
| Halt 函数体 | `0x114C cpsr=8000001F`（542 次） | 同地址同 CPSR |
| IRQ 打断点 | 596 次进 `0x18→0x1FB0→0x1FBC`，其中 **543 次 lr=0x115C**（= Halt 暂停点 0x1158） | 首个 IRQ 即 `pc=0000115C cpsr=2000001F`，帧落在同一 IRQ 栈 |
| SoftReset | 首 4000 条低地址事件中 0 次 | 仅在游戏自行恢复 BIOS 内部现场时按真机语义执行 |

**几个必须一起改的口径（否则栈帧/模式会对不上）**：

- **异常入口低 8 位**：SWI `0x93`、未定义 `0x9B`、中止 `0x97`、IRQ `0xD2`、
  FIQ `0xD1`（melonDS 口径）。共同点：**入口一律清 T**（向量表是 ARM 码；
  参考核实测 Thumb SWI 的入口 CPSR 是 `60000093` 而不是带 T 的值），
  这让「游戏把被打断现场存进任务上下文再恢复」的路径与参考一致。
- **HALTCNT（0x04000301，ARM7）**：0x80=Halt、0xC0=Sleep；`cpu_step` 消费暂停请求，
  唤醒条件与 melonDS `HaltInterrupted(1)` 一致——`(IF & IE) != 0`，不看 IME；
  唤醒后**先做 IRQ 检查**（真机被打断的是 BIOS 里的 0x1158，不是调用方代码）。
- **ARM7 WRAM 镜像**：ARM7 视角 0x03800000-0x03FFFFFF 以 64KB 步长镜像，
  FreeBIOS 的 handler 槽 `[0x04000000-4] = 0x03FFFFFC` 与游戏写的 0x0380FFFC
  是同一字节；软件中断标志 0x03FFFFF8 同理。
- **BIOS 可读字节影子**（`bios7_image.c`）：向量表 0x00-0x1F 与 SWI 表
  0x10B0-0x112B。游戏恢复「PC 停在 SWI 向量」的现场后，分发器会用
  `ldrb r12,[lr,#-2]` 从 BIOS 里取编号；本地低地址原本读 0，会拿到
  SoftReset(0) 而不是参考的字节，随后栈帧错位跑飞。
- **直接启动寄存器初值**（`cpu_direct_boot`，melonDS `SetupDirectBoot`）：
  ARM7 sp=0x0380FD80 / sp_irq=0x0380FF80 / sp_svc=0x0380FFC0，
  ARM9 sp=0x03002F7C / sp_irq=0x03003F80 / sp_svc=0x03003FC0。
  没有 SVC 栈的话，BIOS 分发器的压帧会写到栈外。

**已知偏差（有意保留）**：① SWI 0x1F 在 FreeBIOS 表外会读成代码字，本地按真机
BIOS 口径接到 CustomHaltPost(0x1FA4)；② 低地址只影射向量表/SWI 表，
其余读 0；③ ARM9 的 SWI 仍走阶段 11/12 的直接 HLE（本次只改 ARM7）。

**验证**：`[case 21-B9wt]` 两组共 30 项断言（SWI 分发器栈帧/模式/返回、
Halt 暂停 + IRQ 优先打断顺序 + 0x1FB0/0x1FC0 帧），全量 **860 项检查 0 失败**；
真 ROM 900 帧 ARM7 稳定停在 BIOS Halt `0x1158 cpsr=8000001F`，标题段
（f=300 `disp=00161F10`）与 2600 帧终点（ARM9=0200957C）不回归。

## 2026-09-12 · 21-B9yd — 完整 FreeBIOS 镜像 + 未建模函数体真地址执行

**动机**：B9wt 的「已知偏差 ②」只影子化向量表与 SWI 表，其余低地址读 0；
未被逐条建模的 SWI 函数体则是在分发器里调 C 版 HLE 算完直接跳 `swi_complete`
——结果对，但 BIOS 内部的 PC/lr/栈帧/周期轨迹与参考核不同，IRQ 落在函数体
内部时保存的现场也就不一样。

**改动**：

- 新增 `src/bios/bios7_rom.h`（由 `build/gen_bios7_rom.py` 生成，源是 melonDS
  同一份 BSD-2 许可的 `bios_ntr_arm7`）：完整 0x4000 字节镜像，有效 0x2030，
  其余补 0；`bios7_image.c` 改为整段提供，不再只影子向量表/SWI 表。
- `bios7_low.c` 分发器：未建模的函数体**在真地址上执行真实字节**
  （`0x11E4` Div、`0x1240` CpuSet、`0x134C` GetCRC16、`0x1CA0` GetPitchTable、
  `0x1FC8` GetVolumeTable、`0x1488` LZ77…），C 版 HLE 只在镜像缺失时兜底。
- 新增统计 `bios7_low_stats_t`（每个 SWI 号的调用次数、真地址执行次数、
  HLE 兜底次数、未建模取指次数）与 `bios7_low_reset_stats/get_stats`。

**实证（本步顺带纠正了两条测试期望的口径）**：真实执行后
`[case 21-B9a]` 的 ARM7 分支给出 **0x37DD**（8 字节 "12345678"），
正好等于独立实现的 CRC-16/ARC——说明本地是把真实字节码算对了；
而 FreeBIOS 的 ARM7 实现按**半字**推进（`lsrs r2,r2,#1` → ⌊n/2⌋ 轮），
奇数字节长度的尾巴会被忽略（真机 BIOS 支持任意长度，属 FreeBIOS 差异）。
`SWI 0x08 SoundBias` 的真字节码也**不看 r0**：当前值非 0 就写回 0x200，
与旧 C 版 HLE（按 r0 写电平）语义不同 ⇒ 测试改为断言 FreeBIOS/参考核口径。

## 2026-09-12 · 21-B9ye — 分发器逐条化 + 与参考核逐地址对照（93/93）

**动机**：B9yd 之后低地址行为已经「真地址执行」，但覆盖范围没有证据。
给两边都加**低地址取指直方图**（本地 `bios7_low_dump_hist`，`NDS_BIOS7_HIST=1`
开启、`NDS_BIOS7_HIST_OUT=<path>` 另存 4096×u32；参考核在同帧数下 dump
`hist7: pc=… count=…`），跑同样帧数后逐地址比对。

**首次对照（6000 帧）发现两处差异**：

1. 本地缺 **0x1084-0x10A8**（分发器中间 10 条）——旧实现把 0x1080 整段
   合成一步执行，PC 直接从 0x1080 跳到函数体。差别只在「IRQ 落在分发器
   中间」时可见：0x1098 的 `msr` 之后 I 位取自调用方 SPSR，若调用方允许
   IRQ，参考核能在 0x109C-0x10A8 之间插入中断，保存的现场 PC 就是这些地址。
2. 其余 83 个地址两边完全一致（含 GetCRC16 的 112 次循环、CpuSet 单次调用、
   Halt 体 7.3 万次、IRQ 出入口 0x1FB0-0x1FC4、音频查表 0x1CA0/0x1FC8）。

**改动**：把分发器拆成 11 条独立指令等价（0x1080 push{r4,r12,lr} → 0x1084
`mrs r4,spsr` → 0x1088 push{r4} → 0x108C `and #0x80` → 0x1090 `orr #0x1f` →
0x1094 `ldrb r12,[lr,#-2]` → 0x1098 `msr cpsr_fc`（切 System、I 取自 SPSR）→
0x109C push{lr} → 0x10A0 `cmp r12,#0x20` → 0x10A4 `movge r12,#1` →
0x10A8 `ldr pc,[pc,r12,lsl#2]`），每条给 1-4 周期。

**对照结果（900 帧，本地 vs melonDS+FreeBIOS）**：

```
distinct ref=93 loc=93   (集合完全相同：only-ref/only-loc 均为空)
0x18 IRQ 向量   ref=11237  loc=11267  (+0.3%)
0x1FB0-0x1FC4   ref=11237  loc=11267  (+0.3%)
0x11xx Halt 体  ref=10592  loc=10889  (+2.8%)
0x1368 CRC 循环 ref=112    loc=112    (完全一致)
0x08  SWI 向量  ref=39823  loc=42890  (+7.7%)  ← 余下差异见下
```

**余下的已知差异（不是低地址路径问题）**：本地 ARM7 的 SWI 调用次数略多，
且随时间累积（900 帧 +8%、6000 帧 +33%），来源几乎全是
`0x1CA0 GetPitchTable` / `0x1FC8 GetVolumeTable` 这一对音频查表
（6000 帧：本地 235,937 次 vs 参考 168,298 次）。这是游戏侧「声音/音乐
节拍」跑得比参考快，属于定时器/声音推进口径的分歧，下一步据此排查。

## 2026-09-13 · 21-B9yi（续94）：`CpuSet/CpuFastSet` 规模统计 —— **重帧不是 HLE 拷贝造成的**（负结果）

- **动机**：续93/93b 发现重场景里有 25~27 ms 的突发帧（其中**模拟占 20~24 ms**，典型帧只有 7.8 ms）。
  怀疑对象之一是 BIOS HLE 的大块内存拷贝：本地 `bios_cpuset`/`bios_cpufastset` 是
  **逐字调用 `bus_read32`/`bus_write32`**（每个字都走一遍总线分发），
  若游戏经常整块搬几十 KB，就会出现这类尖峰。
- **做了什么**：给两个 HLE 函数加调用次数与字数统计，跑完打印一行：
  `biosmem: CpuSet N 次 / M 字，CpuFastSet N 次 / M 字`（已入库，作为常驻诊断）。
- **实测（9000 帧、固定脚本）**：
  ```
  biosmem: CpuSet 1 次 / 1 字，CpuFastSet 0 次 / 0 字
  ```
  ⇒ 游戏**几乎不用**这两个 SWI（它自己做拷贝循环）。
- **结论**：❌ 排除该怀疑。重帧是**游戏自己的 CPU 循环**（解包/构建显示列表等）造成的，
  要进一步提速只能靠更快的 CPU 核心（JIT/重编译器），不属于本轮范围。
  保留统计诊断（很便宜，将来任何 ROM 出现「拷贝型尖峰」都能一眼看出）。
