# 阶段 21：真实 .nds 兼容性 bring-up（Phase A 总结 + Phase B 路线）

> 阶段 21 的目标不是再实现一整块新硬件，而是让前面 0–20 阶段攒出来的模拟器
> **真正跑起来一个商业 ROM**（本项目验收对象：`tools/` 下的 FFXII）。这个阶段没有
> 一条“从头写到尾”的实现主线，而是一轮一轮地：跑真 ROM → 找到第一个卡点 →
> 修掉 → 再跑 → 再找下一个。这种调试方式叫 **bring-up**（逐段点亮）。
> 本文先总结已完成的 Phase A（bring-up 设施 + 首份 gap 清单），再说明 Phase B 的迭代路线。

---

## 1. 为什么真实 ROM 值得单独开一个阶段

之前的每个阶段都用「手写机器码 / 自制 mini.nds」验证：程序已知、数据已知，预期也已知。
真实商业 ROM 完全不同：

- 代码体积大（FFXII 的 ARM9 镜像 0x78038 字节 ≈ 480KB，ARM7 约 162KB）；
- 启动过程依赖很多**之前从未一起工作的子系统**（双核、CP15、中断、卡带、显示、内存镜像）；
- 某个环节差一点，后面的代码就会在错误数据上继续跑，现象往往不是“报错”，而是
  PC 慢慢滑进未映射内存、画面全黑、或者卡在一个看起来很像死循环的地方。

所以阶段 21 需要先有**诊断设施**：不开窗口也能跑 N 步、能逐条看指令、能把“未知寄存器/
未知 SWI/未实现指令”只打一次而不是刷屏。Phase A 就是建这套设施并跑出第一份卡点清单。

---

## 2. Phase A 做了什么

### 21-A1：修 ARM7 镜像装载

早期装载 ARM7 镜像时按“固定 64KB ARM7 WRAM（0x03800000）”判断，但 FFXII 的 ARM7
入口和装载目标其实是 **Main RAM 0x02380000**（165552 字节）。

修复：装载时按 ROM 头的 `ram` 字段判断目标区——落在 Main RAM（4MB）就写 Main RAM，
落在 ARM7 WRAM 就写 WRAM，不再假设固定 64KB。

### 21-A2：bring-up 诊断设施

新增：

- `bus.diag` 开关：未知 SWI / 未实现指令 / 未知 IO 访问**只打印一次**（位图去重），
  避免游戏循环轮询同一寄存器时刷屏；
- `--headless N`：不开窗口、不开音频，跑 N 步后打印两核 PC / cycles 并退出；
- `--trace`：逐条打印指令，用于追启动前几十步或定位跳转点；
- `src/runner/` 模块：headless 驱动（ARM9:ARM7 = 2:1 与主循环一致）。

### 21-A3：修 ARM r15 读值缺 +8

ARM 架构里读 `r15` 当普通寄存器用时应得到「当前指令地址 + 8」（流水线），
PC 相对的字面量池（`LDR rX, [PC, #imm]`）依赖这个值。原来直接返回 `r[15]`，
导致真 ROM 启动时 PC 相对读数差 8 字节。修复后统一走 `read_reg`：

```c
r15 读值 = (当前 PC & ~3) + 8
```

### Phase A 产物：首份 gap 清单

| # | gap | 状态 |
|---|-----|------|
| G1 | ARM r15 读缺 +8，PC 相对字面量池读错 | ✅ 已修 |
| G2 | Shared WRAM（0x03000000-0x03007FFF）及镜像（0x037F8000 一带）未映射 | ✅ 已修（21-B1，2026-09-05） |
| G3 | 双核启动握手时序（ARM7 挂起/释放 + IPC FIFO 在真实 ROM 上验证） | ✅ 已过旧卡点（21-B2..B8：IPCSYNC/镜像/BX/Thumb/WRAMCNT + ARM9 DTCM/ITCM + 信箱解码；2026-09-05 起 ARM9 进 boot 初始化，下个 gap 见 G4/B9） |
| G4 | 修完 G2/G3 后继续暴露的更多 gap（SWI/PPU/中断/内存等） | ⏳ Phase B（B9 起：ARM9 BIOS/IRQ 高向量 + ARM7 SWI 0x0E CRC16 + 一批未知 IO） |

---

## 3. 跑真 ROM 时看到的现象（读代码时可对照）

这是 Phase A 版本的现象记录，后面每个微步都在改写：

```sh
build/nds-emu.exe "tools/Z 最终幻想12…(1024Mb).nds" --headless 20000000
```

会看到类似：

```
headless: step=1048576 ARM9 PC=0022F004 cyc=699051 | ARM7 PC=037FFEAC cyc=349525
...
headless: done. ARM9 PC=132612F8 cyc=13333334 | ARM7 PC=037FFC44 cyc=6666666
```

两个特征值得记住：

1. **ARM7 的 PC 落在 0x037F8xxx**：这是 Shared WRAM 的镜像区，当时 bus 没映射，
   读返回 0。0x00000000 恰好是一条合法但无意义的指令（`ANDEQ r0,r0,r0`），
   所以 CPU 不报错，只是一直“取 0 → 前进 4”，PC 从低地址往高地址漂。
2. **ARM9 的 PC 也在按 4 字节递增漂移**：说明某条跳转把执行流送进了没有代码/没有
   映射的地址区间，模拟器既没有崩也没有报未实现指令。

诊断口诀：真 ROM “跑飞”时先看两核 PC 落在哪个区间，再往回找最后一次合理的分支/BL。

### 21-B2 之后（2026-09-05）：trace 定位两个具体跑飞点

修完 IPCSYNC 后 headless 不再报 `0x04000180/81` 未知 IO，但两核终点不变。逐条 trace
定位到两个明确原因：

1. **ARM9 在约 cycle 126,634 弹 PC=0**：调用 `0x0201A9B8` 时函数用
   `STMFD/LDMFD sp!, {r4,pc}` 保存/恢复返回地址，而 SP≈0x027E3Fxx。该地址落在
   Main RAM 的**无缓存镜像区 0x02400000-0x027FFFFF**（真机存在、当前 bus 未映射），
   栈写入丢失 → 弹回 0 → 在零区逐 4 漂移。
