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
