# 开发日志（总索引）

> 本文件只做索引，不写细节。每个功能模块一个目录 `src/<模块名>/`，源码与日志同目录：内含 `<模块名>.h`、`<模块名>.c`、`<模块名>log.md`（如 `src/cpu/` 内含 `cpu.h`、`cpu.c`、`cpulog.md`）。
> 新建模块时，先复制 `devlog/_template.md`，再在本页两个表中各加一行。

## 模块日志清单

| 模块 | 日志文件 | 职责 |
|------|----------|------|
| 窗口 / 主循环 | [`src/mainlog.md`](src/mainlog.md) | 工程骨架、SDL2、窗口与主循环（`src/main.c`） |
| 窗口 | [`src/window/windowlog.md`](src/window/windowlog.md) | 窗口/渲染器生命周期、缩放、退出事件 |
| 菜单 | [`src/menu/menulog.md`](src/menu/menulog.md) | 菜单 UI、字体、文本渲染、语言切换 |
| 整机状态 | [`src/nds/ndslog.md`](src/nds/ndslog.md) | 整机状态容器 `nds_t`（`src/nds/nds.h` / `src/nds/nds.c`） |
| 卡带装载 | [`src/cart/cartlog.md`](src/cart/cartlog.md) | 读 `.nds` 文件、解析 ROM 头、KEY1 安全区解密、卡带总线 cartbus、存档 save（`src/cart/cart.h` / `cart.c` / `key1.*` / `cartbus.*` / `save.*`） |
| 内存总线 | [`src/bus/buslog.md`](src/bus/buslog.md) | Main RAM / VRAM / IO 地址换算与 8/16/32 读写（`src/bus/bus.h` / `src/bus/bus.c`） |
| CPU | [`src/cpu/cpulog.md`](src/cpu/cpulog.md) | ARM9/ARM7 状态、取指/单步框架、指令执行（`src/cpu/cpu.h/.c` + 功能文件 `src/cpu/arm9.h/.c`、`src/cpu/arm7.h/.c`、`src/cpu/exec.h/.c`） |
| 显示 | [`src/ppu/ppulog.md`](src/ppu/ppulog.md) | 真 2D PPU：读寄存器出图（`src/ppu/ppu.h/.c` SDL 侧 + 纯渲染 `src/ppu/render.h/.c`） |
| IO 寄存器 | [`src/io/iolog.md`](src/io/iolog.md) | 中断 IME/IE/IF（双核两套）、定时器 0-3、KEYINPUT、DMA 4 通道、IPC FIFO、显示控制 DISPCNT/BGxCNT、触摸屏 SPI、音频寄存器路由、卡带总线/存档接线（`src/io/io.h/.c` + 功能文件 irq/timer/key/dma/fifo/disp/touch） |
| 音频 | [`src/snd/sndlog.md`](src/snd/sndlog.md) | 16 通道音频寄存器 + 混音合成（PCM8/PCM16/IMA-ADPCM/PSG），`snd_render` 输出 32768Hz 立体声（`src/snd/snd.h/.c`） |
| 3D 几何 | [`src/gx/gxlog.md`](src/gx/gxlog.md) | 3D 几何引擎：DISP3DCNT/GXSTAT/GXFIFO 寄存器 + 命令解码 + 矩阵/顶点变换 + 软件光栅化（`src/gx/gx.h/.c`） |
| 测试 | [`tests/testlog.md`](tests/testlog.md) | 统一测试入口 `tests/test_nds.c`（bus/指令/显示/清屏/矩形/死循环/中断/定时器/按键/DMA/双核/FIFO/2D PPU/触摸/音频/3D） |
| 时序 | [`src/timing/timinglog.md`](src/timing/timinglog.md) | 事件目标调度：系统时间戳与最小事件表（`src/timing/timing.h/.c`） |
| 帧驱动 | [`src/runner/runnerlog.md`](src/runner/runnerlog.md) | 无头/窗口共用的帧驱动 runner：双核交替步进与系统时间折算、VBlank/扫描线事件、按键注入、截图与诊断开关（`src/runner/runner.h/.c`） |

## 按时间索引

