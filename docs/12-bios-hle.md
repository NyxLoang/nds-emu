# 阶段 11 短文：BIOS / SWI / HLE

阶段 10 已经把 ARM 指令集补到「真实机器码能跑起来」的程度，其中 `SWI` 只是**记录编号、
不跳异常**。阶段 11 要接上后半段：识别 `SWI <编号>` 后，用 C 直接实现对应系统函数，
而不去逐字节执行 NDS 的 BIOS ROM。这就是 **HLE（High-Level Emulation，高层仿真）**。

## 1. BIOS 是什么

NDS 主机内部有两颗 CPU，各自有一块内置只读存储器 **BIOS**：

| 芯片 | BIOS | 大小 | 作用 |
|------|------|------|------|
| ARM9 BIOS | `0xFFFF0000` 起 | 32 KB | 启动引导、解密、HLE 系统函数 |
| ARM7 BIOS | `0x00000000` 起 | 16 KB | 启动引导、系统函数（含部分音效相关） |

BIOS 里的代码，除了开机的安全引导（加密校验、内存初始化）外，大部分是一堆**系统服务函数**：
除法、开平方、内存拷贝、LZ77/RL/Huffman 解压、等待中断等。游戏不自己写这些底层函数，
而是通过一条指令 `SWI` 去「呼叫」BIOS。

## 2. SWI 是什么

`SWI`（Software Interrupt，软件中断）是 ARM 的一条指令，格式：

```
SWI <24 位注释字段>
```

- **THUMB 状态**：`swi 0x09`，注释字段直接就是函数号 `9`。
- **ARM 状态**：`swi 0x090000`，函数号被左移 16 位放进注释字段（`9 << 16 = 0x090000`）。

我们阶段 10.8 的 `exec.c` 已经做了第一步：把 24 位注释字段存进 `cpu->swi_num`。
因此：

```
函数号 n = cpu->swi_num >> 16        （ARM 状态）
```

> 本阶段只处理 ARM 状态（Thumb 在 3b.11 暂缓），所以统一用 `swi_num >> 16` 取号。

## 3. HLE 是什么

真实主机上，`SWI` 会触发异常，CPU 跳进 BIOS 里对应的 handler 执行。模拟器有两条路：

1. **LLE（Low-Level Emulation）**：把真 BIOS ROM 的机器码加载进来，逐条执行。  
   缺点：需要合法持有 BIOS 镜像，且启动引导那段加密/硬件初始化很难模拟。
2. **HLE（High-Level Emulation）**：看到 `SWI <n>` 后，**不执行 BIOS ROM**，直接调用
   自己用 C 写好的同名函数，按约定读/写寄存器，然后 `PC += 4` 返回。

阶段 11 走 **HLE** 路线：新建 `src/bios/` 模块，提供 `bios_dispatch(n, cpu)`，把函数号
路由到对应实现。这样既不依赖 BIOS 镜像，又让真正的 `.nds` 代码能调用系统服务。

## 4. NDS BIOS SWI 编号表

> 注意：NDS 与 GBA 的编号**不一样**（例如 `Div` 在 GBA 是 `0x06`，在 NDS 是 `0x09`）。
> 下面以 **NDS9（ARM9，游戏主逻辑跑在这颗核上）** 为主列出常用函数；最后一列标注 NDS7
> 是否相同。完整表见 GBATEK「BIOS Function Summary」。

| 函数号 | 名称 | 作用 | NDS7 |
|--------|------|------|------|
| 0x00 | SoftReset | 软复位，跳回启动向量 | 同 |
| 0x01 | RegisterRamReset | 复位指定 RAM/IO 区域 | 同 |
| 0x03 | WaitByLoop | 忙等循环延时 | 同 |
| 0x04 | IntrWait | 关中断并等待指定 IRQ 置位 | 同 |
| 0x05 | VBlankIntrWait | 等 VBlank 中断 | 同 |
| 0x06 | Halt | 停止 CPU 直到中断 | 同 |
| 0x09 | Div | 有符号除法（商/余/绝对值商） | 同 |
| 0x0B | CpuSet | 内存搬移/填充（可半字） | 同 |
| 0x0C | CpuFastSet | 快速 32 位搬移/填充 | 同 |
| 0x0D | Sqrt | 无符号开平方（整数） | 同 |
| 0x0E | GetCRC16 | 算 CRC16 校验 | 同 |
| 0x10 | BitUnPack | 位流解包 | 同 |
| 0x11 | LZ77UnComp (Wram) | LZ77 解压，8 位写 | 同 |
| 0x12 | LZ77UnComp (Vram/回调) | LZ77 解压，16 位写 | 同 |
| 0x13 | HuffUnComp | Huffman 解压 | 同 |
| 0x14 | RLUnComp (Wram) | RLE 解压，8 位写 | 同 |
| 0x15 | RLUnComp (Vram/回调) | RLE 解压，16 位写 | 同 |
| 0x08 | SoundBias | 音频偏置 | 仅 NDS7 |
| 0x16 | Diff8bitUnFilter | 8 位差分滤波还原 | 仅 NDS7 |
| 0x18 | Diff16bitUnFilter | 16 位差分滤波还原 | 仅 NDS7 |

