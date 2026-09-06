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