2. **ARM7 在约 cycle 231,180 起按 ARM 码跑飞**：`BX r12 -> 0x038043C9`（LSB=1，
   真机应切 Thumb 并把 PC 对齐到 0x038043C8）没有更新 T 位，CPU 把 0x038044xx 的
   Thumb 数据当 ARM 指令译码，最终 `B 0x049E07B5` 离开可执行区。

这两个点成为 B3/B4 的直接依据；每修一个就重跑一次，直到 hard_title。

### 21-B3 之后（2026-09-05）：ARM9 卡点解除

把 0x02400000-0x027FFFFF 映射为 Main RAM 别名后，ARM9 的栈读写不再丢失。trace 中
cycle 126,634 的返回点变为：

```c
cpu: PC=020008F8 insn=E8BD8010 LDM r13!, list=8010 n=2 cycles=126634
```

`headless 20000000` 全程 ARM9 PC 停在主存代码区 0x0200B9xx 循环（修复前是在 0 区
逐 4 漂移、终点 0x132612F8）。ARM7 仍按原轨迹跑飞（BX 奇地址未切 Thumb，B4），
ARM9 的循环疑似在等 ARM7 握手结果——B4 修完 ARM7 后重跑即可验证。

### 21-B4 之后（2026-09-05）：ARM7 进入 Thumb，但段内二次跑飞

ARM BX 切 Thumb 后，ARM7 能正确落到 0x038043C8（旧路径里它把这里当 ARM 数据译码、
跳到 0x049E07B5 的跑飞消失）。但 headless 随即出现约 65k 行
`io: read unknown addr=0400xxxx (arm7=1)` 顺序刷屏——ARM7 从 IO 区 0x04000182 附近
开始逐字节读取/取指漂移，20M 步终点为 0x0178CBD8（仍是跑飞，只是换了一条路径）。
普通 Thumb 指令当前不打印 trace，无法直接看到跳进 IO 区前的最后几条指令，
因此 B5 第一步补 Thumb 逐条 trace，再定位根因。

### 21-B5 之后（2026-09-05）：BX lr 解码错误修复，卡点前移

Thumb 逐条 trace 直接抓到二次跑飞源头：0x038043C8 只有两条 Thumb 指令
`SWI 3` + `BX lr`（0x4770）。旧解码把 BX/BLX 的 Rm 从 bit2:0+bit6 拼（0x4770→r8），
而规范编码是 bit6:3（0x4770→lr），于是“返回”跳进了 r8 里的 0x04000180，随后逐字
刷读 IO 区。修复 Rm 解码后 IO 刷屏归零，ARM7 正确反复进出该桩并轮询 0x027FFFF0。
卡点前移到 ARM9：约 cycle 463,554，ARM9 在 0x0200B9B0
`LDMFD sp!, {r4,r5,lr}` 后 `BX r14 -> 0xE1C010B0`——栈里保存的返回地址被污染，
B6 将定位污染来源。

### 21-B6 之后（2026-09-05）：栈污染来自 ARM7 误写 Main RAM 镜像

给 LDM trace 增加 SP/弹出 PC 后定位 LR 槽 0x027E3B34；总线写监视抓到写入者是
**ARM7 的块拷贝循环（0x02380124-130）**，它在约 cycle 141,868 把 0xE1C010B0 写进
该槽。根因：0x02400000-0x027FFFFF 是 **ARM9 专用的无缓存镜像**（ARM7 内存图没有
这段），B3 无差别映射让 ARM7 的拷贝真实落盘、覆盖了 ARM9 的栈。改为仅 ARM9 可见后，
ARM9 全程停在主存代码区不再崩栈；ARM7 停在 0x037FC0xx 轮询循环。
新出现的 3 条 ARM9 未知 IO（读 0x04000204/205、写 0x04000247=03）与 ARM7 轮询
循环一起成为 B7 素材。

### 21-B7 之后（2026-09-05）：EXMEMCNT/WRAMCNT 语义补齐，握手卡点前移 B8

给 bus 补齐真实 Shared WRAM 所有权之后，用 `--headless 3000000` 重跑 FFXII：

```text
headless: step=1048576 ARM9 PC=0200B844 cyc=699051 | ARM7 PC=037FC0B0 cyc=349525
headless: done. ARM9 PC=0200B840 cyc=2000000 | ARM7 PC=037FC0B0 cyc=1000000
```

1. `io: read unknown 04000204/205` 与 `io: write unknown 04000247` 三条全部消失；
   寄存器初值/读写位与 melonDS 直接启动口径一致（WRAMCNT=3、EXMEMCNT=0xE880）。
2. trace 显示 IPCSYNC 上的 8→0 计数握手已经能跑完（ARM9 走完 0x0200B94C 附近
   的轮询并正常返回），随后 ARM9 进入 0x0200B834 的事件位轮询：按 `r1` 选
   `0x027FFC00 + r1*4`、再读 `+0x38C` 处字并测试 bit12；ARM7 停在 0x037FC0A0/B0
   轮询 `0x027FFFF0`。ARM9 在等的那一位（0x027FFF8C bit12）始终未被 ARM7 置上。

结论：B7 已把未知 IO 清零，双核当前卡在「ARM7 命令口 0x027FFFF0 → ARM9 事件
状态区 0x027FFF8C」这条数据约定上，排入 B8 继续解码。

### 21-B8 之后（2026-09-05）：根因是 ARM9 DTCM/ITCM，不是“镜像仅 ARM9 可见”

解码 ARM7/ARM9 两侧代码后确认：B7 看到的两个轮询（ARM7 等 `0x027FFFBC==0x7F`、
ARM9 等 `0x027FFF88/8C` 事件位）都是**自研信箱协议的正常握手**，不是模拟器缺
寄存器。真正拦住握手的是内存归属：

1. **0x027E0000-0x027E3FFF 是 ARM9 的 DTCM**。FFXII 复位例程 `0x02000A6C` 用
   `MCR p15,0,rX,c9,c1,0`（值 0x027E000A）配置 16KB DTCM 到 0x027E0000，再把 SVC/
   IRQ/User 栈建在这里。ARM7 同时把自己的镜像搬到主存 0x027E0000 起 0x182A0 字节；
   ARM7 看不到 DTCM，写的是同一地址下的 Main RAM（真机按 0x02000000-0x02FFFFFF
   窗口解码，melonDS 的 ARM7 也命中两个 8MB 窗口）。B6 把镜像“仅 ARM9 可见”其实是
   掩盖了“DTCM 未实现”，ARM9 栈被写坏是因为它被错误地放在 Main RAM 里。