> `DivArm`（GBA 的 `0x07`）在 NDS 上**已移除**，无需实现。
> 阶段 11 只需覆盖表里「本阶段要做的」那几项：除法/开方（0x09/0x0D）、搬移（0x0B/0x0C）、
> 解压（0x10–0x15）、等待（0x03/0x04/0x05/0x06）。

## 5. 本阶段要实现的函数与寄存器约定

BIOS 函数按 ARM 调用约定：`r0–r3` 传参，返回多在 `r0/r1`（其余寄存器保持不变）。

- **Div（0x09）**：`r0 = 被除数（有符号）`，`r1 = 除数（有符号）`；
  返回 `r0 = 商`、`r1 = 余数`、`r3 = |商|`。
- **Sqrt（0x0D）**：`r0 = 无符号 32 位`；返回 `r0 = 无符号 16 位整数开方`。
- **CpuSet（0x0B）/ CpuFastSet（0x0C）**：`r0 = 源地址`、`r1 = 目的地址`、`r2 = 控制字
  （含长度、模式、传送单位）`。CpuSet 可按 16/32 位且可「固定源填充」；CpuFastSet 固定
  32 位四字对齐。
- **LZ77/RL/Huffman 解压（0x10–0x15）**：`r0 = 源（含 32 位头，头里记录解压后大小）`、
  `r1 = 目的地址`；解压后 `r0 = 源末尾地址`。
- **Halt / IntrWait / VBlankIntrWait（0x06/0x04/0x05）**：让 CPU 暂停/等待中断；
  `IntrWait/VBlankIntrWait` 以 `r0 = 掩码` 为参数，等对应 IRQ 置位后返回。

> 各函数实现前会先查 GBATEK 的精确寄存器/边界定义（尤其除 0、Halt 与中断的交互），
> 再写代码与单测。

## 6. 与我们模拟器的衔接

1. `cpu_step` 检测到 SWI 时（或 `exec_step` 内），调 `bios_dispatch(swi_num >> 16, cpu)`。
2. 已知函数：按约定读写 `cpu->r[]`，返回后 `PC += 4`（SWI 已 `PC += 4`，HLE 不再加）。
3. 未知函数号：打印一条日志、不崩溃、`PC += 4` 继续（阶段 11.2 建立这个框架）。
4. 真正的异常向量跳转（SWI → 0x08 向量）留到阶段 12；本阶段 HLE 在向量之前拦截。

---

**结论**：阶段 11 的目标是**让真正的 `.nds` 代码能调用 BIOS 系统服务**——用 HLE 方式，
新建 `src/bios/` 模块，`SWI <n>` → `bios_dispatch(n, regs)`，先搭分发框架（11.2），
再逐个实现除法/开方（11.3）、搬移（11.4）、解压（11.5）、等待（11.6），最后综合验收（11.7）。

## 7. 补充：ARM7 低地址路径的「参考级 HLE」（21-B9ws）

上面的 `bios_dispatch` 是「在 SWI 指令处直接算结果」的简化 HLE，对纯函数
（除法/开方/解压/查表）足够；但**等待与调度类**（Halt/IntrWait/WaitByLoop）
不行：真机这些功能是 BIOS 低地址里的机器码，游戏侧（尤其是把被打断现场当
任务上下文保存的调度器）能看到 BIOS 内部的 PC、栈帧与模式。

21-B9ws 起 ARM7 走「按地址等价执行」的路线（`src/bios/bios7_low.c`）：

- SWI 先做真异常（SVC、lr=返回地址、CPSR 低 8 位=0x93）→ 0x08 向量 → 0x1080
  分发器（SVC 栈压 r4/r12/lr/SPSR、切 System、压 System lr、查 0x10B0 表）；
- 函数体按 FreeBIOS 镜像逐条等价：Halt 写 HALTCNT 后暂停在 0x1158、
  WaitByLoop 逐轮 subs/bgt、IntrWait 轮询软件中断标志 0x03FFFFF8；
- `swi_complete`（0x112C）恢复 System lr、切回 SVC、`msr SPSR`、`movs pc,lr`；
- IRQ 走 0x18 → 0x1FB0（六字帧 + lr=0x1FC0）→ `[0x03FFFFFC]` 用户 handler →
  0x1FC0 弹帧 → `subs pc,lr,#4`；
- 低地址「可读字节」（向量表/SWI 表）由 `bios7_image.c` 提供影子，
  这样「游戏恢复 BIOS 内部现场」后分发器读到的编号与参考核一致。

ARM9 仍保留第 6 节的直接 HLE（其 IRQ 尾部在 21-B9wa 已按 FreeBIOS 0xFFFF06F0 等价）。
