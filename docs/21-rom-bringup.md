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