2. **0x01FF8000 起 32KB 是 ARM9 的 ITCM**。复位代码 `0x020009F0` 用尾部描述表
   `0x02078020`（目标 0x01FF8000、大小 0x6BC0；目标 0x027E0000、大小 0x1040）把
   ARM9 镜像 0x02070420 起的系统例程拷进 ITCM/DTCM，随后清掉源区。ITCM 未映射时
   ARM9 第一次 `BL 0x01FF81B4` 就取到 0，从 ITCM 零区一路漂进主存。
3. 0x027FFxxx 的主存高 4KB 就是共享信箱区：ARM7 拷贝后按
   `0x027FFFF0→0x027FFFB8/BC→0x027FFFBE` 的锁存/握手顺序运行，ARM9 事件位在
   `0x027FFC00+0x388` 起（事件 n 的 bitn），不再卡死。

**做了什么**：

- `bus` 新增 ARM9 DTCM（16KB，CP15 可配基址）与 ARM9 ITCM（32KB，固定
  0x01FF8000）；ARM9 优先命中 DTCM，ARM7 仍访问同地址的 Main RAM 镜像。
- `exec_coprocessor` 按子寄存器保存 CP15 c9,c1,0/1，c1 bit16 使能后把 DTCM 映射
  同步给 bus；ARM9 MRC c9 读回同一配置。
- Main RAM 镜像恢复双核可访问（melonDS 口径），撤掉 B6 的“仅 ARM9”限制并修正
  bus.h/buslog 里的错误结论；`direct-boot tables` 按 melonDS SetupDirectBoot 写
  0x027FFxxx 卡带信息表。
- 测试新增 `[case 21-B8]`：DTCM 与镜像互不覆盖、ARM7 在 DTCM 地址下写主存、ITCM
  仅 ARM9 可访问。全量 **594 项检查 0 失败**。

重跑 FFXII `--headless 2000000`：双核不再停在旧信箱轮询；ARM9 跑到 boot 初始化
0x02009EC0（打开中断后被 IRQ 打断），ARM7 首次调 `SWI 0x0E`（GetCRC16）没有
HLE 实现。下一卡点是 **ARM9 高向量 0xFFFF0000 的 BIOS/IRQ 分发 + ARM7 SWI 0x0E**，
排入 B9。

### 21-B9a 之后（2026-09-05）：GetCRC16 落地，ARM7 越过 SWI 0x0E

**做了什么**：

- 新增 `src/bios/bios_crc16.h/.c` 并登记 SWI 0x0E：`r0=CRC 初值`、`r1=数据地址`、
  `r2=字节数`，返回 `r0=CRC16`。算法为标准 CRC-16/IBM：初值由调用方给出（FFXII
  用 0xFFFF），反射多项式 0xA001 逐位右移，末尾不反转，与 GBATEK 伪代码/libnds
  `swiCRC16` 的用法一致。
- 测试新增 `[case 21-B9a]`：ASCII `"123456789"` + 初值 0xFFFF → 0x4B37；覆盖
  ARM、Thumb、ARM7 三条路径与长度 0 返回初值。全量 **600 项检查 0 失败**。

重跑 FFXII `--headless 2000000`：

```text
headless: step=1048576 ARM9 PC=02009EC0 cyc=699051 | ARM7 PC=037FC89C cyc=349525
headless: done. ARM9 PC=0025B6C0 cyc=1333334 | ARM7 PC=037FC898 cyc=666666
```

1. `bios: unknown SWI 0x0E` 消失，ARM7 首次 CRC 调用点通过并进入 0x037FC8xx
   的后续 boot 代码（旧终点 0x000366A4 不再出现）。
2. ARM9 仍停在 0x02009EC0 附近被 IRQ 打断后进 0xFFFF0000 高向量区读 0 跑飞
   （终点 0x0025B6C0），需要查高向量跳板/中断时机。
3. 日志露出大量 ARM9 未知 IO：0x04000304/305、0x04000240-249、
   0x04000004/05、0x0400004C-5F、0x04001004-6F，另有 ARM7 读 0x04000300/301；
   这些是下一 gap 的素材（PowerControl/PostFlag/中断控制/显示控制等）。

结论：B9a 后下一卡点排为 **ARM9 BIOS/IRQ 高向量 + 这批未知 IO 桩**，按 gap 继续。

### 21-B9b（2026-09-05）：首中断现场快照——确认 FFXII 用“DTCM 槽”装 IRQ handler

**做了什么**：

- `cpu.c`：IRQ 真正触发时（`--headless` 诊断模式）只打印一次首中断现场：核名、
  被打断 PC、CPSR、IME/IE/IF，以及 IRQ 槽末 8 字节（ARM9：DTCM+0x3FF8/FC，
  基址取 bus 的 CP15 配置；ARM7：0x03FFFFF8/FC）。用于判断游戏把 IRQ handler
  装到哪个约定槽，避免每步刷屏。
- `exec.c`：异常 trace 加 `arm9`/`arm7` 前缀，中断后执行流归属一眼可辨。

重跑 FFXII `--headless 1100000` 的关键输出：

```text
irq: first arm9 IRQ pc=02009EC0 cpsr=8000001F ime=04000001 ie=00042009 ifl=00000008
     slot+3FF8=00000000 slot+3FFC=01FF8000
headless: done. ARM9 PC=000117C0 ...
```

结论：

1. 游戏遵守 BIOS 槽协议：ARM9 的 DTCM 末 4 字节（0x027E3FFC）放着用户 IRQ
   handler 指针 **0x01FF8000**（ITCM 开头，复位时拷入的系统例程区）；
   0x3FF8 的等待标志为 0（本次不是等待被唤醒）。
2. ARM9 使能了 VBlank（bit3）与 FIFO 相关中断（bit17/18），首中断是 VBlank。
3. 模拟器没有 BIOS ROM，无法执行 0xFFFF0018 的跳板，于是从高向量读 0 一路漂到
   0x000117C0。B9c 将让 IRQ 入口模拟该跳板：保存异常现场后直接跳槽里的 handler。

