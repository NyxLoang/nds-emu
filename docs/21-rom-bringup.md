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
| G3 | 双核启动握手时序（ARM7 挂起/释放 + IPC FIFO 在真实 ROM 上验证） | ⏳ Phase B（21-B2..B5 已修：IPCSYNC + Main RAM 镜像 + ARM BX 切 Thumb + Thumb BX 寄存器号；ARM7 IO 刷屏消失，ARM9 栈 LR 污染待 B6） |
| G4 | 修完 G2/G3 后继续暴露的更多 gap（SWI/PPU/中断/内存等） | ⏳ Phase B 迭代 |

---

## 3. 跑真 ROM 时看到的现象（读代码时可对照）

用当前版本执行：

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

1. **ARM7 的 PC 落在 0x037F8xxx**：这是 Shared WRAM 的镜像区，当前 bus 没映射，
   读返回 0。0x00000000 恰好是一条合法但无意义的指令（`ANDEQ r0,r0,r0`），
   所以 CPU 不报错，只是一直“取 0 → 前进 4”，PC 从低地址往高地址漂。
2. **ARM9 的 PC 也在按 4 字节递增漂移**：说明某条跳转把执行流送进了没有代码/
   没有映射的地址区间，模拟器既没有崩也没有报未实现指令。

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
| B6 | 定位 ARM9 栈中返回地址被污染成 0xE1C010B0 的来源并修复 | trace/寄存器诊断 + headless，ARM9 过 0x0200B9B4 返回点 |
| B7… | 继续按新 gap 逐个修，直到 hard_title | 每个 gap 一次提交 |

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