| 日期 | 微步 | 模块 | 一句话说明 | 详情 |
|------|------|------|------------|------|
| 2026-09-12 | 21-B9yi（续31，行为长跑） | 工程 | **跑到 f=10000**（修复前会卡在 ~2140）：画面序列 f4500 黑→f5000-6500 渐变出现灰色场景（顶屏 59.8% 非黑、(57,57,57)）→f7000 黑→**f7500-9500 画面完全静止 2000+ 帧**（顶 5.5% (8,6,4)/底 18.6% (21,18,14)），同段 ARM9 一直在空闲任务 `0200957C`、GX 计数几乎不动、ARM7 全程 Halt ⇒ 与「GX 忙等停摆」不同，是游戏自己在等事件/输入。下一步：查它等的是输入（按键注入路径）还是硬件事件（IF/IE、IPC FIFO 积压、卡带/GX/定时器最后推进时刻） | [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9yi（续30，行为里程碑） | GX/工程 | **修复后游戏不再停在 f≈2140**：`--headless-frames 4000`（带按键）全程序推进——ARM9 PC 在 `01FFDE94/01FFE628/02011918/0202350C/02022CB0/0200957C` 等真实游戏代码间循环、GX 命令累计 3556→38497、DISPCNT 在 `00121F10`/`80111F18` 间切换、ARM7 全程 Halt；画面 200 帧粒度抽样从白底标题→暖色场景→灰蓝→天蓝→深蓝→转场全黑→恢复内容→淡入，**画面一直在变**（修复前会永久卡在 `0x020046B8-C0` 的 GX 忙等、画面停在 2140） | [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9yi（续29，对照结论） | 工程 | **重建的 master 参考核不能替代历史参考核**：用本地历史截图 `build\gu_*.bmp`（与历史参考核逐像素一致的那版）做 10 帧粒度扫描，**f=40 起就分叉**（MAD 26.7），f=2000 达 421.8（本地顶屏全黑、新参考核 100% 非黑）；再用 `-DCMAKE_BUILD_TYPE=Debug`（与旧参考核同 `-Og`）重建后 f=2000 的 MAD **仍是 421.8** ⇒ 编译级别不是原因，是 **melonDS master 行为本身不同**。⇒ f≥40 的逐像素判定暂不可用；本轮改走**结构性判据**（代码路径/IRQ/DMA/GX/IO 状态/里程碑标记），并把「按 f=2000 MAD=0 判据 bisect 找历史 commit」列为后续手段 | [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9yi（续28，工具恢复） | 工程 | **参考核工具链重建完成**：重新 clone melonDS（master `906e9eb`）+ `-DBUILD_QT_SDL=OFF -DENABLE_OGLRENDERER=OFF -DENABLE_JIT=OFF -DENABLE_GDBSTUB=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo`，把仓库里的 `ref_harness.cpp` 装成 `refhead` 目标并补上 `REF_FRAMES`/`REF_SHOT_EVERY`；冒烟验证帧 100 = 81.9%（与历史一致），2150 帧只要 **12 秒**（旧 -Og 参考核要 ~16 分钟）。⚠️ 但新参考核是 **master 新版**，f≥2000 与历史参考核不同（与本地新旧两版 MAD 均 ~400），因此 **f≥2000 的逐像素判定暂不可用**，需找回历史 commit 或改用结构性判据；本地侧交叉验证显示 GX 修复把本地推得**更接近**参考核（f=2120 MAD 420→190） | [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9yi（续27，修复） | GX/IO | **bit27 改为「交换缓冲后置位」+ GX FIFO 按 112 条条目节流 + 模式 7 DMA 每次推 1 字**：修复前 ARM9 在 f=2135-2152 卡在 GX 忙等（`q=113 pend=744`、队列卡在缺参数的 `0x34`）；修复后 f=2135-2140 ARM9 在 ITCM 游戏代码里正常推进（与参考核 2140-2152 的 ctrace 形态一致）。931 项单测 0 失败、900 帧三标记一致。⚠️ `%TEMP%` 被系统清理：melonDS 参考核源码/补丁与 `ref_fb_*.bin` 均丢失 ⇒ 逐像素对照暂不可用，恢复计划见 [21](docs/21-rom-bringup.md)（重 clone + 按文档补回 REF_* 诊断） | [gx](src/gx/gxlog.md) · [io](src/io/iolog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9yi（续26，对照） | GX/工程 | **参考核 f=2140-2145 一次都没写 GX FIFO**（新增 `REF_GXFIFO` 日志 + `REF_NORENDER` 快速模式）：参考核同段在 ITCM/主存跑**流式加载**，而本地在提交 3D 命令 ⇒ 本地 f=2140+ 的 GX 队列卡死是**状态分叉的症状**而非根因（两侧任务/阶段推进顺序已不同：本地整体落后 10 帧，却在局部做参考核更晚才做的事）。工具备注：只跳光栅化对总时长帮助有限（2150 帧仍 ~16 分钟，大头在几何/管线） | [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9yi（续25，诊断） | GX | **f=2141 停摆定位＝GX 命令队列卡在 `0x34(1/32)`**：新增 `gx_state()`（FIFO 字数/工作周期/队列长度/待收参数）并接进 `NDS_FTRACE`（多打 `gxfifo=… q=… pend=…`），`NDS_GXDBG=1` 打印队头缺参数时的队列快照——现场显示 43 条 `34(1/32)`、`pend=992`，ARM9 卡在 GXSTAT bit27 忙等（`0x0200468C-94`）。两个方向的实验都回退：①按 melonDS 表把 `0x34` 当 32 参数 ⇒ 渲染全黑（游戏实际按「0x34+1 参数」发流）；②bit27 只在 SWAP 置位 ⇒ 停摆消失但本地冲在参考核前面（说明还缺 3D 管线成本：顶点/多边形摊到引擎周期）。当前保留诊断、行为回到已验证状态：f≤2140 逐帧 100% 一致、931 项 0 失败 | [gx](src/gx/gxlog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9yi（续24，重构） | GX | **GX 命令改为「按成本执行」**：入队后由 3D 引擎工作周期驱动（`gx_run_due`：先扣周期，归零才执行下一条并按命令类型记 3-254 周期成本），FIFO 消费速度由命令成本决定、**去掉 NDS_GXDRAIN 标定常数**；同轮修掉由此暴露的 bug（队列保留已完成命令后，`gx_feed_param` 必须喂**队尾**未收齐的那条，否则参数全丢、队列卡死）。实测仍保持 **f≤2140 逐帧 100% 一致**（与标定最优同窗口），f=2150 起为「画面落后」；931 项 0 失败、900 帧三标记一致、帧 100 仍 81.9% | [gx](src/gx/gxlog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9yi（续23，修复/标定） | GX/时序 | **FIFO 消费余数丢失（真 bug）+ 消费速率标定**：`gx_advance` 的 `cycles/div` 在 ARM9 每步 1-2 周期时恒为 0 ⇒ FIFO 永不消费、模式 7 DMA 卡死（实测 f=2141 起 ARM9 在 `0x0202350C` 三指令小循环死等、`dma3=…/FC400134` 不前进）。加 `fifo_drain_acc` 余数累加后，用 `NDS_GXDRAIN` 标定 4/16/64/**128**/256：4-16 太快（f=2120 起内容分歧）、64 到 2120、**128 与 256 都能把逐帧 100% 一致保持到 f=2140**（256 之后整体拖慢 3 倍）⇒ 取 **128** 为默认；931 项 0 失败、900 帧三标记一致、帧 100 仍 81.9% | [gx](src/gx/gxlog.md) · [io](src/io/iolog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9yi（续22，修复） | GX/IO | **GX 命令 FIFO 加节拍（112 项容量 + 4 周期/字消费 + 模式 7 DMA 分批/续跑）**：本地模式 7 DMA 不再「一次搬完」，`IF bit11` 从 **58 次/81 帧 → 0 次**（与参考核一致）；**逐帧一致窗口从 f=1900-2110 扩展到 f=1900-2140**（全 100% 逐像素），f=2150 起降级为「整体落后 10 帧」（本地 f=2150 与参考核 f=2140 逐像素相同）；931 项 0 失败、900 帧三标记一致、帧 100 仍 81.9% | [gx](src/gx/gxlog.md) · [io](src/io/iolog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9yi（续21，对照/工具） | IO/诊断 | **整片 IO 寄存器窗口对照**（画面仍 100% 相同的 f=2100）：IO9 有 33 个、IO7 有 83 个寄存器不同。关键单调量：`0x04000004 DISPSTAT` 参考核 `0106001A`（含 bit1 HBlank 标志 + bit4 HBlank 中断使能 + VCount=262）vs 本地 `0000000C`（无 HBlank）；`0x040000DC DMA3` 参考核 `7C400185`（模式 7 GX FIFO + IRQ）vs 本地 `04000100`（模式 0）。另：melonDS 核心 `Log()` 会把对照写成 **4.1 GB** 日志、单次 2100 帧要 13 分钟 ⇒ 改为默认静音（`REF_VERBOSE=1` 才输出） | [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9yi（续20，对照/诊断） | GX/DMA | **ARM9 多出的 IF bit11 定位到「没有节拍的 GX-FIFO DMA」**：`NDS_DMAIRQ` 显示本地 58 次 bit11 全是 `ch=3 cnt_h=7C40（模式 7 GX FIFO + IRQ on end）dad=04000400 pc=01FF8300`；参考核同窗口 DMA3 全是 `cnt_h=8400`（模式 0 立即、无 IRQ）的普通内存搬运 ⇒ 本地模式 7「一次搬完」立刻满足 RemCount==0 而挂中断（melonDS 受 112 项 FIFO 容量与 3D 消耗速度限制会 Stall 分批），且两侧 DMA3 用途已不同 ⇒ bit11 是状态分叉的症状。下一步＝给 GX 加有界命令 FIFO + 消耗节拍 + GXSTAT 真实电平位，并让模式 7 DMA 分批推进 | [21](docs/21-rom-bringup.md) · [io](src/io/iolog.md) |
| 2026-09-12 | 21-B9yi（续19，对照） | CPU/诊断 | **两核 IRQ 画像逐项对比**（本地 `locirq/locirq7`、参考 `refirq/refirq7`，f=2000-2060）：**ARM7 完全一致**（均 12.5 次/帧；bit0=61、bit2=244、bit3=8、bit4≈452）⇒ 20% 任务保存次数差不是 ARM7 中断率造成；ARM9 剩两处差异：HBlank 多 17%（202 vs 172 次/帧）、DMA3 完成中断多 ~1 次/帧（参考 0）；另：DISPCNT 翻转两侧走**同一段代码**（pc=02002840、同值交替），本地只是早 ~15 帧。参考核高流量调试打印改为 `REF_VERBOSE=1` 才输出 | [21](docs/21-rom-bringup.md) · [cpu](src/cpu/cpulog.md) |
| 2026-09-12 | 21-B9yi（续18，代码/对照） | IO/时序 | **补上 HBlank 中断（IF bit1）**：两侧 IRQ 采样证明参考核 ARM9 每帧 268 次 IRQ（f=2090-2102，IF bit1 占 3516/3741），本地只有 5-6 次且**全项目没有 HBlank 触发**——FFXII 靠它做每行流式加载。实现后本地 f=2015-2025 也是 **268/帧**（同型），f=2000-2110 仍逐帧 100% 相同、900 帧三标记一致、帧 100 仍 81.9%、931 项 0 失败；f=2120 分歧仍在。新线索：用 `--watch 02077000-02077010` 统计任务现场保存次数，两侧从第 1 帧起就差 ~20%（本地更少）⇒ 调度节拍差，下一步核对驱动它的定时器/HBlank 使能窗口 | [io](src/io/iolog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9yi（续17，实验/回退 + 对照） | GX/诊断 | **「交换后清屏」假说被证伪**：把 GX 改成双缓冲 + `0x50 SWAP_BUFFERS` 交换清后缓冲后，f=2000-2110 仍 100% 相同、但 **f=2120/2140/2150 的 MAD 与改动前逐值完全相同**（589.3/587.3/552.4），且单测出现 1 项失败 ⇒ 已回退（931 项回到 0 失败）。改用**内存级**证据：f=2115 主存不同字 274055/1048576，其中小数值差异集中在 **0x02077xxx 游戏任务/调度上下文表**（±1/±3/±7/±30 这类计数）⇒ 首个分歧是游戏任务状态本身；另记录 `--dump` 参数顺序的 Windows 解析小坑 | [21](docs/21-rom-bringup.md) · [runner](src/runner/runnerlog.md) |
| 2026-09-12 | 21-B9yi（续16，代码/对照） | GX/时序 | **GXSTAT bit27（3D 忙）建模**：反汇编证实游戏在 `0x020046B8` 用 `ands r0,#0x08000000` 等 3D 引擎；本地此前恒不置位。按 melonDS `GPU3D::AddCycles()` 口径给每条 GX 命令记工作周期（矩阵 16-35 / 顶点 18 / 交换 254），`gx_advance()` 挂 ARM9 系统时钟消耗。单测 931 项 0 失败、900 帧三标记一致、帧 100 仍 81.9%、f=2090-2110 仍逐帧 100% 相同、帧 500 A/B 无差异（MAD 135.27）；**f=2120 分歧仍在**（逐值相同）⇒ 该分歧不是 3D 忙等待，而是「同一 VRAM bank 的内容差异」（参考黑/本地有画面）⇒ 下一步做 GX 双缓冲 + 交换清屏 | [gx](src/gx/gxlog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9yi（续15，修复） | IO/时序 | **卡带时钟不再丢剩余周期 ⇒ 加载阶段追平参考核**：`cartbus_advance()` 每次只取一个字、把没用完的周期丢掉，空闲期（ARM9 WFI、runner 一次给 delta≈2130 周期）每帧只取 ~290 字（参考核 6400 字/帧），于是「等最后一块搬完」白等 ~330 帧。改成「取字→触发该字 DRQ 的卡带 DMA→继续用剩余周期」循环后：空闲期 1456~3686 字/帧，**f=1900-2110 与参考核逐帧 100% 逐像素相同**（MAD 0.00），f=2000 已进入同步读卡带循环；新分歧点＝f=2120 起本地的淡入/过场比参考核早 ~30-40 帧 | [io](src/io/iolog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9yi（续14，分析） | IO/时序 | **滞后 ~330 帧的机理＝空闲期系统时钟不推进**：停摆修好后本地能跑完加载阶段，但空闲期（ARM9 在空闲任务/等待、ARM7 停在 Halt）卡带每帧只前进 **34 个字**（参考核同步阶段 6400 字/帧）——本地的卡带/定时器时钟只在 ARM9 指令步进时推进，而 melonDS 把它们挂在系统时钟上（空转/WFI 时照样走）。f=2600 画面与参考核 f=2300 的 MAD 71（对照 f=2600 的 MAD 150）与滞后判断一致；下一轮改系统时钟推进 | [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9yi（续13，修复） | 卡带/IO | **1904 帧停摆的真正根因＝一次多余的卡带完成中断**：窗口内唯一一条「无传输」的 END（f=1888 `pos=0 len=0`）来自「立即模式 DMA 同步执行」——游戏每块收尾写 `CNT_H=0x8500` 时本地当拍就读了 `CARD_DATA`（`xfer_len==0` 被误判成传输结束）；任务计数每个完成中断减 0x200，多一次就**提前一块**以为搬完 ⇒ 转去读芯片 ID、最后一块没人读、ROMCTRL bit31 永远等不到。修法：`cartbus_end()` 只在 `xfer_len != 0` 时挂完成中断。结果：f=1910 ARM9 回到空闲任务 `0200957C`（改前死等 `02011C50`），游戏继续走完加载阶段并在 f≈2400 开始出三角形（tri=8906→13359）、DISPCNT 与参考核同序列（滞后 ~300 帧）；900 帧三标记一致、帧 100 画面 81.9%、931 项 0 失败 | [cart](src/cart/cartlog.md) · [io](src/io/iolog.md) · [tests](tests/testlog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9yi（续12，修复/对照） | IO/卡带/CPU | **卡带 DMA 改成参考核语义（去瞬间推进 + 电平敏感重检 + 完成中断只挂 ARM9）**：本地卡带节奏从 205 块/帧（≈4 倍速）回到 **52 块/帧**（参考 51）、第 1886-1903 帧**前 931 条命令逐条相同**、ARM7 不再恒挂 `IF bit19`（`if7=00000000/ie7=0104009D` 与参考逐位一致）；**第 1904 帧的停摆仍在**，但机理已定位到「处理器先停 DMA、该块剩 2 字无人读 → 芯片 ID 读取等 ROMCTRL bit31 等死」（参考核是「整块搬完 → 才停 DMA」）；900 帧里程碑三标记与参考一致、帧 100 画面 81.9% 不回归；931 项 0 失败 | [io](src/io/iolog.md) · [cart](src/cart/cartlog.md) · [cpu](src/cpu/cpulog.md) · [runner](src/runner/runnerlog.md) · [tests](tests/testlog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9yi（代码/对照） | 卡带/IO/CPU | **卡带真机语义 + 预取 FIFO**：ROMCTRL 只读位/仅 bit31 0→1 启动/AUXSPICNT 门控 + 卡带侧 2 字预取 FIFO（melonDS 口径）——命令流前 5388 条、CARD_DATA 前 500,905 次读取与参考核逐条/逐值一致；取指代价默认改 1 后**读卡带阶段与参考核稳定同步在 −8~−13 帧**；929 项 0 失败 | [cart](src/cart/cartlog.md) · [io](src/io/iolog.md) · [cpu](src/cpu/cpulog.md) · [tests](tests/testlog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9yi（续，诊断） | 卡带/诊断 | **把「按键离开标题后卡死」精确到帧**：给卡带传输启动加上发起者 PC 后，本地第 1887 帧出现连续两次 B8（芯片 ID 校验失败重试）、第 1888 帧出现唯一一次 IRQ 上下文发起命令，之后卡在 ROMCTRL 轮询；参考核同段只有块读/芯片 ID 两个来源且 ARM9 始终在 System 模式。卡带时钟改为按指令实际周期推进（口径修正，进度曲线不变） | [cart](src/cart/cartlog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9yi（续3，修复） | IO/卡带 | **卡带中断风暴修复（死锁解除）**：melonDS 的数据就绪只触发 DMA、不挂中断（中断只在传输结束且 AUXSPICNT bit14 时挂出）；旧实现每取一字挂 IF bit19 ⇒ ARM9 每 ~170 周期被中断、任务调度相位错乱并死锁。修正后 ARM9 与参考核一样进入空闲任务不再卡死；新增 `NDS_IRQLOG`/`NDS_MODELOG` 诊断；930 项 0 失败 | [io](src/io/iolog.md) · [cart](src/cart/cartlog.md) · [tests](tests/testlog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9yi（续4，对照） | 诊断/工程 | **模式级对照：本地与参考核同步到第 1902 帧**（两侧都在空闲任务↔IRQ 往返），分歧点是参考核第 1903 帧被事件唤醒、本地没有；四区逐字节：ITCM/VRAM **0.00%**、DTCM 0.12%、主存 2.19%，关键寄存器一致；内核「恢复任务」例程本地能正确执行（含 `ldm ^` + `subs pc,lr,#4` 的 User/System 模式恢复），只是就绪队列为空 | [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9yi（续5，对照） | 诊断/卡带 | **唤醒事件确认＝卡带传输完成中断（IF bit19 / IRQ_CartXferDone）**：参考核空闲期把卡带 DMA 武装+使能（`AF000001`），DMA 自动搬完整笔传输→`ROMEndTransfer`→bit19→任务唤醒（第 1903 帧）；本地 DMA 已用尽且未重武装、bit19 从 1888 到 3000 帧恒 0。下一步对齐「DRQ 已置位时武装 DMA 立即开始」的电平敏感语义 | [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9yi（续6，对照） | 诊断/卡带 | **DMA 写序列对照**：两边是同一段 ITCM 代码、同一组值（`SAD=04100010`、`CNT=AF000001`：模式 5 + 重复位 + 使能），差别只在推进——参考核空闲期由该 DMA 把整笔传输搬完并挂 bit19；本地停在「FIFO 还剩一个字」。新增 `NDS_DMALOG` 诊断；「搬完再检查 + 去掉瞬间推进 hack」的完整改造列入下一步（本轮先回退保持 930 项全绿） | [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9yi（续9，修复） | IO/卡带 | **读来源统计锁定 DMA 两个真 bug 并修复**：①重复模式（CNT bit25）地址不前进 ⇒ 卡带 DMA 每字都写同一个 dword；②DRQ 未置位也触发卡带 DMA（melonDS `CheckDMA()` 第一行即查 bit23）⇒ 把 FIFO 残留 `0xFFFFFFFF` 搬进游戏缓冲。修好后 930 项 0 失败、900 帧里程碑标记不回归；统计显示该帧 127 次读全部来自 DMA | [io](src/io/iolog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9yi（续10，实验/回退） | IO/卡带 | **四件套组合实验（搬运后重检 + 去瞬间推进 + 地址前进 + DRQ 门控）**：单测 930 项全绿且卡带用例改成真机逐字节奏，但实测游戏从「与参考核一致的空闲等待」变成**硬死锁**（1900-3000 帧卡在 `0x02011C50` 轮询、IRQ 模式；bit19 能置位但处理器等一个只有任务侧才能推进的传输）⇒ 只保留两个确定 bug 修复，节奏/重检回退；下一步先解释参考核的 CPU/DMA 分工与任务阻塞点 | [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9yh（代码/测试） | CPU/帧驱动 | **按取指区域计费**：`arm_cpu_t.next_fetch_pc` + `cpu_fetch_cost()`（ARM9 主存非顺序取指 3、其余 1）——ARM9 吞吐从 +45%/+8.9% 收到 **−2.3%/−7.2%**（对照参考核 26 帧/1900 帧指令数）；时间线稳定在 +1~+13 帧；926 项 0 失败 | [cpu](src/cpu/cpulog.md) · [runner](src/runner/runnerlog.md) · [tests](tests/testlog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9yg（代码/对照） | 帧驱动/时序 | **ARM9 时钟折算对齐参考核**（`cost9/2` → `/1`，可 `NDS_ARM9_DIV=2` 回退）：逐帧对照从「第 20 帧起分歧」变成帧 10/20/30 **100% 逐像素相同**；开显示帧 13→30（参考 26）、标题帧 ~300→535（参考 542）、第 1500 帧两边 OCR 同为 `ILLUSTRATION RYOMA ITO`；新增 runner 模块日志；921 项 0 失败 | [runner](src/runner/runnerlog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9ye（代码/测试/对照） | BIOS/CPU | **ARM7 低地址「参考级」实证**：SWI 分发器 0x1080-0x10A8 逐条拆分（IRQ 可像参考核一样插在 0x109C-0x10A8 之间）+ 两边低地址取指直方图对照 —— 900 帧**两边各命中 93 个低地址、集合完全相同**；发现余下差异是「本地 SWI 调用次数偏多（音频查表 +33%）」，留给下一步；921 项 0 失败 | [bios](src/bios/bioslog.md) · [tests](tests/testlog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9yd（代码/测试） | BIOS/总线 | ARM7 低地址换装**完整 FreeBIOS 镜像**（0x4000，BSD-2 同源）+ 未逐条建模的 SWI 函数体**在真地址执行真实字节**（此前 C 版 HLE 直接跳 swi_complete）；CRC16 真执行结果 0x37DD = 独立 CRC-16/ARC，证明字节码算对；顺带纠正 SoundBias/CRC16 两条测试口径（FreeBIOS 语义）；909 项 0 失败 | [bios](src/bios/bioslog.md) · [bus](src/bus/buslog.md) · [tests](tests/testlog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9yc（代码/测试） | 显示 | RGB555→888 换算对齐参考核（`(c5<<3)|(c5>>3)`，白 0xF8→0xFB）+ 28 条颜色期望逐条更新：帧 100 逐像素相同率 0%→**81.9%**（MAD 59.1→51.5），907 项 0 失败 | [ppu](src/ppu/ppulog.md) 路 [tests](tests/testlog.md) 路 [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9xc（代码/测试） | IO/主循环 | VBlank 时序对齐真机（第 192 行触发、帧边界结束）：IF 模式与参考一致，标题 frame1900 仍逐像素一致，885 项 0 失败 | [io](src/io/iolog.md) 路 [main](src/mainlog.md) 路 [tests](tests/testlog.md) 路 [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9xa（代码/测试） | IO | IME 只保留 bit0（dump 与参考一致）+ 声音驱动诊断 `io: snd-cnt-writes`：879 项 0 失败 | [io](src/io/iolog.md) 路 [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9wy（代码/测试） | 3D/IO | GXSTAT FIFO 状态位对齐参考核（bit25+bit26，与参考 0x86000000 一致）：879 项 0 失败 | [gx](src/gx/gxlog.md) 路 [tests](tests/testlog.md) 路 [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9ww/B9wx（代码/测试） | IO/音频/3D | ARM9 DMA 模式 7（GX 显示列表 FIFO）+ NDS RTC 串行时钟（0x04000134/138）：875 项 0 失败 | [io](src/io/iolog.md) 路 [tests](tests/testlog.md) 路 [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9wv（代码/测试） | IO/按键 | KEYINPUT 复位值修正（0=全按下 → 全松开）：游戏不再误判 START/A 一直按着，时间线与参考的卡带读循环对齐；866 项 0 失败 | [io](src/io/iolog.md) 路 [tests](tests/testlog.md) 路 [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9wu（代码/测试） | IO/音频/主循环 | DMA 按属主核分流 IO（ARM9 显示列表不再被当成音频写）+ 无头模式补推 SPU 时间 + `--dump` 现场导出与逐帧 IPC/IF 诊断：865 项 0 失败 | [io](src/io/iolog.md) 路 [snd](src/snd/sndlog.md) 路 [main](src/mainlog.md) 路 [tests](tests/testlog.md) 路 [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9ws（代码/测试） | BIOS/CPU/总线 | ARM7 FreeBIOS 低地址 Halt/调度路径参考级 HLE：SWI 分发器栈帧/模式、Halt+HALTCNT、IntrWait 软件标志、IRQ 0x1FB0/0x1FC0、SoftReset、WRAM 镜像与 BIOS 影子字节；860 项 0 失败 | [bios](src/bios/bioslog.md) 路 [cpu](src/cpu/cpulog.md) 路 [bus](src/bus/buslog.md) 路 [io](src/io/iolog.md) 路 [tests](tests/testlog.md) 路 [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9wr（代码/测试） | IO/卡带/主循环 | KEYCNT 按键中断 + NDS7 卡带 DMA 触发模式 0x12 + 按键按帧保持：830 项 0 失败 | [io](src/io/iolog.md) 路 [main](src/mainlog.md) 路 [tests](tests/testlog.md) 路 [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9wg（代码/测试） | CPU | ARM7 IRQ 入口/尾部改 FreeBIOS 0x1FB0/0x1FC0 参考级 HLE：六字帧 + SPSR 恢复，792 项 0 失败 | [cpu](src/cpu/cpulog.md) 路 [tests](tests/testlog.md) 路 [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9wh（代码/测试） | CPU | CLZ Rd 掩码修正（0x0FFF0FF0 留出 Rd）：0x847C 的 CLZ r10,r3 不再误判为 CMN，标题位流解码越过 0x86A4，795 项 0 失败 | [cpu](src/cpu/cpulog.md) 路 [tests](tests/testlog.md) 路 [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9wi（代码/测试） | BIOS/工程 | ARM7 SWI 0x1A-0x1D 音频/启动查表（FreeBIOS）+ headless-cycles 帧/VRAM 摘要与截图：0x0380443C 的 SWI 0x1C 不再掉低地址，805 项 0 失败 | [bios](src/bios/bioslog.md) 路 [tests](tests/testlog.md) 路 [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9wj（代码/测试） | 显示/总线 | DISPCNT VRAM 直显模式（bit16-17=2，物理 bank 直读）：FFXII 标题顶屏从全黑变为 2912 色，808 项 0 失败 | [ppu](src/ppu/ppulog.md) 路 [bus](src/bus/buslog.md) 路 [tests](tests/testlog.md) 路 [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9wk（代码/测试） | CPU/显示 | ARMv5 DSP 乘法（SMLAxy/SMLAWy/SMLALxy/SMULxy/SMULWy）：标题解码 42,899 条指令与参考逐条一致，818 项 0 失败；截图上下屏顺序修正 | [cpu](src/cpu/cpulog.md) 路 [tests](tests/testlog.md) 路 [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9wl（里程碑） | 工程 | **hard_title 达成候选**：本地连续运行到 frame1790，模拟器自身渲染出标题文字（OCR：FINAL FANTASY…）；此前 OCR 读到开场制作人员表，证明画面链路正常；本地时间线比参考晚约 500 帧（仅文档） | [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9wm（验证） | 工程 | 标题后可交互：无头脚本在 frame1950 按 START 后游戏离开标题进入下一状态（frame5580 场景变化、DISPCNT 切换）；按键路径可用（仅文档） | [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9wn（工具） | 主循环 | 无头脚本按键 CLI：`--key-frame/--key-mask/--key-period`（真实 ROM 上 START 可推进流程）；headless 摘要补两核 IRQ 计数，用于排查开场节奏 | [main](src/mainlog.md) 路 [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9wo（代码/测试） | 时序/IO | 定时器改为按“经过周期”推进（timer_advance 接受 cycles）+ 事件等待期补偿两核定时器：参考字幕阶段 TM0/1 CNT_H=00C1 已使能，修正后同一步数字幕前进约 5%，818 项 0 失败 | [io](src/io/iolog.md) 路 [cpu](src/cpu/cpulog.md) 路 [tests](tests/testlog.md) 路 [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9wp（验证） | 工程 | 定时器修正后里程碑复测：16.5 亿步到 frame1683 仍渲染出 FINAL FANTASY 标题字（`build/title3_A_x2.png`），ARM7 稳定 0x1158（仅文档） | [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9wq（代码/测试） | 主循环/工程 | 窗口模式接入与 headless 同源的持久化帧驱动 runner（`--headless-frames N`）+ 本工程源码 -O2：1700 帧可到标题、约 18fps 等价吞吐，818 项 0 失败 | [main](src/mainlog.md) 路 [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9wf（代码/测试） | CPU/BIOS | ARM7 FreeBIOS 低地址等待路径 HLE：WaitByLoop 真循环节拍 + Halt 暂停/唤醒，790 项 0 失败 | [cpu](src/cpu/cpulog.md) 路 [bios](src/bios/bioslog.md) 路 [tests](tests/testlog.md) 路 [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9we（代码/测试） | 总线/PPU | LCDC 分 bank 映射 + Engine B BG 扩展调色板：SQUARE ENIX 屏亮起，770 项 0 失败 | [bus](src/bus/buslog.md) 路 [io](src/io/iolog.md) 路 [tests](tests/testlog.md) 路 [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9wd（代码/测试） | 总线/IO | 实现 VRAMCNT 动态映射：FFXII bank D→A BG/C→B BG/E→A OBJ/H→B OBJ，768 项 0 失败 | [bus](src/bus/buslog.md) 路 [io](src/io/iolog.md) 路 [tests](tests/testlog.md) 路 [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9wa（代码/测试） | CPU | ARM9 IRQ 返回桩改为 FreeBIOS 尾部 0xFFFF06F0（弹六字帧 + SUBS 返回）：第二次 6B 后 service11 越过旧空闲，763 项 0 失败 | [cpu](src/cpu/cpulog.md) 路 [tests](tests/testlog.md) 路 [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9vz（分析） | 工程 | 第二次 6B 后参考先发 1AB 再执行 0778C 旧 sleep 收尾；本地顺序反了，先 0778C 回写 76F24 后空闲（仅文档） | [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9vy（分析） | 工程 | 参考第二次 service11 窗口 Δt9≈1079-1131/Δt7≈545-588，本地 6B→AB 仅 548 条指令，作为成本校准基准（仅文档） | [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9vx | 工程 | 事件目标 headless 驱动入口 `--headless-cycles`：timing 事件表挂 VBlank/扫描线，固定 2:1 保留回退 | [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9vw | 时序/测试 | 新增最小事件目标调度表 timing（arm/disarm/next/advance），762 项 0 失败 | [timing](src/timing/timinglog.md) 路 [tests](tests/testlog.md) 路 [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9vv | CPU/测试 | 周期成本模型骨架：arm_cpu_t.step_cycles（普通=1、WFI 等待=0），755 项 0 失败 | [cpu](src/cpu/cpulog.md) 路 [tests](tests/testlog.md) 路 [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9zu（路线） | 工程 | 确定大改造路线：周期成本模型 → 事件目标调度器 → 双核驱动迁移 → 回到第二次 6B 校准（仅文档） | [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9zr（分析） | 工程 | 第二次唤醒后 F18=0x76F24 的回写前没有新扫描：ITCM 切换只写了 F18，未真正把 0x76FE4 恢复为当前任务（仅文档） | [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9zq（分析） | 工程 | FIFO 队列计数复测：ARM7 发送后 ARM9 立即读走、无积压，排除第二次 6B 的 C024 积压猜测（仅文档） | [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9zp（分析） | 工程 | 第二次 6B 唤醒后 ITCM 已把 F18 切回 0x76FE4，但本地随后在 077D4 又切回空闲 0x76F24；参考直接续发下一条 service11（仅文档） | [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9zo | 卡带/测试 | ROMCTRL 块长按 melonDS `0x100<<n` 修正（旧 0x100<<(n-1) 导致 B7 每块只读一半）：FFXII 主存缓冲与参考 frame30 逐字节一致，751 项 0 失败 | [cart](src/cart/cartlog.md) 路 [tests](tests/testlog.md) 路 [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9zn | CPU/测试 | melonDS A_STM “基址在列表内保存当前写地址”补到 ARM9（B9zg 只修了 ARM7）：751 项 0 失败；发送点探针复测确认本地已能发 2B/E3E82B/AB 三次 service11 后停回空闲 | [cpu](src/cpu/cpulog.md) 路 [tests](tests/testlog.md) 路 [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9zm（实验） | 工程 | 帧/VCOUNT 节奏 4 倍加速后 service11 仍为 0，排除“帧数不足”（仅文档） | [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9zl（分析） | 工程 | service 分发 0x0200D9FC 本地仅 4 轮 r1=9/0xA 事件就等待；转回 ARM7 A4F8 激活点（仅文档） | [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9zk（分析） | 工程 | 事件位图 0x027FFFB0 本地 BFFFFFFF vs 参考 1FFFFFFF，属少推进的结果而非原因（仅文档） | [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9zj（分析） | 工程 | 事件号 0x02078E5C 本地 0x41 vs 参考 0x42，状态机缺第 0x42 次完成事件（仅文档） | [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9zi（分析） | 工程/卡带 | service7 后本地卡带循环仅约 1 帧、参考约 12 帧才发 service11；ARM7 服务槽未持续喂读循环（仅文档） | [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9zh（分析） | 工程/卡带 | 本地 ARM9 service11=0 vs 参考 20 次；A4F8 入队本地 11 vs 参考 284，卡点前移到 service7 完成后的分支（仅文档） | [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9zg | CPU/测试 | ARM7 STM 基址寄存器在列表内保存当前写地址（melonDS A_STM 口径）：任务 A4F8 上下文自指字段对上参考，747 项 0 失败 | [cpu](src/cpu/cpulog.md) 路 [tests](tests/testlog.md) 路 [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9zf（实验） | 工程 | 强制 0x0380A500 自指后 900M 步仍无第二阶段，说明该字段不是充分条件（仅文档） | [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9ze（分析） | 工程 | 写监视定位 ARM7 A4F0+10/A4F8+8 差 4 的来源：第一次上下文保存时用户 r1 本地 A4FC vs 参考 A500（仅文档） | [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9zd（实验） | 工程/卡带 | 强制保留 0x027FFC30=FFFF 可停住 ARM7 入队风暴，但基座对象计数仍 1 vs 参考 7，第二阶段仍缺（仅文档） | [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9zc | 卡带/IO/测试 | B7/B8 按 melonDS 周期时序（首字/cmd/gap/逐字延迟）实现：FFXII 第一阶段从约 2 帧拉长到约 38 帧才空闲，743 项 0 失败 | [cart](src/cart/cartlog.md) 路 [io](src/io/iolog.md) 路 [tests](tests/testlog.md) 路 [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9zb（分析） | 工程/卡带 | 长帧参考 2480 帧 + 500/530/560/770 快照对齐：第二阶段约第 539 帧由 ARM7 对象队列 7→11 触发；本地卡带零延迟导致第一阶段提前完成、ARM7 反复入队（仅文档） | [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9w | 卡带/装载 | direct-boot 表卡带 ID 按补幂容量推导 + 卡带 B8 命令返回芯片 ID：FFXII 越过错误的 service14 分支，进入 service11 文件读取（含 4 项新检查） | [cart](src/cart/cartlog.md) 路 [main](src/mainlog.md) 路 [tests](tests/testlog.md) |
| 2026-09-06 | 21-B9x | CPU | ARM9 IRQ 入口补 BIOS 六字帧（r0-r3/r12/lr）：FFXII ITCM 分发器不再弹栈底 0，ARM9 不再跳低地址空扫，稳定停在空闲任务（738 项 0 失败） | [cpu](src/cpu/cpulog.md) 路 [tests](tests/testlog.md) 路 [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9y | CPU | IRQ 返回时恢复 ARM9 IRQ SP：压帧后 SP 不回弹会约 180 帧后溢出覆盖 handler 表，导致 FIFO 中断跳到 IO；修复后 5 亿步仍稳定空闲（739 项 0 失败） | [cpu](src/cpu/cpulog.md) 路 [tests](tests/testlog.md) 路 [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9z | 卡带 | B7 地址按补幂 ROM 掩码并对 <0x8000 重定向，尾部按 melonDS 零填充：卡带读取口径更贴近参考（739 项 0 失败） | [cart](src/cart/cartlog.md) 路 [tests](tests/testlog.md) 路 [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9za | 3D/IO | GXSTAT bits30-31 FIFO IRQ 模式可写并同步 ARM9 IF bit21：本地 IF9 补上参考的 0x200000（742 项 0 失败） | [gx](src/gx/gxlog.md) 路 [io](src/io/iolog.md) 路 [tests](tests/testlog.md) 路 [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9v | IO/CPU | VCOUNT 逐行推进 + DISPSTAT VCount 匹配中断（IF bit2）：FFXII ARM7 0x37FDDF0 任务调度器被调起，C024 系列回执开始出现（731 项 0 失败） | [io](src/io/iolog.md) 路 [cpu](src/cpu/cpulog.md) 路 [tests](tests/testlog.md) 路 [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9u | IO/测试 | VCOUNT（0x04000006）+ DISPSTAT（0x04000004 bit0）只读实现：ARM7 任务调度不再把帧状态当未知 IO 读 0（729 项 0 失败） | [io](src/io/iolog.md) 路 [tests](tests/testlog.md) 路 [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9t（分析） | CPU/BIOS | 对照参考定位：C0204006 后缺的不是回执本身，而是 ARM7 Halt 经 BIOS 唤醒后继续系统任务的路径（仅文档） | [cpu](src/cpu/cpulog.md) 路 [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9s+ | CPU | ARM9 CP15 WFI 唤醒后 PC 前进：与 ARM7 Halt 同族，空闲任务能越过 WFI（723 项 0 失败） | [cpu](src/cpu/cpulog.md) 路 [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9s | CPU/IO/测试 | ARM BLX 立即数（安全区 Thumb SWI 桩入口）+ ARM7 Halt 中断后跳过 SWI：FFXII 越过 BA94/IPC 死锁，双核推进到系统空闲（723 项 0 失败） | [cpu](src/cpu/cpulog.md) 路 [io](src/io/iolog.md) 路 [tests](tests/testlog.md) 路 [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9r（分析） | 工程 | trace 定位 IPC 死锁：ARM9 0x0200B8C8 等 IPCSYNC 回应、ARM7 0x03807524 等 FIFO，请求处理器 0x037FEB24 未被触发（仅文档） | [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9q | IO/测试 | DMA 控制器按核拆成两套（ARM9/ARM7 各 4 通道），VBlank/卡带触发双核各自点火（718 项 0 失败） | [io](src/io/iolog.md) · [tests](tests/testlog.md) |
| 2026-09-06 | 21-B9p | IO/测试 | DMA 完成中断接线：搬完且 CNT bit14 置位时置 IF bit8-11（718 项 0 失败） | [io](src/io/iolog.md) · [tests](tests/testlog.md) |
| 2026-09-06 | 21-B9o | IO/测试 | 定时器按核拆成两套（ARM9/ARM7 各 4 个，真机 8 个）；中断分流不再用共享定时器（716 项 0 失败） | [io](src/io/iolog.md) · [cpu](src/cpu/cpulog.md) · [tests](tests/testlog.md) |
| 2026-09-06 | 21-B9n | IO/CPU/测试 | 电源/启动寄存器 POWCNT1/2+POSTFLG+WIFIWAITCNT 默认值 + ARM9 WFI 等待语义 + `--screenshot` 诊断；FFXII 越过空闲死锁继续启动（716 项 0 失败） | [io](src/io/iolog.md) · [cpu](src/cpu/cpulog.md) · [main](src/mainlog.md) · [tests](tests/testlog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9l | BIOS/测试 | ARM7 SWI 0x08 SoundBias（仅 NDS7）；FFXII ARM7 不再因 unknown SWI 落入低地址漂移（704 项 0 失败） | [bios](src/bios/bioslog.md) · [tests](tests/testlog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9k | IO/卡带/测试 | ARM9 硬件除法/开方寄存器（DIV/SQRT）+ ROMCTRL bit31 忙位/块结束语义；FFXII 卡带读块循环推进到启动服务（700 项 0 失败） | [io](src/io/iolog.md) · [cart](src/cart/cartlog.md) · [tests](tests/testlog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9j+ | IO/测试 | SPI device1 固件 Flash 完整事务状态机 + CRC 合法用户设置镜像；FFXII 固件序列错位修复，ARM9 离开忙等进入 ROM 装载（675 项 0 失败） | [io](src/io/iolog.md) · [tests](tests/testlog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9j（后半之二） | CPU | ARM7 IRQ 入口帧预填：0x37FBA10 不再从空 IRQ 栈读 0，3200 万步内 ARM7 不跑飞（665 项 0 失败） | [cpu](src/cpu/cpulog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9j（后半） | IO/测试 | SPI device1（固件 Flash）最小回读 0xFF；ARM9 首次离开忙等推进到 0x0200B838 服务循环（665 项检查 0 失败） | [io](src/io/iolog.md) · [tests](tests/testlog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-05 | 21-B9j（前半） | IO/测试 | 定时器 CNT_L reload 语义 + VBlank 双核置 IF；ARM7 能收到帧事件，service6 完成回执仍待追（664 项检查 0 失败） | [io](src/io/iolog.md) · [tests](tests/testlog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-05 | 21-B9i | CPU/IO/测试 | FIFO CNT 合并写修复（错误应答不吞使能位）+ ARM7 IRQ 槽跳板（0x0380FFFC）；ARM9 FIFO 命令能送达 ARM7 并完成 handler（659 项检查 0 失败），忙位回执待 B9j | [cpu](src/cpu/cpulog.md) · [io](src/io/iolog.md) · [tests](tests/testlog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-05 | 21-B9h | CPU/IO/测试 | LDM/STM ^ 的 User 槽语义（FFXII 任务队列不再成环）；VBlank 改回 bit0、Timer0-3 溢出置 IF bit3-6；headless 进度带 CPSR/IRQ 状态（649 项检查 0 失败），多帧 VBlank IRQ 可正常进入/恢复，新卡点 0x0200EA84 忙位待 B9i | [cpu](src/cpu/cpulog.md) · [io](src/io/iolog.md) · [tests](tests/testlog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-05 | 21-B9g | CPU | 模式私有 r13/r14（banked registers）：MSR/异常/返回统一走模式同步；FFXII 三栈隔离，ARM9 不再跑飞（643 项 0 失败） | [cpu](src/cpu/cpulog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-05 | 21-B9f | CPU | IRQ HLE 桩：跳 handler 前保存 r0-r3/r12/CPSR/返回点，返回后恢复；第一次 VBlank 链执行完，新卡点=模式共用 SP（633 项 0 失败） | [cpu](src/cpu/cpulog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-05 | 21-B9e | CPU | ARM BLX Rm 寄存器间接调用；ARM9 越过 0x02006488 回到主等待循环，8M 步无固定卡点（628 项 0 失败） | [cpu](src/cpu/cpulog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-05 | 21-B9d | CPU | CLZ 前导零指令（ARMv5）；FFXII IRQ 分发循环跑完，ARM9 前进到 0x02006488，新卡点=ARM BLX Rm（618 项 0 失败） | [cpu](src/cpu/cpulog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-05 | 21-B9c | CPU | ARM9 IRQ 槽跳板：异常后按 DTCM+0x3FFC 槽跳用户 handler；FFXII 进入 ITCM 中断分发器，新卡点=CLZ 缺失（608 项 0 失败） | [cpu](src/cpu/cpulog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-05 | 21-B9b | CPU | 首中断现场快照（IME/IE/IF + IRQ 槽）；确认 FFXII 把 IRQ handler 装在 DTCM+0x3FFC=0x01FF8000，异常 trace 加核标识（600 项 0 失败） | [cpu](src/cpu/cpulog.md) · [21](docs/21-rom-bringup.md) |
| 2026-08-08 | 0.1 | 窗口/主循环 | 安装 WinLibs 工具链（gcc 16.1 / cmake 4.4） | [main](src/mainlog.md) |
| 2026-08-08 | 0.2 | 窗口/主循环 | CMakeLists + main.c 打印 `nds-emu` | [main](src/mainlog.md) |
| 2026-08-08 | 0.3 | 窗口/主循环 | FetchContent 本地 URL 引入 SDL2 2.26.3 | [main](src/mainlog.md) |
| 2026-08-08 | — | 工程管理 | git 分支工作流（main 保护 / develop 默认）+ 开发日志体系建立 | [main](src/mainlog.md) |
| 2026-08-08 | — | 窗口/菜单 | 拆分 window 与 menu 为独立模块 | [main](src/mainlog.md) · [window](src/window/windowlog.md) · [menu](src/menu/menulog.md) |
| 2026-08-08 | P3 | 预习 | 新增 `docs/00b-hex-endianness.md`：十六进制与小端、`bus_read/write` 拼接影响 | [00b](docs/00b-hex-endianness.md) |
| 2026-08-08 | 0.5 | 整机状态 | 定义空 `nds_t`（`nds.h`/`nds.c` + create/destroy），`main` 创建一份 | [nds](src/nds/ndslog.md) |
| 2026-08-08 | 0.6 | 窗口/主循环 | 顶/底屏两种纯色区分（上蓝下绿），`SCREEN_W/H` 单屏常量 | [main](src/mainlog.md) |
| 2026-08-08 | 1.1 | 预习/装载 | 新增 `docs/02-nds-rom-header.md`：ARM9/ARM7 头四字段与偏移 | [02](docs/02-nds-rom-header.md) |
| 2026-08-08 | 1.2 | 卡带装载 | `cart` 读整个文件到缓冲区，`argv[1]` 传路径，打印文件大小 | [cart](src/cart/cartlog.md) |
| 2026-08-08 | 1.3 | 卡带装载 | 按偏移解析 ARM9 头四字段并打印（小端 `read_le32`） | [cart](src/cart/cartlog.md) |
| 2026-08-08 | 1.3+ | 卡带装载 | 修复中文/Unicode ROM 路径打不开（`cart_load_w` + `CommandLineToArgvW`） | [cart](src/cart/cartlog.md) |
| 2026-08-08 | 1.4 | 卡带装载 | cart 拆 arm9/arm7 功能文件（接口+功能），解析 ARM7 头四字段 | [cart](src/cart/cartlog.md) |
| 2026-08-08 | 1.5 | 卡带装载 | 最小假 `.nds`：`homebrew/make_fake_rom.py` + `mini.nds` | [cart](src/cart/cartlog.md) |
| 2026-08-08 | 1.6 | 卡带装载 | ARM9 镜像拷入 RAM 缓冲区（裸数组过渡），首字节对照 | [cart](src/cart/cartlog.md) |
| 2026-08-08 | 1.7 | 卡带装载 | CLI 装载摘要 `=== NDS cartridge ===`（file/arm9/arm7/image） | [cart](src/cart/cartlog.md) |
| 2026-08-08 | 2.1 | 内存总线 | 新增 `docs/03-memory-map.md`：Main RAM / VRAM / IO 地址区间与换算 | [03](docs/03-memory-map.md) |
| 2026-08-08 | 2.2 | 内存总线 | `bus` 分配 Main RAM/VRAM/IO 数组，挂到 `nds_t`，`calloc` 清零 | [bus](src/bus/buslog.md) |
| 2026-08-08 | 2.3 | 内存总线 | `bus_read8/write8` + `bus_resolve` 区间命中换算，未映射读 0 写忽略 | [bus](src/bus/buslog.md) |
| 2026-08-08 | 2.4 | 内存总线 | `read16/write16` 小端拼拆（写 `0xABCD` 字节序 `CD AB`） | [bus](src/bus/buslog.md) |
| 2026-08-08 | 2.5 | 内存总线 | `read32/write32` 小端拼拆（`0x12345678` → `78 56 34 12`） | [bus](src/bus/buslog.md) |
| 2026-08-08 | 2.6 | 内存总线 | VRAM 区间映射（`0x06000000` 656KB），写读回一致 | [bus](src/bus/buslog.md) |
| 2026-08-08 | 2.7 | 内存总线 | IO 桩：`0x04000000` 区间读 0、写忽略，访问不崩 | [bus](src/bus/buslog.md) |
| 2026-08-08 | 2.8 | 内存总线 | 装载衔接：ARM9 镜像经 `bus_write8` 装入 Main RAM，`bus_read32` 读回 `EAFFFFFE` | [bus](src/bus/buslog.md) |
| 2026-08-08 | 3a.1 | CPU | 新增 `docs/04-cpu-loop.md`：取指-执行循环、PC、B 指令位模式 | [04](docs/04-cpu-loop.md) |
| 2026-08-08 | 3a.2 | CPU | `arm_cpu_t`：`r[16]`（r15=PC）、`cpsr`、`cycles`，挂入 `nds_t` | [cpu](src/cpu/cpulog.md) |
| 2026-08-08 | 3a.3 | CPU | `cpu_fetch`：按 PC 从 bus 读 32 位指令字 | [cpu](src/cpu/cpulog.md) |
| 2026-08-08 | 3a.4 | CPU | `cpu_step`：取指后 PC+=4（非分支默认推进） | [cpu](src/cpu/cpulog.md) |
| 2026-08-08 | 3a.5 | CPU | 识别「无条件 B 跳自己」死循环（`EAFFFFFE`），PC 原地打转 | [cpu](src/cpu/cpulog.md) |
| 2026-08-08 | 3a.6 | CPU | 未实现指令：打印机器码并计数（阶段 3b 逐类实现） | [cpu](src/cpu/cpulog.md) |
| 2026-08-08 | 3a.7 | CPU | 主循环每帧执行固定 N 步；修复 `mini.nds` 布局（ram=entry） | [cpu](src/cpu/cpulog.md) |
| 2026-08-08 | 3b.1 | CPU | 条件码执行框架（15 种条件按 N/Z/C/V 判断，不满足跳过） | [cpu](src/cpu/cpulog.md) |
| 2026-08-08 | 3b.2 | CPU | `MOV` 立即数（`imm8 ROR (rot4*2)` 旋转编码） | [cpu](src/cpu/cpulog.md) |
| 2026-08-08 | 3b.3 | CPU | `ADD`/`SUB` 立即数或寄存器，更新进位/溢出标志 | [cpu](src/cpu/cpulog.md) |
| 2026-08-08 | 3b.4 | CPU | `CMP` + flags 更新（N/Z/C/V，只比较不写回） | [cpu](src/cpu/cpulog.md) |
| 2026-08-08 | 3b.5 | CPU | `LDR` 立即偏移（前变址 ±offset，`bus_read32`） | [cpu](src/cpu/cpulog.md) |
| 2026-08-08 | 3b.6 | CPU | `STR` 立即偏移（`bus_write32`） | [cpu](src/cpu/cpulog.md) |
| 2026-08-08 | 3b.7 | CPU | `B` 相对跳转（PC+8 语义）；修复分支符号扩展 bug | [cpu](src/cpu/cpulog.md) |
| 2026-08-08 | 3b.8 | CPU | `BL`+`BX lr` 最小调用返回（lr=PC+4） | [cpu](src/cpu/cpulog.md) |
| 2026-08-08 | 重构 | CPU | 指令执行拆分为 `src/cpu/exec.h/.c` 功能文件（接口 `cpu.h/.c` 只留框架），行为不变 | [cpu](src/cpu/cpulog.md) |
| 2026-08-08 | 3b.9 | CPU | `AND`/`ORR`/`EOR` 位运算（逻辑运算按 S 位更新 N/Z） | [cpu](src/cpu/cpulog.md) |
| 2026-08-08 | 3b.10 | CPU | 综合：纯机器码把 RGB555 红色 `0x7C00` 写进 VRAM 并读回，阶段 3b 完成 | [cpu](src/cpu/cpulog.md) |
| 2026-08-09 | 4.1 | 显示 | 新增 `docs/05-framebuffer.md`：像素与 RGB555/888、线性 framebuffer | [05](docs/05-framebuffer.md) |
| 2026-08-09 | 4.2 | 显示 | 约定：顶屏=VRAM 起 256×192 RGB555，底屏=VRAM+`0x18000` | [ppu](src/ppu/ppulog.md) |
| 2026-08-09 | 4.3 | 显示 | `ppu` 模块：`bus_read16` 读 VRAM → RGB555 转 RGB888 → SDL 纹理上屏 | [ppu](src/ppu/ppulog.md) |
| 2026-08-09 | 4.4 | 显示 | 顶屏 6 色带+白框测试图、底屏纯蓝（主机直写 VRAM 验证管线） | [ppu](src/ppu/ppulog.md) |
| 2026-08-09 | 4.5 | 显示 | 模拟 CPU 跑写 VRAM 的测试码出图（黄十字顶屏+底屏纯绿），修复 3 个手工汇编编码错误 | [ppu](src/ppu/ppulog.md) |
| 2026-08-09 | 4.6 | 显示 | 默认 2× 缩放启动，菜单可切 1x/2x，双屏比例不变，阶段 4 完成 | [ppu](src/ppu/ppulog.md) |
| 2026-08-09 | 5.1 | 测试 | 抽 `ndscore` 核心库 + `tests/test_nds.c` 统一测试入口 + `ctest`，迁移 bus/3b/4.5 自测 | [tests](tests/testlog.md) |
| 2026-08-09 | 5.2 | 测试 | 用例：清屏（CPU 整屏填约定底色 `0x0000`，先涂乱码再验证全清） | [tests](tests/testlog.md) |
| 2026-08-09 | 5.3 | 测试 | 用例：画矩形（左上象限 128×96 红）；修复「STR 32 位写只着 1 像素」bug | [tests](tests/testlog.md) |
| 2026-08-09 | 5.4 | 测试 | 用例：死循环保活（10 万步不崩、PC 稳定、cycles=100000） | [tests](tests/testlog.md) |
| 2026-08-09 | 5.5 | 测试 | README 新增「运行测试」小节（构建/ctest/直接运行/退出码/新增用例） | [tests](tests/testlog.md) |
| 2026-08-09 | 5.6 | 测试 | 新增 `docs/06-devkitarm.md`：未来 devkitARM 接入说明（仅文档） | [tests](tests/testlog.md) |
| 2026-08-09 | 4.5修 | 显示 | 修复十字 demo：竖线只画上半屏（终点 `+0xC000` 应 `+0x18000`）；横线 1px vs 竖线 2px 粗细不一（横线改双行嵌套循环） | [ppu](src/ppu/ppulog.md) |
| 2026-08-09 | 6.1 | 预习 | 新增 `docs/07-interrupts.md`：中断=CPU 被打断去跑处理程序；IME/IE/IF 三者配合 | [07](docs/07-interrupts.md) |
| 2026-08-09 | 6.2 | IO 寄存器 | 新建 `src/io/` 模块（接口 + irq/timer/key 功能文件）；bus IO 桩改路由 io 模块；IME/IE/IF 读写、IF 写 1 清除 | [io](src/io/iolog.md) |
| 2026-08-09 | 6.3 | IO 寄存器 | 指令计数近似产生 VBlank：主循环每帧跑完 N 步 `io_set_vblank`，IF bit3 置位 | [io](src/io/iolog.md) |
| 2026-08-09 | 6.4 | IO 寄存器 | 最小 IRQ 检测：`(IF & IE & IME)!=0`，首次 pending 打印一次（不进异常向量） | [io](src/io/iolog.md) |
| 2026-08-09 | 6.5 | IO 寄存器 | Timers 0-3：CNT_L/H、bit7 使能、bit0-1 分频(1/64/256/1024)、每条指令 tick；修字节偏移 `<<32` UB bug | [io](src/io/iolog.md) · [cpu](src/cpu/cpulog.md) |
| 2026-08-09 | 6.6 | IO 寄存器 | KEYINPUT（按下=0）+ SDL 键映射（main.c 组合根，window/menu 不依赖机器） | [io](src/io/iolog.md) · [main](src/mainlog.md) |
| 2026-08-09 | 6.7 | 测试 | 等 VBlank 轮询程序（置位前卡循环、置位后写屏）+ 读键程序（按 A 蓝/未按红）；修 3 个手写汇编编码错误 | [tests](tests/testlog.md) |
| 2026-08-09 | 6 完成 | 收尾 | 阶段 6 完成：io 模块 + VBlank/IRQ 检测/定时器/按键；70 项检查 0 失败 | [io](src/io/iolog.md) · [tests](tests/testlog.md) |
| 2026-08-09 | 7.1 | 预习 | 新增 `docs/08-dma.md`：DMA = 硬件搬内存（源/目的/长度/启动，CPU 不参与） | [08](docs/08-dma.md) |
| 2026-08-09 | 7.2 | IO 寄存器 | `src/io/dma.h/.c` 功能文件：DMA0 的 SAD/DAD/CNT_L/CNT_H 读写；io 加 bus 反指并路由 | [io](src/io/iolog.md) |
| 2026-08-09 | 7.3 | IO 寄存器 | 立即模式：写 CNT_H 使能位同步拷贝（16/32 位块、源/目的可固定），搬完自动清使能 | [io](src/io/iolog.md) |
| 2026-08-09 | 7.4 | 测试 | CPU 程序触发 DMA 填 VRAM 色块（128 像素红）；修 `ADD #0x1000` 立即数旋转编码 bug | [tests](tests/testlog.md) |
| 2026-08-09 | 7 完成 | 收尾 | 阶段 7 完成：DMA0 立即模式搬运/填色；90 项检查 0 失败 | [io](src/io/iolog.md) · [tests](tests/testlog.md) |
| 2026-08-09 | 8.1 | 预习 | 新增 `docs/09-dual-core.md`：双核分工、共享内存、IPC FIFO 概念 | [09](docs/09-dual-core.md) |
| 2026-08-09 | 8.2 | CPU | 拆分 `arm9.h/.c`/`arm7.h/.c` 功能文件（`cpu.h/.c` 作接口），`arm_cpu_t` 加 `is_arm7` | [cpu](src/cpu/cpulog.md) |
| 2026-08-09 | 8.3 | 内存总线 | ARM7 WRAM（`0x03800000` 64KB）映射 + 装载 ARM7 镜像 + cpu7 指入口 | [bus](src/bus/buslog.md) · [nds](src/nds/ndslog.md) · [main](src/mainlog.md) |
| 2026-08-09 | 8.4 | CPU | 双核交错调度 ARM9:ARM7=2:1（主循环 + 测试驱动） | [main](src/mainlog.md) · [cpu](src/cpu/cpulog.md) |
| 2026-08-09 | 8.5 | IO 寄存器 | 中断按 CPU 分流：IME/IE/IF 拆成 `irq[2]` 两套，同址按访问者身份选择 | [io](src/io/iolog.md) |
| 2026-08-09 | 8.6 | IO 寄存器 | IPC FIFO 完整：双向 16 字队列 + CNT 状态位 + SEND/RECV + 边沿中断 IF17/18 | [io](src/io/iolog.md) |
| 2026-08-09 | 8.7 | 测试 | 双核 FIFO 传值 + 底屏体现；117 项检查 0 失败 | [tests](tests/testlog.md) |
| 2026-08-09 | 8 完成 | 收尾 | 阶段 8 完成：ARM7 + IPC（双核调度、共享内存、FIFO 传字） | [cpu](src/cpu/cpulog.md) · [io](src/io/iolog.md) · [tests](tests/testlog.md) |
| 2026-08-14 | 9.1 | 显示 | 新增 `docs/10-bg-tile-palette.md`：BG/tile/tilemap/调色板、位图 vs tile 模式 | [10](docs/10-bg-tile-palette.md) |
| 2026-08-14 | 9.2 | 显示/IO | 新建 `src/io/disp.h/.c` 显示寄存器（DISPCNT/BGxCNT/滚动，主副引擎）+ bus 调色板 RAM/OAM/VRAM 固定窗口 | [disp](src/io/iolog.md) · [bus](src/bus/buslog.md) |
| 2026-08-14 | 9.3 | 显示 | 新建 `src/ppu/render.h/.c` 纯渲染器：直色位图模式（Mode 3-5，BG2/BG3 直色 16bpp） | [ppu](src/ppu/ppulog.md) |
| 2026-08-14 | 9.4 | 显示 | Mode 0 tile 图层渲染（4bpp/8bpp 位面、tilemap、调色板、透明、翻转、优先级） | [ppu](src/ppu/ppulog.md) |
| 2026-08-14 | 9.5 | 显示 | Engine B 对称渲染（副 DISPCNT/BGxCNT/调色板/VRAM 窗口 `0x06200000`） | [ppu](src/ppu/ppulog.md) |
| 2026-08-14 | 9.6 | 显示 | OBJ 最小：OAM 读取 + 1D tile 映射 + OBJ 调色板，一个 8×8 sprite（16/256 色） | [ppu](src/ppu/ppulog.md) |
| 2026-08-14 | 9.7 | 显示/测试 | 自造数据 2D 场景验收 + main 演示迁移（弃假 FB，改 DISPCNT/BGxCNT/OAM 驱动）；148 项检查 0 失败 | [ppu](src/ppu/ppulog.md) · [tests](tests/testlog.md) · [main](src/mainlog.md) |
| 2026-08-14 | 9 完成 | 收尾 | 阶段 9 完成：真 2D PPU（DISPCNT + BG tile + OBJ，主副引擎对称） | [ppu](src/ppu/ppulog.md) |
| 2026-08-15 | 10.1 | 预习/CPU | 新增 `docs/11-arm-instructions-full.md`：真码为何需要移位/MRS/MSR/LDM/STM/乘法/半字访存/SWI | [11](docs/11-arm-instructions-full.md) |
| 2026-08-15 | 10.2 | CPU | 移位操作数 LSL/LSR/ASR/ROR（立即 + 寄存器移位）+ 进位输出 | [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 10.3 | CPU | 更多数据处理 MVN/BIC/ADC/SBC/RSB/RSC/TST/TEQ/CMN | [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 10.4 | CPU | MRS/MSR 读写 CPSR/SPSR（字段掩码 + 模式位保护）；cpu.h 增 spsr/swi_num/cp15[16] | [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 10.5 | CPU | LDM/STM（PUSH/POP 别名，IA/IB/DA/DB + 写回） | [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 10.6 | CPU | 乘法 MUL/MLA + 长乘 UMULL/UMLAL/SMULL/SMLAL | [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 10.7 | CPU | 字节/半字访存 LDRB/STRB/LDRH/STRH/LDRSB/LDRSH + 前后变址/寄存器偏移 | [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 10.8 | CPU | SWI 记录软件中断号（暂不跳异常，阶段 11 BIOS HLE 拦截） | [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 10.9 | CPU | SWP/SWPB 寄存器与内存交换 | [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 10.10 | CPU | MRC/MCR 协处理器访问（CP15 桩，按 CRn 索引） | [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 10.11 | CPU/测试 | 综合真码（PUSH/POP + SWI + MUL + STRH 循环搬 VRAM）；204 项检查 0 失败 | [cpu](src/cpu/cpulog.md) · [tests](tests/testlog.md) |
| 2026-08-15 | 10 完成 | 收尾 | 阶段 10 完成：ARM 指令集补全 + SWI（移位/MRS/MSR/LDM/STM/乘法/半字访存/SWI/SWP/MRC/MCR） | [cpu](src/cpu/cpulog.md) · [tests](tests/testlog.md) |
| 2026-08-15 | 11.1 | 预习/BIOS | 新增 `docs/12-bios-hle.md`：BIOS/SWI/HLE 概念 + NDS SWI 编号表 + 寄存器约定 | [12](docs/12-bios-hle.md) |
| 2026-08-15 | 11.2 | BIOS | 新建 `src/bios/` 模块：`bios_dispatch` 分发框架；exec.c SWI 改 HLE 拦截（等待不前进 PC） | [bios](src/bios/bioslog.md) |
| 2026-08-15 | 11.3 | BIOS | Div(0x09)/Sqrt(0x0D)；注 DivArm 为 GBA 专属 | [bios](src/bios/bioslog.md) |
| 2026-08-15 | 11.4 | BIOS | CpuSet(0x0B)/CpuFastSet(0x0C) 16/32 位搬移与填充 | [bios](src/bios/bioslog.md) |
| 2026-08-15 | 11.5 | BIOS | 解压 BitUnPack(0x10)/LZ77(0x11)/RL(0x14)/Huffman(0x13) | [bios](src/bios/bioslog.md) |
| 2026-08-15 | 11.6 | BIOS | 等待 Halt(0x06)/IntrWait(0x04)/VBlankIntrWait(0x05)（PC 不动等价等待） | [bios](src/bios/bioslog.md) |
| 2026-08-15 | 11.7 | BIOS/测试 | 综合真码（LZ77 解压到 VRAM + Div + Sqrt 串行）；256 项检查 0 失败 | [bios](src/bios/bioslog.md) · [tests](tests/testlog.md) |
| 2026-08-15 | 11 完成 | 收尾 | 阶段 11 完成：BIOS HLE + SWI 分发（`src/bios/` 模块 + 除法/开方/搬移/解压/等待） | [bios](src/bios/bioslog.md) · [tests](tests/testlog.md) |
| 2026-08-15 | 12.1 | 预习/CPU | 新增 `docs/13-exceptions-modes.md`：特权模式/CPSR 模式位/异常向量表 | [13](docs/13-exceptions-modes.md) · [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 12.2 | CPU | 异常向量：`arm_exception` + `vector_base`（ARM9 高/ARM7 低）；未定义→0x04、未知 SWI→0x08 | [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 12.3 | CPU | CPSR 模式切换 + SPSR 分槽 `spsr[5]` 保存/恢复（MSR 切模式、`SUBS pc`/`LDM ^` 恢复）；修「写 PC 后又 +4」跳转 bug | [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 12.4 | CPU | CP15 c1 的 V 位联动 `vector_base`（cache/MMU 使能位先存不生效） | [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 12.5 | CPU | IRQ 真实响应：取指前查 pending 进 handler、`SUBS pc,lr,#4` 返回 | [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 12 完成 | 收尾 | 阶段 12 完成：CP15 + 异常向量 + 中断真实化；282 项检查 0 失败 | [cpu](src/cpu/cpulog.md) · [tests](tests/testlog.md) |
| 2026-08-15 | 13.1 | 预习/CPU | 新增 `docs/14-thumb.md`：Thumb 16 位编码 / T 位 / BX-BLX 切换 / PC 约定 | [14](docs/14-thumb.md) · [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 13.2 | CPU | Thumb 译码框架：`thumb.c` + `thumb_step` + `cpu_fetch16` + T 位分发 | [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 13.3 | CPU | Thumb 数据处理：移位/立即数/16 种 ALU（全更新标志） | [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 13.4 | CPU | Thumb 访存：字/字节/半字/SP 相对/寄存器偏移/字面量池 | [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 13.5 | CPU | Thumb 分支 B/BL/BX/BLX/条件分支 + PUSH/POP + STMIA/LDMIA | [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 13.6 | CPU | Thumb 杂项：高寄存器 ADD/CMP/MOV、取地址、SWI 进 HLE | [cpu](src/cpu/cpulog.md) |
| 2026-08-15 | 13.7 | CPU/测试 | Thumb 真码写 VRAM + 调 SWI；336 项检查 0 失败 | [cpu](src/cpu/cpulog.md) · [tests](tests/testlog.md) |
| 2026-08-15 | 13 完成 | 收尾 | 阶段 13 完成：Thumb 指令集（16 位译码 + 全指令语义 + T 位切换） | [cpu](src/cpu/cpulog.md) · [tests](tests/testlog.md) |
| 2026-08-15 | 14.1 | 预习/卡带装载 | 新增 `docs/15-secure-area.md`：安全区加密（KEY1/Blowfish + 子密钥派生 + encryObj） | [15](docs/15-secure-area.md) |
| 2026-08-15 | 14.2 | 卡带装载 | KEY1（Blowfish）实现：块加解密 + 三级密钥调度 + 密钥表常量 0x1048 字节 | [cart](src/cart/cartlog.md) |
| 2026-08-15 | 14.3 | 卡带装载 | 安全区解密 + 游戏子密钥派生 + `cart_decrypt_secure_area` | [cart](src/cart/cartlog.md) |
| 2026-08-15 | 14.4 | 卡带装载/主循环 | 装载时解密安全区后再拷 RAM（`main.c` 接入） | [cart](src/cart/cartlog.md) · [main](src/mainlog.md) |
| 2026-08-15 | 14.5 | 测试 | 自制含加密安全区 ROM 完整装载；360 项检查 0 失败 | [tests](tests/testlog.md) |
| 2026-08-15 | 14 完成 | 收尾 | 阶段 14 完成：商业 ROM 装载 + 安全区解密（KEY1/Blowfish） | [cart](src/cart/cartlog.md) · [tests](tests/testlog.md) |
| 2026-08-15 | 15.1 | 预习/卡带装载 | 新增 `docs/16-cartridge-protocol.md`：卡带命令协议（ROMCTRL/AUXSPICNT/CARD_COMMAND/CARD_DATA + B7/B8） | [16](docs/16-cartridge-protocol.md) · [cart](src/cart/cartlog.md) |
| 2026-08-15 | 15.2 | 卡带装载 | 新建 `cartbus`：ROMCTRL/命令解码 + CARD_DATA 数据端口（瞬时传输模型） | [cart](src/cart/cartlog.md) |
| 2026-08-15 | 15.3 | IO 寄存器 | DMA 补全：4 通道 + 地址控制（增/减/固定）+ repeat + 触发源（立即/VBlank/卡带） | [io](src/io/iolog.md) |
| 2026-08-15 | 15.4 | 卡带装载/IO/主循环 | io/bus/nds 接线 + `main.c` 挂载卡带 + 卡带 DMA 读路径 + 卡带 IRQ（IF bit19） | [cart](src/cart/cartlog.md) · [io](src/io/iolog.md) · [main](src/mainlog.md) |
| 2026-08-15 | 15.5 | 测试 | 综合：卡带命令读 + DMA 从 ROM 搬数据到 RAM（单元 + CPU 程序）；393 项检查 0 失败 | [tests](tests/testlog.md) |
| 2026-08-15 | 15 完成 | 收尾 | 阶段 15 完成：卡带协议 + DMA 补全（流式读卡带，真游戏运行时路径） | [cart](src/cart/cartlog.md) · [io](src/io/iolog.md) · [tests](tests/testlog.md) |
| 2026-08-15 | 16.1 | 预习/卡带装载 | 新增 `docs/17-save-memory.md`：存档类型（EEPROM/Flash/FRAM）+ AUXSPI 协议 | [17](docs/17-save-memory.md) · [cart](src/cart/cartlog.md) |
| 2026-08-15 | 16.2 | 卡带装载 | 新建 `save`：存档芯片 SPI 状态机（EEPROM/Flash 命令）+ cartbus 接 AUXSPI | [cart](src/cart/cartlog.md) · [io](src/io/iolog.md) |
| 2026-08-15 | 16.3 | 卡带装载/主循环 | 存档持久化：`save_load_file/save_save_file` + `main.c` 装载/写回 .sav | [cart](src/cart/cartlog.md) · [main](src/mainlog.md) |
| 2026-08-15 | 16.4 | 测试 | 综合：游戏经 AUXSPICNT/AUXSPIDATA 读写存档（单元 + CPU 程序）；423 项检查 0 失败 | [tests](tests/testlog.md) |
| 2026-08-15 | 16 完成 | 收尾 | 阶段 16 完成：存档（EEPROM/Flash + .sav 持久化，游戏能保存进度） | [cart](src/cart/cartlog.md) · [tests](tests/testlog.md) |
| 2026-08-15 | 17.1 | 预习/IO | 新增 `docs/18-touch-spi.md`：触摸屏 + 主 SPI 总线（SPICNT/SPIDATA + TSC 命令协议） | [18](docs/18-touch-spi.md) · [io](src/io/iolog.md) |
| 2026-08-15 | 17.2 | IO 寄存器 | 新建 `touch`：SPICNT/SPIDATA + TSC 命令状态机（12/8 位回传、X/Y/Z/电池通道）+ io 接线 | [io](src/io/iolog.md) |
| 2026-08-15 | 17.3 | 测试 | 综合：CPU 程序经 SPICNT/SPIDATA 读出 X/Y 坐标（单元 + 集成）；443 项检查 0 失败 | [tests](tests/testlog.md) |
| 2026-08-15 | 17 完成 | 收尾 | 阶段 17 完成：触摸 SPI（SPICNT/SPIDATA + TSC 坐标读取，游戏能读触控输入） | [io](src/io/iolog.md) · [tests](tests/testlog.md) |
| 2026-08-15 | 18.1 | 预习/音频 | 新增 `docs/19-audio.md`：16 通道音频（PCM8/PCM16/IMA-ADPCM/PSG）+ 寄存器/混音 | [19](docs/19-audio.md) · [snd](src/snd/sndlog.md) |
| 2026-08-15 | 18.2 | 音频 | 新建 `snd`：16 通道寄存器（SOUNDxCNT/SAD/TMR/PNT/LEN + SOUNDCNT/SOUNDBIAS）+ io 路由 | [snd](src/snd/sndlog.md) · [io](src/io/iolog.md) |
| 2026-08-15 | 18.3 | 音频 | 混音器：PCM8/PCM16/IMA-ADPCM/PSG 合成 + 音量/声相/主音量/偏置 + 单发/循环 | [snd](src/snd/sndlog.md) |
| 2026-08-15 | 18.4 | 音频/主循环 | SDL 音频回调 + `main.c` 接线 + demo 提示音（~256Hz 方波） | [snd](src/snd/sndlog.md) · [main](src/mainlog.md) |
| 2026-08-15 | 18.5 | 测试 | 综合：寄存器读写 + 混音样本 + CPU 程序配置通道；467 项检查 0 失败 | [tests](tests/testlog.md) |
| 2026-08-15 | 18 完成 | 收尾 | 阶段 18 完成：音频（16 通道 PCM/ADPCM/PSG 混音 + SDL 回调发声，游戏能出声音） | [snd](src/snd/sndlog.md) · [tests](tests/testlog.md) |
| 2026-08-15 | 19.1 | 预习/3D | 新增 `docs/20-3d.md`：3D 几何引擎（GE/RE、GXFIFO 命令、矩阵/顶点定点格式、图元） | [20](docs/20-3d.md) · [gx](src/gx/gxlog.md) |
| 2026-08-15 | 19.2 | 3D 几何 | 新建 `gx`：DISP3DCNT/GXSTAT/RAM_COUNT 寄存器 + GXFIFO/命令端口 32 位写 + 命令 FIFO 解码 | [gx](src/gx/gxlog.md) · [io](src/io/iolog.md) · [bus](src/bus/buslog.md) |
| 2026-08-15 | 19.3 | 3D 几何 | 几何命令：矩阵（LOAD/MULT/SCALE/TRANS/PUSH/POP/IDENTITY）+ 顶点提交 + 变换（pos×proj→透视除→视口） | [gx](src/gx/gxlog.md) |
| 2026-08-15 | 19.4 | 3D 几何/显示 | 三角形软件光栅化（半平面判定）+ render.c 合成 3D 图层进 2D 顶屏（DISP3DCNT 使能位） | [gx](src/gx/gxlog.md) · [ppu](src/ppu/ppulog.md) |
| 2026-08-15 | 19.5 | 测试 | 综合：顶点变换 + 光栅化 + GXFIFO 命令流出图 + 3D 图层合成；483 项检查 0 失败 | [tests](tests/testlog.md) |
| 2026-08-15 | 19 完成 | 收尾 | 阶段 19 完成：3D 几何引擎（GXFIFO 命令流 + 矩阵/顶点变换 + 软件光栅化 + 3D 图层合成） | [gx](src/gx/gxlog.md) · [tests](tests/testlog.md) |
| 2026-08-15 | 20.1 | 显示 | 仿射（旋转/缩放）背景：BGxPA-PD(1.7.8)/X-Y(1.19.8) 寄存器 + 逆仿射采样 + MODE1/2 出图 | [disp](src/io/iolog.md) · [ppu](src/ppu/ppulog.md) |
| 2026-08-15 | 20.2 | 显示/IO | 每像素合成器 + 颜色特效：Alpha 混合/增亮/减暗 + MASTER_BRIGHT + 显示捕获（顶屏→LCDC VRAM） | [ppu](src/ppu/ppulog.md) · [disp](src/io/iolog.md) · [bus](src/bus/buslog.md) |
| 2026-08-15 | 20.3 | 显示 | 窗口（WIN0/WIN1 限定图层显示区域）+ 更多混合效果；修 16 位寄存器高字节写入丢失 bug | [ppu](src/ppu/ppulog.md) · [disp](src/io/iolog.md) |
| 2026-08-15 | 20 完成 | 收尾 | 阶段 20 完成：2D PPU 补全（旋转/缩放 + 混合/亮度 + 窗口 + 显示捕获）；521 项检查 0 失败 | [ppu](src/ppu/ppulog.md) · [tests](tests/testlog.md) |
| 2026-08-15 | 21-A1 | 卡带装载/主循环 | 修 ARM7 镜像装载按目标区判大小（FFXII ARM7 载入 Main RAM 0x02380000，165KB） | [main](src/mainlog.md) |
| 2026-08-15 | 21-A2 | 诊断 | bring-up 诊断设施：`bus.diag` 开关 + 未知 SWI/未实现指令/未知 IO 只打一次 + `--headless N`/`--trace` 命令行 | [main](src/mainlog.md) · [bus](src/bus/buslog.md) · [io](src/io/iolog.md) · [bios](src/bios/bioslog.md) |
| 2026-08-15 | 21-A3 | CPU | 修 ARM r15 读缺 +8（`read_reg` 统一修正 PC 相对字面量池）；跑 FFXII 定位首个 gap：Shared WRAM 未映射 | [cpu](src/cpu/cpulog.md) |
| 2026-09-05 | 21-B0 | 预习/文档 | 新增 `docs/21-rom-bringup.md`：Phase A 总结 + Phase B 路线（真实 ROM 兼容性验收学习文档） | [21](docs/21-rom-bringup.md) |
| 2026-09-05 | 21-B1 | 内存总线 | Shared WRAM 映射：0x03000000 主区 + 0x037F8000 镜像区（32KB 同一物理内存）；ARM7 不再卡 0x037F8xxx | [bus](src/bus/buslog.md) |
| 2026-09-05 | 21-B2 | IO/IPC | IPCSYNC（0x04000180/81）同步寄存器：双核 out 交叉读写 + bit13 请求→对端 IF16 + bit14 门控；真 ROM 不再报未知 IO（544 项检查 0 失败） | [io](src/io/iolog.md) · [tests](tests/testlog.md) |
| 2026-09-05 | 21-B3 | 内存总线 | Main RAM 无缓存镜像 0x02400000-0x027FFFFF（4MB 同一物理别名）；真 ROM ARM9 不再弹 PC=0，稳定停在主存代码区（552 项检查 0 失败） | [bus](src/bus/buslog.md) · [tests](tests/testlog.md) |
| 2026-09-05 | 21-B4 | CPU | ARM BX 奇地址切换 Thumb（T 位按目标 LSB 置/清 + PC 清 LSB）；真 ROM ARM7 不再按 ARM 误译 Thumb 区（555 项检查 0 失败） | [cpu](src/cpu/cpulog.md) · [tests](tests/testlog.md) |
| 2026-09-05 | 21-B5 | CPU | Thumb BX/BLX 寄存器号解码修复（Rm 在 bit6:3，BX lr=0x4770 不再变成 BX r8）+ Thumb 逐条 trace；真 ROM IO 刷屏消失（557 项检查 0 失败） | [cpu](src/cpu/cpulog.md) · [tests](tests/testlog.md) |
| 2026-09-05 | 21-B6 | 内存总线 | Main RAM 无缓存镜像仅 ARM9 可见（ARM7 写访问落空）；修复 ARM7 块拷贝覆盖 ARM9 栈导致的弹 PC=0xE1C010B0（559 项检查 0 失败） | [bus](src/bus/buslog.md) · [cpu](src/cpu/cpulog.md) · [tests](tests/testlog.md) |
| 2026-09-05 | 21-B7 | IO 寄存器/内存总线 | EXMEMCNT/WRAMCNT 内存控制寄存器 + Shared WRAM 按 WRAMCNT 双核切分；真 ROM 3 条未知 IO 消除（587 项检查 0 失败），ARM7 轮询卡点排入 B8 | [io](src/io/iolog.md) · [bus](src/bus/buslog.md) · [tests](tests/testlog.md) |
| 2026-09-05 | 21-B8 | 内存总线/CPU/主循环 | 解码 0x027FFxxx 信箱约定；根因是 ARM9 DTCM/ITCM 未实现——新增 CP15 DTCM/固定 ITCM、镜像恢复双核别名、直接启动卡带表（594 项检查 0 失败），真 ROM 越过旧轮询进入 boot 初始化 | [bus](src/bus/buslog.md) · [cpu](src/cpu/cpulog.md) · [main](src/mainlog.md) · [tests](tests/testlog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-05 | 21-B9 | BIOS | SWI 0x0E GetCRC16（CRC-16/IBM）：新增 `bios_crc16.*`，ARM/Thumb/ARM7 单测（600 项检查 0 失败）；真 ROM unknown SWI 消失，ARM7 前进到 0x037FC89C | [bios](src/bios/bioslog.md) · [tests](tests/testlog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-05 | — | 工程管理 | 程序入口自动切控制台 UTF-8（`SetConsoleOutputCP(CP_UTF8)`），修复中文日志乱码 | [main](src/mainlog.md) · [tests](tests/testlog.md) |
| 2026-09-12 | 21-B9xi | IO/音频/诊断 | ARM7 BIOS 保护值 0x04000308=0x1204（进 power 模块）+ SOUNDBIAS 上电 0x200/10 位写语义；新增直启表/IPC/TIMER1 诊断，锁定“ARM9 在第 2064 帧关掉 TIMER1、ARM7 因此收不到 0000C187 请求”这一分歧链（894 项检查 0 失败） | [io](src/io/iolog.md) · [snd](src/snd/sndlog.md) · [tests](tests/testlog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9xj | 显示 IO/诊断设施 | **ARM7 独立 DISPSTAT**（melonDS `DispStat[1]`）：此前两核共用一个寄存器，ARM7 每帧按扫描线写 0x04000004 会把 VCount 比较值与中断使能"写进" ARM9 的 DISPSTAT，导致 ARM9 多出一次 VCount 匹配中断（IF9 bit2 常驻）；同时新增 `--watch LO-HI` 总线写监视（打印写入者 PC/LR/SP/栈顶）与 ARM9/ARM7 事件计数（903 项检查 0 失败） | [io](src/io/iolog.md) · [bus](src/bus/buslog.md) · [tests](tests/testlog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9xq | 内存总线（根因） | **GBA 扩展槽空槽读值**：slot-2 窗口（0x08000000-0x0BFFFFFF）无卡时应对齐参考核返回 0xFF。FFXII 用 DMA1 从这里取 0x40 字节填 `0x02079920-5F`，本地读 0 导致该表（含拷贝到 `0x027FFC30` 的签名）全 0，进而使 ARM7 调度器闸门 `0x0380BA7C` 不置位、心跳条数偏多、ARM9 显示重初始化循环快一倍。修复后 `0x027FFC30`/`0x03808240`/`0x0380BA7C` 与参考核逐项一致（903 项检查 0 失败） | [bus](src/bus/buslog.md) · [tests](tests/testlog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9xb（分析） | 诊断 | 进度行补 sp/lr + 服务队列条目对照：分歧定位到 ARM9 任务内容（仅文档） | [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9xd（分析） | 工程 | 两核吞吐差归因（帧延迟空转循环）+ ARM9 深睡例程调用链；当前任务指针差异 02076F24 vs 02076FE4（仅文档） | [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9xf（分析） | 工程 | ARM7 协作式调度器时钟与派发点（`0x03808240` 待办标志差异）（仅文档） | [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9xh（分析） | 工程 | ARM7 调度器任务队列为空（`0x03806E50` 返回 0）⇒ 待办位被清 0（仅文档） | [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9xk（工具/分析） | 诊断设施 | 读监视 `--watch-r`（IPC 收取探针）+ 心跳发送时间表：本地从帧 0 起、参考核帧 539 起 | [io](src/io/iolog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9xl（分析） | 工程 | 心跳报文编码解读（`0x03009000+n` 槽）+ 栈快照指向任务条目表同一循环 | [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9xm（分析） | 工程 | 帧 120 双核快照对照（共享 WRAM/队列/声音寄存器全同）+ ARM7 视角寄存器差异逐条核对 | [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9xn（分析） | 工程 | 排除 IPC 嫌疑；锁定 ARM7 调度器 tick 次数差异（参考核 6000 帧 2 次 vs 本地每 10 帧一次） | [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9xo（分析） | 卡带/诊断 | 根因锁定=卡带读取路径（`0x04100010` CARD_DATA）；ARM7 tick 闸门 `0x0380BA7C` 与 `0x027FFC30` 的完整链条 | [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9xr（分析） | 工程 | 心跳由 ARM7 消息队列循环驱动（`0x03804514` 出队 → 类型 1 → `0x03804A54` 槽处理）；四条心跳同源 `0x03804B5C` | [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9xt（验证） | 工程 | 6000 帧长程验证——GBA 槽修复后本地不再卡死（GX 2595→20435、ARM9 在 ITCM 执行）+ 截图与参考核抽样帧对照（仅文档） | [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9xu（工具） | 主循环 | 画面时间线 `--shot-every N --shot-prefix P`（无头模式每 N 帧存 BMP） | [main](src/mainlog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9xv（对照） | 工程 | 对照前提修正——参考核从帧 1700 注入按键；对齐后本地 1900 帧偶数列 100% 一致 | [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9xw（代码/测试） | 3D/总线 | DISP3DCNT bit12/13 写 1 清零 + 3D 图层不再受 bit13 门控；帧 1900 两边 VRAM 逐字节一致 ⇒ 差异锁定 DISPCNT mode2 渲染路径；907 项 0 失败 | [gx](src/gx/gxlog.md) · [bus](src/bus/buslog.md) · [tests](tests/testlog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9xx（实验） | 显示 | mode2 VRAM-display 分支试验（无变化，已回退并记录）+ 帧 1900 全量 IO 差异清单 | [ppu](src/ppu/ppulog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9xy（代码/测试） | IO | KEYINPUT 高位对齐参考核（0x03FF）+ `0x04000320` 硬编码 46：907 项 0 失败 | [io](src/io/iolog.md) · [tests](tests/testlog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9xz（结论） | 工程 | BG2/BG3 仿射差异实为 melonDS 读路径缺口（`NDS.cpp` 无 `0x04000020` 分支），本地值才是真值，从待修项划掉（仅文档） | [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9y0（分析） | 显示 | 条纹来源线索（mode2 分支 + `tri=0` 的 3D 空白）与下一步实验设计（dump 参考核 3D 层输出） | [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9y1（实验） | 显示 | NO3D 实验证明条纹非 3D 层；16 位寄存器对照引擎 A 全同；剩余未比项=调色板 RAM | [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9y2（验证） | 显示 | 调色板逐字节一致（0/1024）+ 复核 mode2 实验 ⇒ 差异在本地 2D 渲染器取数逻辑 | [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9y3（诊断） | 显示 | 渲染器 BG 取址诊断 `NDS_BGDBG=1`（帧 1900 仅 BG2 参与合成）+ 截图与 dump 采样时刻不一致的方法论更正 | [ppu](src/ppu/ppulog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9y5（诊断） | 显示 | BG 诊断扩展（tile0 原始字节 + 调色板前 4 项）：帧 1900 BG2 数据非零、主调色板 pal0=001F/pal2=0421 | [ppu](src/ppu/ppulog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9y6（结论） | 显示 | 查表证明本地 BG2 数据解析为青蓝色（非黑）⇒ 问题在合成之后；DISPCNT=80111418 复核 | [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9y7（诊断） | 显示 | 合成器里是青色、`comp_finalize` 后变黑；根因=MASTER_BRIGHT 且参考核 dump 是加亮度前画面（新增 `NDS_NOMB`） | [ppu](src/ppu/ppulog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9y8（重要更正） | 工程 | 参考核 framebuffer 是 **32 位/像素**（此前按 16 位读得到“黑+青色条纹”假象）；修正读法后的亮度对照（仅文档） | [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9y9（对照） | 工程 | 对齐输入 + `NDS_NOMB=1` 逐帧对照：发现两种差异成分（RGB555→888 换算 + 帧 100 已有真实内容分歧） | [21](docs/21-rom-bringup.md) |
| 2026-09-12 | 21-B9yb（验证） | 显示 | 色彩换算修正已验证有效（帧 100 相同像素 0%→81.9%），因批量替换误伤 13 条非颜色断言而本轮先回退保持 907 全绿 | [ppu](src/ppu/ppulog.md) · [tests](tests/testlog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-05 | 21-B9a（代码/测试） | BIOS | SWI 0x0E GetCRC16（CRC-16/IBM）落地：新增 `bios_crc16.*` + ARM/Thumb/ARM7 单测，ARM7 越过 SWI 0x0E | [bios](src/bios/bioslog.md) · [tests](tests/testlog.md) · [21](docs/21-rom-bringup.md) |
| 2026-09-06 | 21-B9m（代码/测试） | CPU | ARM9 CP15 WFI（`MCR p15,0,r0,c7,c0,4`）：空闲任务等待 (IE&IF) 唤醒而不是满速空转 | [cpu](src/cpu/cpulog.md) · [tests](tests/testlog.md) · [21](docs/21-rom-bringup.md) |