### 21-B9c（2026-09-05）：ARM9 IRQ 槽跳板——FFXII 中断分发器被点亮

**做了什么**：

- `cpu.c`：IRQ 异常现场建好后，若 ARM9 DTCM 已使能且 `DTCM+0x3FFC` 槽非 0，
  直接改 PC 跳槽里的用户 handler（LSB=1 时按 Thumb 入口置 T）。槽为 0 或 DTCM
  未配置时保留旧的高向量路径便于诊断。ARM7 的 0x03FFFFFC 约定待出现真实 IRQ
  证据后再对称实现。
- 测试新增 `[case 21-B9c]`：槽指向 ITCM，handler 为 `SUBS pc,lr,#4`，验证
  触发后 PC/mode/I/SPSR/LR 与返回恢复，共 8 项检查（全量 **608 项 0 失败**）。

重跑 FFXII：ARM9 终点从 0x000117C0（高向量读 0 漂移）变为 0x01FF8028——
**成功进入 ITCM 里的 FFXII 中断分发器**。对 ITCM 0x01FF8000 起做反汇编：

```text
stmdb sp!, {lr}          ; 保存返回
mov ip, #0x04000000
add ip, ip, #0x210       ; ip = 中断寄存器区
ldr r1, [ip, #-8]        ; IME
cmp r1, #0 / ldmeq ...   ; IME=0 直接返回
ldm ip, {r1, r2}         ; r1=IE, r2=IF
ands r1, r1, r2          ; pending = IE & IF
ldmeq sp!, {pc}          ; 无 pending 返回
mov r3, #0x80000000
clz r0, r1               ; ← 模拟器未实现 CLZ，误译导致死循环
bics r1, r1, r3, lsr r0  ; 清掉当前最高位
bne 循环                 ; 逐个分发所有挂起中断
...
```

结论：跳板已通，下一卡点（B9d）是 **CLZ 指令缺失**——不是 IO/时序问题。

### 21-B9d（2026-09-05）：CLZ 前导零计数——中断分发循环跑完

**做了什么**：

- `exec.c` 实现 ARMv5 `CLZ Rd, Rm`：数 Rm 的前导零（0→32）。FFXII 的 IRQ
  分发器流程是 `CLZ r0,r1` 找最高挂起位 → `BICS r1,r1,r3,lsr r0` 逐位清除 →
  分发对应处理函数；此前模拟器把 CLZ 误译成普通数据运算，r1 永远清不空，
  循环永不退出。
- 测试新增 `[case 21-B9d]`：0/最高位/最低位/FFXII IE 值/VBlank 位共 5 组
  CLZ 结果 + PC 前进断言（全量 **618 项 0 失败**）。

重跑 FFXII：IRQ 分发器处理完 VBlank 并返回主程序，ARM9 终点从
0x01FF8028 前进到 **0x02006488**；反汇编该处是 `blx r1`（ARM 态寄存器间接
调用），模拟器未实现 ARM `BLX Rm`，排入 B9e。

### 21-B9e（2026-09-05）：ARM BLX Rm——ARM9 回到主等待循环

**做了什么**：

- `exec.c` 补 `BLX Rm`（0x012FFF3x）：`r14 = PC+4` 后按目标 LSB 切换
  Thumb/ARM 并跳转（与 BX 相同的 T/PC 规则，额外保存返回地址）。
- 测试新增 `[case 21-B9e]`：ARM→Thumb 与 ARM→ARM 两条路径，各验证
  LR/T/PC 与目标函数首条指令执行（全量 **628 项 0 失败**）。

重跑 FFXII `--headless 8000000`：ARM9 越过 0x02006488，回到 0x02009EC0
主等待循环（PC 在 0x02009EC4-ECC 间游走，等下一次 VBlank），8M 步内不再有
固定单点卡死。下一步观察多次 VBlank 后启动流程是否继续前进。

### 21-B9f（2026-09-05）：IRQ HLE 桩——被中断现场保存/恢复

**做了什么**：

- 跳用户 IRQ handler 前，把 r0-r3/r12/CPSR/返回点（被打断 PC）存入模拟器
  私有槽；FFXII 分发器弹栈返回后，在取指前恢复现场、重执行被打断指令。
- 修复异常 trace 参数顺序 bug；headless 打印前若干次 IRQ 触发/恢复点。
- B9c 测试改为 FFXII 分发器风格（push lr → 破坏 r1 → pop pc），断言 r1/CPSR
  恢复、被打断指令重执行（全量 **633 项 0 失败**）。

真机：第一次 VBlank 完整处理链（含处理函数内嵌套的大块 memset）已执行完；
但第二次 IRQ 前 ARM9 落入未映射区 0x0027xxxx。反汇编启动序列确认 FFXII 分别
初始化了 SVC/IRQ/System 栈（0x027E3FC0 / 0x027E3F7C / 0x027E3AF8），
而模拟器所有模式共用 SP——IRQ handler 在 System 栈上压栈嵌套会覆盖用户现场。
**排 B9g：实现按模式独立的 r13/r14（banked registers）。**

### 21-B9g（2026-09-05）：模式私有 r13/r14——ARM9 停止跑飞

**做了什么**：

- CPU 增加 User/System 主 r13/r14 与 FIQ/IRQ/SVC/ABT/UND 私有 r13/r14 槽；
  新增 `exec_apply_cpsr` 统一处理“切模式时保存旧栈/加载新栈”。MSR、异常
  入口、异常返回、IRQ HLE 恢复全部走该入口。
- 启动代码中 FFXII 分别配置的 SVC/IRQ/System 三个栈真正隔离；B9c 用例先配置
  IRQ 专用 SP 再中断。新增 B9g 用例验证三模式往返栈值互不串扰
  （全量 **643 项 0 失败**）。

重跑 FFXII：ARM9 不再落未映射区，第一次 IRQ 后继续执行至 **0x02007CE0**
链表遍历（ARM7 在 0x037FC8A0 轮询配合）。日志新露出 ARM9 读
0x04000280-2BF 未知 IO——排 B9h 查这批寄存器（IPC/中断扩展区）。

### 21-B9h（2026-09-05）：LDM ^ 的 User 槽语义——任务队列不再成环

**做了什么**：
- 反汇编 + 运行时写监视确认 0x02007CCC 是 FFXII 协作调度器的“就绪队列删任务”函数；
  队列曾出现 A=0x02076FE4 ↔ B=0x02076F24 两节点互指成环，删除函数死转。
- 根因不是链表代码，而是 **LDM ^ 的 User 槽语义缺失**：上下文恢复
  `ldm r0,{r0-r14}^`（SVC 模式、列表不含 PC）应把 r13/r14 装入 User/System 槽，
  旧实现装进 SVC 私有槽，导致“切到 A 却还在跑旧现场”。
- 修复后队列恢复 A→B→NULL；第二次及以后的 VBlank IRQ 能正常进入/恢复。
- 顺带按硬件修正中断位：VBlank=bit0、Timer0-Timer3 溢出=bit3-bit6。

**验证**：新增 21-B9h 单测 6 项；全量 **649 项检查 0 失败**；headless 4M/8M：
ARM9 到达 0x0200EA84 忙等（0x02078F0A bit1 等清 0），Timer0 溢出已能触发
handler，下一步 B9i 追“忙位谁清”。

### 21-B9i（2026-09-05）：FIFO 命令送达 ARM7 + ARM7 IRQ 槽跳板

**做了什么**：
- 0x0200B780 反汇编确认是 **IPC FIFO 发送**（读写 0x04000184/188）；ARM9 发命令后
  置忙位并等清 0。此前 `cnt9/cnt7` 恒 0，命令被静默丢弃——根因是 CNT 高字节
  0xC4（错误应答+使能+收 IRQ）合并写被旧代码当成“纯错误应答”。
- ARM7 IRQ 首次真实触发后确认槽在 **0x0380FFFC**（0x03FFFFFC 是空的），指向
  0x037FB8F4 的中断分发代码；按 ARM9 相同 HLE 方案实现 ARM7 槽跳板。

**验证**：新增 B9i 单测 10 项；全量 **659 项检查 0 失败**；headless：ARM9 FIFO
命令入队并触发 ARM7 IRQ，ARM7 handler 能完整返回。ARM9 忙位仍未清，B9j 查
ARM7 回执路径。

### 21-B9j（前半，2026-09-05）：定时器重载语义 + ARM7 VBlank 信号

**做了什么**：
- service6 的真实运行时入口定位为 0x03804D7C；第二条命令会成功置 state=1，但
  完成回执需后续事件推进状态机。
- 修两处硬件语义：① TM3/任意定时器 CNT_L 应作为 reload，CNT_H 使能时从 reload
  起跳、溢出回 reload；② VBlank 同时置 ARM9/ARM7 两套 IF（此前 ARM7 IE bit0
  永远等不到帧事件）。

**验证**：B9j 单测 4 项 + 6.3 补 1 项；全量 **664 项检查 0 失败**。ARM7 现在能
收到 VBlank IRQ，但 service6 仍不发完成回执，B9j 后半继续。

### 21-B9j（后半，2026-09-06）：SPI device1 固件最小实现——ARM9 首次离开忙等

**做了什么**：
- service6 的真实工作内容锁定为 **SPI 访问**：事件处理器 0x03804A54 操作
  0x040001C0/1C2，SPICNT=0x8900 的 device 位是 1（固件 Flash）。
- 旧实现只认触摸 device 2，固件读命令全被忽略；补“空固件回 0xFF”最小 HLE。

**验证**：新增 SPI device1 单测；全量 **665 项检查 0 失败**。headless 真 ROM：
ARM9 PC 从 0x0200EA90 推进到 0x0200B838/0xF1BC 服务循环，ARM7 进入 0x027F6xxx
继续处理；后续需补固件真实内容或完整 SPI 状态机。

**后续修复（IRQ 入口帧）**：ARM7 长跑后会把调度器上下文 PC 存成 0，根因是
0x37FBA10 从 IRQ 栈上方读旧任务寄存器帧，而槽跳板没有预填该帧。已在 ARM7 IRQ
入口把 r0-r3/r12/lr(=PC+4) 写到 IRQ SP 上方，3200 万步内 ARM7 不再跑飞。

### 21-B9j+（2026-09-06）：SPI device1 完整状态机 + 合法固件用户区

**背景**：B9j 后半的最小 HLE 让 ARM9 第一次离开忙等，但 2M 步诊断显示 ARM9 仍
卡在 0x0200EA88，ARM7 在 0x038043E2 被反复打断。抓 SPI 字节流后发现问题不是
数据而是**解析错位**：

```text
8900:03 8900:03 8900:FE 8900:00 ...   ; READ 0x3FE00（地址首字节就是 0x03）
```

旧状态机把地址里的 0x03 再次当成 READ 命令，地址变成 0xFE0000 起，镜像全错。

**做了什么**：
- device1 状态机改为按片选（HOLD）分事务：命令只在事务开头解析，READ 收满
  3 字节大端地址后逐字节回读；`touch.c` 合成最小固件内容：主机型号/MAC/偏移
  0x7FC0/两个 CRC 合法的用户设置镜像。
- 验证 675 项 0 失败；FFXII ARM9 推进到 0x02011918 卡带读块循环，新卡点不是
  固件而是 ROMCTRL 忙位。

### 21-B9k（2026-09-06）：ARM9 硬件除法/开方 + ROMCTRL 块忙位

**背景**：0x04000280-2BF 是 NDS9 硬件 DIV/SQRT（此前被当成“未知 IO”），
IRQ/调度器要保存这些寄存器；而 0x02011918 循环读 ROMCTRL 的 bit23/bit31，
旧实现只置 DRQ 不落 bit31，块读完循环不退出。

**做了什么**：
- 新建 `src/io/math.h/.c`：DIVCNT/SQRTCNT 三模式除法（含除零/溢出钳制）与
  32/64 位开方，瞬时模型。
- `cartbus.c`：命令激活置 bit31=1，块内最后一字读完后 DRQ 与 bit31 一起清 0。
- 验证 700 项 0 失败；FFXII 越过卡带读块循环，继续走 ARM7 SoundBias/启动服务。

### 21-B9l（2026-09-06）：ARM7 SWI 0x08 SoundBias

ARM7 在 0x038043FA 调 SWI 0x08（NDS7 BIOS SoundBias）。此前 unknown SWI 把
ARM7 弹到 0x00000008 向量，之后在未映射低地址按 Thumb 漂移。新增
`src/bios/bios_snd.c` 瞬时写 SOUNDBIAS（0x000/0x200），仅 ARM7 登记。
验证 704 项 0 失败；ARM7 回到 Halt 服务入口。

### 21-B9m（2026-09-06）：ARM9 CP15 WFI

NDS9 无 HALTCNT，FFXII OS 空闲任务用 `MCR p15,0,r0,c7,c0,4` 代替 Halt。
旧实现当普通 CP15 写忽略，空闲任务满速空转；按 GBATEK 口径实现 WFI：
IME 门控下等 (IE&IF)，挂起后唤醒并进 IRQ。ARM9 的 CPU 周期不再无界增长。

### 21-B9n（2026-09-06）：POWCNT/POSTFLG 默认值——越过“提前空闲”死锁

**背景**：即使 WFI 正确，ARM9/ARM7 也会双双空闲，屏幕全白、DISPCNT=0。对比
melonDS 直接启动初值发现缺整组电源寄存器：POWCNT1 读 0 让游戏以为 LCD/3D
未上电，提前结束启动任务。

**做了什么**：
- 新建 `src/io/power.h/.c`：POSTFLG 双核=1、POWCNT1=0x820F、POWCNT2=1、
  WIFIWAITCNT=0x30；写掩码/粘住位按真机。
- headless 增加 `--screenshot`：跑完用纯渲染器输出双屏 24 位 BMP，便于不开
  窗口核对画面。
- 验证 716 项 0 失败；FFXII 离开 0x0200957C 空闲死锁，继续 0x027FFF8C 信箱
  握手与启动服务（截图仍是白屏，说明画面初始化还在其后）。

### 21-B9o（2026-09-06）：双核定时器实例分离

排查 ARM7 0x03807584 的“请求完成单元=1”等待时发现：真机 NDS9/NDS7 各有
4 个定时器（共 8 个），同址但实例独立；旧模拟器只有一套，两核配置会互相覆盖。
`io->timer` 改为 `timer[2][4]`、推进函数带核参数，716 项测试保持 0 失败。
该卡点仍在 ARM7 请求队列，说明不是定时器实例冲突。

### 21-B9p（2026-09-06）：DMA 完成中断接线

排查 ARM7 请求等待时补上真机 DMA 完成中断（IF bit8-11）：`dma_transfer`
结束后若 CNT bit14 置位，按当前核置 IF。15.3 增加 DMA0 IRQ 断言，718 项
检查 0 失败；headless 复跑 ARM7 的 0x03807584 请求等待仍在，下一步需追
ARM7 请求链表（0x03808248）为何为空。

### 21-B9q（2026-09-06）：DMA 控制器双核分离

与定时器一样把 DMA 拆成 ARM9/ARM7 两套 4 通道（真机 8 条），卡带 DRQ/VBlank
按核触发。718 项测试保持 0 失败；ARM7 请求等待仍未解除，已排除 DMA 实例
冲突这一层。

### 21-B9r（2026-09-06 分析，无代码改动）：IPC 握手时序定位

用 trace + 运行时反汇编把最终卡点收敛到 FFXII 自己的双核 IPC 协议：

- ARM9 引导初始化（0x0200B8C8，经 0x0200B650 跳板从 0x0200836C 调用）先配
  FIFO CNT=0xC408、清 0x027FFC00 事件表，随后在 0x0200B94C 反复读写 IPCSYNC，
  等 ARM7 把自己的输出 nibble 从 0 变成 8；
- ARM7 引导拷贝完成后从 0x037F8468 进入主循环，0x037FCECC → 0x0380760C →
  0x03807524，把 IE 设成 0x40000（FIFO 接收）后等请求完成单元（0x0380BA94）
  变成 1；
- 0x037FEB24 才是 ARM7 真正处理请求、经 0x037FEA B8 置 0x027FFF8C 事件位、
  并经 0x037FEC00 回 IPCSYNC 的代码，但它由 FIFO 消息驱动；
- 因此现在两边互相等待：ARM9 在等“ARM7 就绪/IPC 回应”，ARM7 在等 ARM9 的
  FIFO 数据；下一步应确认 0x0200B8C8 之前是否应发出第一条 FIFO（或在直接启动
  语义下 ARM7 应先置 IPCSYNC nibble=8），并对照 melonDS 的 IPC 初始化顺序。

**临时注入实验（已还原）**：在 ARM7 首次进入 0x03807578 FIFO 等待时，从 ARM9
侧补发历史消息 0x80004106/0x40402806，ARM7 会回 0x80004126/0x40402826，
ARM9 随后开始写 2D 仿射背景寄存器并做显示初始化（0x02002A04 等）——说明
“ARM9 该发而未发 FIFO”确实是当前死锁根因，而不是渲染/显示寄存器缺失；
但仅靠注入会让该握手无限循环，必须找到 ARM9 真正的发送时序（0x02006A94/
0x0200EE4C 路径）才能收尾。

### 21-B9s（2026-09-06）：ARM BLX 立即数缺失 + ARM7 Halt 唤醒语义

**背景**：B9r 的判断方向（ARM9 该发 FIFO）是对的，但“为什么 ARM9 没发”的根因
不在 IPC 寄存器，而在更早的一条指令：ARM9 0x02012500 是
`BLX 0x020007A8`（ARMv5 立即数形式，进安全区里的 Thumb SWI 桩），旧译码把它
当成普通 `B`。于是 CPU 没切 Thumb、没写 LR，直接落入安全区占位字节，
ARM9 后续执行流偏离，永远到不了 service13 的发送函数 0x2012614。

对照 melonDS（本地参考核心 + 探针）拿到三条关键证据：

1. ARM9 首次真实 FIFO 发送值是 **0x03CC504D**：低 5 位=13（服务号），
   参数 = 0x03CC504D>>6 = **0xF3141**；ARM7 收到后在 0x03807510
   写 BA84=0xF3141、BA94=1，这正是 ARM7 主服务等待的“请求完成单元”。
2. 上一条指令 0x02012500 在参考里译成 `BLX 0x020007A8`，目标安全区内容是
   Thumb `svc #0x0B; bx lr`（CPU 拷贝桩），不是 ARM 代码。
3. ARM7 处理 FIFO 请求时若在 Halt（SWI 0x06，Thumb `DF06`）里被中断唤醒，
   真机应继续 SWI 之后的 `bx lr`；模拟器统一“重执行被打断指令”会让它再次
   睡死，处理完的回执/队列逻辑永不落地。

**做了什么**：

- `exec.c` 实现 ARM BLX 立即数（cond=1111 分支编码）：LR=PC+4、切 Thumb、
  H 位 +2、PC 清最低位；
- `cpu.c` ARM7 IRQ HLE 恢复：被打断指令为 Thumb `DF06`（Halt）时 PC 前进 2，
  只对 Halt 特殊处理，IntrWait 等条件等待仍重执行；
- `cpu.c` ARM9 CP15 WFI 唤醒同族修复：中断唤醒时 PC 前进 4（WFI 先完成再进
  IRQ），避免返回后仍停在 WFI 上重执行；
- 新增 `[case 21-B9s]`（BLX 立即数 5 项），全量 **723 项检查 0 失败**。

**真 ROM 效果**：BA84/BA94 首次与参考完全一致（0xF3141 / 1）；ARM7 会回发
service6 回执（C0204006 类），ARM9 离开 0x0200EA88 忙等并越过 service6 完成
握手，最终双核停在系统空闲（ARM9 0x0200957C WFI / ARM7 SWI6 Halt），屏幕仍黑、
DISPCNT=0。进一步 trace 确认 ARM9 其实短暂进入过 0x020119xx 卡带读块循环
（约 300 周期）就退出并空闲，而参考里该循环跨 12+ 帧持续运行；本模拟器
疑似卡带读块状态机过早收尾（0x0201193C 的 r8/r0 比对提前失败），且 ARM7
周期性回执 C0240046 未出现。下一卡点排入 B9t：对照参考的 ROMCTRL/CARD_DATA
逐字时序，让卡带读块循环跑满。

### 21-B9t（2026-09-06 分析）：C0240046 缺失的真实根因是 Halt 唤醒后的任务路径

B9s 后对参考核心加了两级日志（状态机入口 + 关键函数模式），并与本地逐段对照，
结论比“卡带时序”更靠前：

1. 参考在 C0204006 之后不是“再入队一个新请求”，而是 ARM7 从 Halt 醒来后进入
   一个周期任务上下文（r10=0x0380A9E8 / A9C0 / AA10 …），该任务调用 0x4588
   为 16 个 service-0x10 对象入队，随后状态机逐个发 C0240046/C0240086/
   C02400C6/C0240006。
2. 本地在 C0204006 后同样回到 Halt/SWI6，但我们的 BIOS HLE 把 SWI6 当成纯
   “等中断”，唤醒后直接重跑/跳过 SWI，永远不进入参考里 BIOS 返回后继续执行的
   周期任务路径；因此后续 service-0x10 入队与回执全部缺失。
3. q82D8/q8248/q82C4 在两次回执发送瞬间均为 0，说明差异不在这些队列标志，
   而在“Halt 唤醒后由谁选择下一个执行上下文”。

**下一步 B9t 实现方向**：给 ARM7 BIOS HLE 的 Halt（SWI 0x06）补“唤醒后继续
周期任务/调度”语义（参考用 ARM7 BIOS 低地址 0x000011xx/0x00001Fxx 的异常返回
与 SWI 分发完成这一步），而不是继续在 FIFO/回执内容上做补丁。

### 21-B9u（2026-09-06）：VCOUNT / DISPSTAT 只读实现

对照任务注册路径时发现 FFXII ARM7 频繁读 0x04000006（VCOUNT）作为任务截止点
输入；模拟器此前把它当未知 IO 读 0，导致任务调度排序退化为“全部截止点相同”。
补最小实现：io 增加 vcount，帧起始回到 0 并由 runner 逐行推进（0..262 循环，
见 21-B9v），0x04000006/07 按只读字节返回；DISPSTAT（0x04000004/0x04001004）bit0 在
VBlank 后置位，作为帧边界状态。验证 729 项 0 失败。

### 21-B9v（2026-09-06）：逐行 VCOUNT + VCount 匹配中断调起任务调度器

把 VCOUNT 从“每帧一次”改为“逐行推进”（runner 每 ~4000 步一条扫描线），并实现
DISPSTAT 写语义：bit3-5 IRQ 使能可写、bit8-15 为比较值；当 vcount 到达比较值时
置 DISPSTAT bit2 并按 bit5 使能产生 IF bit2 中断。FFXII ARM7 把 IF bit2 的
handler 表项设为 0x37FDDF0——正是周期任务调度器；补上该中断后任务队列被真正
调起，C0240046 系列回执按帧重复出现，ARM9 也重新进入 0x020119xx 卡带读循环。
验证 731 项 0 失败。下一卡点转为 ARM9 收下回执后如何继续显示初始化。

### 21-B9w（2026-09-06）：direct-boot 卡带 ID 口径 + 卡带 B8 芯片 ID 命令

**参考对照**：同一阶段导出的参考 Main RAM 快照里，0x027FF800/0x027FFC00 两组
系统表值都是 `00007FC2`，而本地是 `4A465841`（“AXFJ”）。本地把 ROM 头 0x0C
的游戏代码当成了卡带 ID；melonDS `NDSCart::ParseROM` 实际用“补成 2 的幂后的
ROM 大小”推导芯片 ID（0xC2 | ((size>>20)-1)<<8，本 ROM 补到 128MB → 0x7FC2）。

随后追踪 FIFO 差异发现本地 ARM9 在 service11 之前多发了一条 service14
（0x0000004E），参考没有。调用栈落在 0x02011FE8 → 0x020120E0 → 0x02012058，
而 0x02011FE8 开头会把 `0x02011840()` 从 CARD_DATA 读回的值与 0x027FFC00
比较。反汇编 0x02011840 后再对照 melonDS `CartCommon::ROMCommandReceive`：
它发的是 **B8 命令，B8 返回芯片 ID**，不是 B7 的 ROM 读；旧 cartbus 把 B7/B8
都当 ROM 读，于是拿“AXFJ”比较，走了 service14 错误分支。

**做了什么**：
- `main.c` direct-boot 表按补幂 ROM 容量推导芯片 ID，与 melonDS 一致；
- `cartbus` 新增 `chip_id`：B7 仍读 ROM，B8 激活后固定返回一个字的芯片 ID，
  读完清除 DRQ/busy；
- 15.2 用例补 B8 断言（0x400 字节测试 ROM → 0x100C2）。

**效果**：修正后本地 ARM9 不再发 service14，改走与参考一致的 service11
（0x0000002B/0x81E3E82B/0x000000AB…），开始把 Worldmap/Menu 等 bmd 资源读入
Main RAM；全量测试 735 项 0 失败。下一卡点：ARM9 空闲任务上下文恢复后 PC 被置成
0（0x02076F24 的 ctx40 在切走空闲任务时被写 0），正在对照调度器保存/恢复语义。

---

## 4. 装载时“secure: not encrypted”不是错误

FFXII 这份 ROM 的安全区已经是**解密后的形态**：开头不是 `"encryObj"`，而是
`E7 FF DE FF …`（真机用于“防执行”的占位头）。`cart_decrypt_secure_area`
校验 `"encryObj"` 失败就保持原样，所以打印 “not encrypted (homebrew) or decrypt failed”，
对已解密的商业 ROM 这是正确行为，不需要改代码。

---

## 5. Phase B 路线（每轮一个 gap）

Phase B 的验收口径：**hard_title**——FFXII 能进入标题画面。

每轮固定节奏：

1. 给本轮微步写计划（改哪些文件、用例、验收标准）；
2. 实现 + 自测（`ctest` 全绿 + `test_nds.exe` 0 失败）；
3. 用 `--headless N` / `--trace` 跑 FFXII，观察下一卡点；
4. 更新 `DEVLOG.md` + 相关模块日志（必要时补学习文档）；
5. 提交并汇报，进入下一轮。

按当前 gap 清单，顺序预排为：

| 微步 | 做什么 | 验证 |
|------|--------|------|
| B1 | 映射 Shared WRAM：0x03000000 起 32KB + 0x037F8000 镜像（✅ 已提交 bf8899b） | 单测 + headless 重跑，确认 ARM7 不再在镜像区逐 0 漂移 |
| B2 | 实现 IPCSYNC（0x04000180/81）：双核 out 交叉读写、bit13 → 对端 IF16、bit14 门控 | 单测 544 项 0 失败；headless 不再报 IPCSYNC 未知 IO |
| B3 | 映射 Main RAM 无缓存镜像 0x02400000-0x027FFFFF（FFXII 栈放 0x027E0000 附近） | 单测 552 项 0 失败；headless：ARM9 稳定在 0x0200B9xx 主存代码区，不再弹 PC=0 |
| B4 | 修 ARM BX 奇地址未切 Thumb（ARM7 0x038043C9 入口） | 单测 555 项 0 失败；ARM7 不再按 ARM 误译 Thumb 区，旧 0x0626D1xx 终点消失 |
| B5 | 补 Thumb 逐条 trace，定位并修复 ARM7 跳进 IO 区的根因（Thumb BX/BLX Rm 解码错误） | Thumb trace + headless：IO 刷屏消失，ARM7 正常进出 SWI3/BX-lr 桩（557 项 0 失败） |
| B6 | ARM9 栈 LR 污染定位（当时以“镜像仅 ARM9 可见”临时修复；B8 修正为 ARM9 DTCM 未实现） | LDM trace + 写监视：ARM9 过 0x0200B9B4 返回点、不再弹 0xE1C010B0（559 项 0 失败） |
| B7 | ✅ 排查 ARM9 新未知 IO（0x04000204/205、0x04000247）并实现 EXMEMCNT/WRAMCNT + Shared WRAM 双核切分 | 587 项单测 0 失败；headless 3 条未知 IO 消除 |
| B8 | ✅ 解码 ARM7 0x027FFFF0 命令口与 ARM9 0x027FFF8C 事件位约定；实现 ARM9 DTCM/ITCM、镜像双核别名、direct-boot 表 | 594 项单测 0 失败；headless 越过旧轮询，ARM9 到 0x02009EC0、ARM7 到 SWI 0x0E |
| B9a | ✅ SWI 0x0E GetCRC16（CRC-16/IBM）：新增 `src/bios/bios_crc16.*`，ARM/Thumb/ARM7 单测 | 600 项单测 0 失败；headless 不再报 unknown SWI 0x0E，ARM7 进 0x037FC89C |
| B9… | ARM9 BIOS/IRQ 高向量 + 批次未知 IO 桩（GetCRC16 已完成） | 每个 gap 一次提交 |

> B9 后续实际按 gap 拆成小步：B9a（本表，GetCRC16）已完成，剩余 ARM9 BIOS/IRQ
> 高向量与未知 IO 桩继续逐 gap 提交。

> 后续步骤只有在真机现象出现后才能精确拆解，这也是本项目“一次一个微步”的原因——
> bring-up 阶段不预先猜十步，而是一步一个证据地往前走。

---

## 6. 自测

1. ARM9/ARM7 的 PC 若按 +4 匀速漂移，通常说明什么？
2. Shared WRAM 主区（0x03000000）和镜像区（0x037F8000）为什么必须映射到同一块内存？
3. `--headless` 与 `--trace` 各自适合什么时候用？
4. “secure: not encrypted”为什么不一定代表解密代码有 bug？

<details>
<summary>答案</summary>

1. 多半是在执行未映射内存读回的 0（`ANDEQ r0,r0,r0`），执行流已经跑飞。
2. 真机同一块 32KB RAM 有两个地址别名；不映射会让写入丢失、读回 0，启动代码拷贝过去的内容执行不了。
3. headless 适合大批量跑（看卡在哪个地址区间）；trace 适合小步追（看具体跳转/寄存器/未知访问）。
4. 商业 ROM dump 可能已由工具解密并把安全区开头替换为 `E7FFDEFF`，此时无需再解密。

</details>
