# gx 模块日志

> 覆盖：NDS 3D 几何引擎（Geometry Engine）。`src/gx/gx.h/.c` 纯核心实现（不依赖 SDL）。
> 吃「命令流」（矩阵/顶点/多边形属性），做矩阵变换 + 顶点变换（pos×proj → 透视除 → 视口映射），
> 输出屏幕空间三角形，由软件光栅化写入 3D 帧缓冲（256×192 RGB555），经 `DISP3DCNT` 使能位合成进 2D 顶屏。
> 寄存器：DISP3DCNT(0x04000060)、GXFIFO(0x04000400)、命令端口(0x04000440+cmd*4)、GXSTAT(0x04000600)。
> 注意：`0x04000400..0x040005FF` 与 ARM7 音频寄存器重叠，按访问者 CPU 分流（ARM9→几何，ARM7→音频），
> 由 `bus_write32`（ARM9 整字转发）+ `io_read8/io_write8`（DISP3DCNT/GXSTAT 按字节、gx 侧）路由。

## 19.1 — 短文 + 头文件

- **做了什么**：新增 `docs/20-3d.md`：几何引擎（GE）/渲染引擎（RE）分工、GXFIFO 40 位命令条目
  （8 位命令 + 32 位参数）、三种命令提交方式（unpacked / packed / 命令端口）、矩阵（1.19.12）与
  顶点（1.3.12）定点格式、图元类型、最小软件管线实现指南。
  新增 `gx.h`：寄存器/命令码/图元/定点常量、`gx_vertex_t`（屏幕坐标 + RGB555）、
  `gx_t`（DISP3DCNT/GXSTAT、pos/proj/tex 矩阵 + 栈、当前顶点属性、视口、顶点列表、3D 帧缓冲），
  声明 `gx_is_addr/gx_read8/gx_write8/gx_write32/gx_reset/gx_framebuffer/gx_transform_vertex/gx_raster_tri`。

## 19.2 — 寄存器 + 几何命令 FIFO + io/bus 接线

- **做了什么**：
  - `gx.c` 实现字节级 `gx_is_addr/gx_read8/gx_write8`（DISP3DCNT 4 字节 + GXSTAT/RAM_COUNT，
    GXSTAT 复位置 FIFO 空位 bit26，RAM_COUNT 简化读 0）。
  - 命令 FIFO 解码状态机：`gx_enqueue`（按命令码查参数表入队，0 参数命令立即执行）、
    `gx_feed_param`（喂参数给队头，满则执行）、`gx_flush`（弹队头执行）。
    `gx_write32`：`0x04000400..0x040043F` 为 GXFIFO 命令字——「无待填参数」时按低 4 字节拆成
    ≤4 条命令（0=NOP），「有待填参数」时该字是参数；`0x04000440..0x040005FF` 为命令端口——
    地址低字节编码命令码，写入值即该命令的唯一参数。
  - `io.h/io.c`：`io_t` 增 `gx_t gx`；`io_create` 里 `gx_reset`；`io_read8/io_write8` 增
    `gx_is_addr && !is_arm7` 路由（DISP3DCNT/GXSTAT），`snd_is_addr` 收紧为 `&& is_arm7`；
    新增 `io_gx_write32`。
  - `bus.c`：`bus_write32` 增几何区（`0x04000400..0x040005FF`）ARM9 整字转发到 `io_gx_write32`
    （拆字节会破坏 40 位命令语义）。
- **怎么验证**：`test_gx_fifo` 经 `bus_write32` 发完整命令流（矩阵→颜色→视口→BEGIN/VTX/END）出图；
  `test_snd_regs/mix/program` 切到 `active_is_arm7=1`（音频由 ARM7 控制）后仍全过。

## 19.3 — 几何命令：矩阵 / 顶点变换

- **做了什么**：`gx_exec` 按命令码分发：
  - **矩阵**：`MTX_MODE`(投影/位置/纹理选当前矩阵)、`MTX_IDENTITY`、`MTX_LOAD_4x4/4x3`、
    `MTX_MULT_4x4/4x3/3x3`、`MTX_SCALE`、`MTX_TRANS`、`MTX_PUSH/POP/STORE/RESTORE`（栈 32 层）。
    矩阵行主序存 `int64_t[16]`（1.19.12），装载按列主序，乘法 `r[i][j]=Σa[i][k]b[k][j] >>12`。
  - **顶点**：`COLOR/NORMAL/TEXCOORD` 记当前属性；`VTX_16/VTX_10/VTX_XY/XZ/YZ/VTX_DIFF` 提交顶点；
    `BEGIN_VTXS`（图元类型低 2 位）/`END_VTXS`（弹图元光栅化）；`VIEWPORT`（x1/y1/x2/y2 各 8 位）；
    `SWAP_BUFFERS` 简化 no-op（即时光栅化）。
  - `gx_transform_vertex`：clip=pos×proj → 齐次乘（w=0x1000）→ 右移 12 得 NDC →
    透视除（nw≠0，否则当正交）→ 视口映射（NDC(-1..1)→(vx1..vx2, vy1..vy2)，y 翻转 +1=顶）。
- **怎么验证**：`test_gx_transform`（单位矩阵 + 全屏视口下 (-1,-1)/(1,-1)/(-1,1)/(0,0) → (0,191)/(255,191)/(0,0)/(127,95)）。

## 19.4 — 软件光栅化 + 3D 图层合成

- **做了什么**：
  - `gx_raster_tri`：求包围盒夹到屏幕，逐像素用三条边函数同号（含 0）判定在三角形内，平色写入帧缓冲。
  - `render.c` 新增 `render_3d`：`DISP3DCNT` 使能位（bit13）置位时，把 3D 帧缓冲覆盖到主引擎（顶屏）；
    0 像素视作「无几何」背景保留 2D 输出，非 0 像素用 RGB555 颜色覆盖（最小实现：不分优先级/无 alpha 混合）。
    `render_frame` 末尾调用（仅顶屏，副引擎无 3D）。
- **怎么验证**：`test_gx_raster`（三角 (10,10)-(100,10)-(10,100) 内 (50,50)=红、外=0）；
  `test_gx_layer`（3D 未使能时像素露 2D 黑背景、使能后三角形红覆盖到顶屏）。

## 19.5 — 综合测试

- **做了什么**：`tests/test_nds.c` 增 `test_gx_transform`（8 项）、`test_gx_raster`（3 项）、
  `test_gx_fifo`（3 项）、`test_gx_layer`（2 项）；`test_snd_regs/mix/program` 改 ARM7 视角。
  累计 483 项检查 0 失败，`ctest` 100% 通过。
- **简化说明**：命令同步执行（不模拟 4 拍 FIFO 延迟）；平色着色（无光照/纹理采样/alpha 混合）；
  `SWAP_BUFFERS`/`RAM_COUNT`/`POLYGON_ATTR`/`NORMAL` 为占位或忽略。

## 2026-09-06 · 21-B9za — GXSTAT FIFO IRQ 模式（IF bit21）
- GXSTAT 0x04000603 的 bit30-31 是 FIFO IRQ 模式（melonDS `Write8` 口径），
  FFXII 设 mode=2 后 ARM9 IF bit21 应随 FIFO 空挂起；旧实现把 GXSTAT 当只读，
  本地 IF9 一直缺参考里的 0x200000。
- `gx_write8` 支持 0x04000603 写模式、0x04000601 bit7 清栈标志；
  io 层在 GX 写后同步 IF21（简化：FIFO 同步执行，空即触发）。

## 2026-09-12 · 21-B9wu — 运行计数（命令/三角形/FIFO/端口）

- `gx_t` 增加 `cmd_count / tri_count / fifo_writes / port_writes`：runner 摘要
  打印 `gx3d: fb-nz=... cmd=... tri=... fifo-wr=... port-wr=...`。
  排查「3D 段黑屏」时可直接看出游戏到底有没有提交几何命令、FIFO 通路是否为空。

## 2026-09-12 · 21-B9wy — GXSTAT FIFO 状态位对齐参考核（bit25/26）

- 参考核 `GPU3D::Read32(0x04000600)` 返回
  `GXStat | (PosStackPtr<<8) | (ProjStackPtr<<13) | (fifolevel<<16) |
   (fifolevel<128 ? 1<<25 : 0) | (fifolevel==0 ? 1<<26 : 0)`
  ——FIFO 空时 **bit25 与 bit26 同时置位**。
- 本地旧实现只置 bit26（`0x84000000`），与参考的 `0x86000000` 不一致；游戏若用
  bit25 判断“FIFO 还有空间”就会一直等不到。现在 `gx_reset` 同时置位
  `GXSTAT_FIFO_LESS_HALF | GXSTAT_FIFO_EMPTY`（同步执行模型下 FIFO 始终不满）。
- 单测 `[case 21-B9wy]` 4 项：两个状态位存在、写 IRQ 模式后仍保留。
### 21-B9yi（续16）— GXSTAT bit27（3D 忙）建模

**证据**：游戏在 f=2110-2120 的代码里真的在等 3D 引擎完成：

```
020046B4  ldr  r2, [pc, #0x60]      ; r2 = 0x04000600（GXSTAT）
020046B8  ldr  r0, [r2]
020046BC  ands r0, r0, #0x8000000   ; bit27 = 3D engine busy
020046C0  bne  #0x20046b8           ; 忙则原地等
```

本地此前 `gxstat` 只维护 bit25/bit26（FIFO 不足半满/空），**bit27 恒为 0**，
这类等待被整段跳过（游戏跑得比参考核快）。

**实现**：

- `gx_t.busy_cycles`（新增）：每条 GX 命令按 melonDS `GPU3D::AddCycles()` 的口径
  累加工作周期——交换缓冲 254、4x4/4x3 矩阵 35/16、矩阵乘 17、顶点 18、
  起止图元 3、属性 5、其余 4；置位 `GXSTAT bit27`。
- `gx_advance(gx, cycles)`（新增）：按系统时钟消耗余额，归零时清 bit27；
  在 `io_advance_cart()`（ARM9 时钟片）里调用——对应 melonDS 用
  `ARM9Timestamp` 推进 `GPU3D::CycleCount`、到 0 才 `FinishWork` 清忙位。
- `NDS_NOGXBUSY=1` 可关闭该模型（A/B 对照）。

**实测**：931 项单测 0 失败；900 帧三标记与参考一致；帧 100 仍 81.9%；
f=2090-2110 与参考核仍逐帧 100% 逐像素相同；f=500 开关对照 MAD 均为 135.27（无差异）；
**f=2120 的首个分歧仍在且逐值不变** ⇒ 该分歧与 3D 忙等待无关（见
`docs/21-rom-bringup.md` 续16 的下一步：VRAM bank 内容差异 / 双缓冲 + 交换清屏）。

### 21-B9yi（续22）— GX 命令 FIFO 加节拍（模式 7 DMA 分批 / 续跑）

**动机**：`NDS_DMAIRQ` 显示本地 IF bit11（DMA3 完成）多出 ~0.7 次/帧，全部来自
**模式 7（GX FIFO）DMA**；melonDS 的 GX-FIFO DMA 受 `CmdFIFO`（112 项）容量与
3D 引擎消耗速度限制，`Run9` 会 `Stall`、分批推进，完成时机完全不同。

**实现**：

- `gx_t.fifo_words`：FIFO 内已入队未消费字数（写命令端口 +2 字/次），
  上限 `GX_FIFO_CAP_WORDS = 224`（=112 项）；`gx_fifo_free_words()` 给出空位；
- `gx_advance()`：按系统时钟消费（≈4 周期/字），与 `busy_cycles`（GXSTAT bit27）同一时钟；
- `dma_transfer()`（模式 7）：只在有空位时推进，搬不完保持 enable、剩余记入
  `dma_channel_t.rem`、**不挂完成中断**；`dma_gx_resume()` 在 FIFO 腾空后续跑
  （对齐 melonDS `GPU3D::CheckFIFODMA()`），由 `io_advance_cart()` 每片调用。

**效果**：IF bit11 **58 次/81 帧 → 0 次**（参考核 0 次）；**逐帧逐像素一致窗口
从 f=1900-2110 扩到 f=1900-2140**；f=2150 起本地只落后 10 帧
（本地 f=2150 == 参考核 f=2140 逐像素）；931 项单测 0 失败。

### 21-B9yi（续23）— FIFO 消费余数丢失（真 bug）+ 速率标定（`NDS_GXDRAIN`）

**Bug**：`gx_advance()` 用 `drain = cycles / div` 取整。ARM9 每步只推进 1-2 个
系统周期 ⇒ `cycles/4` 恒为 0 ⇒ **FIFO 永不消费** ⇒ 模式 7 DMA 永远卡住。
实测：f=2141 起 ARM9 在 `0x0202350C/10/14` 三指令小循环里死等，
`dma3=0237083C/04000400/FC400134`（模式 7 + 重复 + 使能）不再前进。

**修正**：新增 `gx_t.fifo_drain_acc`，余数跨调用累加（FIFO 空时清零，避免攒突发）。

**速率标定**（`NDS_GXDRAIN=N` 周期/字，带按键时序与参考核逐帧比）：

| N | f=2120 | f=2140 | f≥2160 |
|---|---|---|---|
| 4/16 | 分歧（本地超前） | 分歧 | 分歧 |
| 64 | 一致 | 分歧 | 分歧 |
| **128（默认）** | **一致** | **一致** | 分歧（MAD 518） |
| 256 | 一致 | 一致 | 内容不变但整体拖慢 3 倍 |

⇒ 默认取 128：**逐帧 100% 一致窗口 = f=1900-2140**。
备注：真实节拍来自 melonDS 的每命令工作周期（`GPU3D::AddCycles`），
本处的「N 周期/字」是标定近似；下一步应改成按 `gx_exec()` 的每命令成本推进。

### 21-B9yi（续24）— 命令按成本执行（去掉标定常数）

**改动**：命令不再「写入即执行」，而是入队 → 由 `gx_advance()` → `gx_run_due()` 驱动：

```
busy_cycles > 0  → 先扣工作周期（ARM9 系统时钟）
busy_cycles == 0 → 执行队首「参数已收齐」的命令，gx_exec 再按类型记 3-254 周期
队列空且成本走完 → 清 GXSTAT bit27；否则保持忙
```

⇒ FIFO 消费速度由**命令成本**决定，续23 的 `NDS_GXDRAIN` 标定常数不再需要
（环境变量保留但默认路径不用）。

**同轮修 bug**：队列保留「已完成待执行」的命令后，`gx_feed_param()` 若继续喂**队头**
会丢弃所有参数、使队列卡死（实测 GX FIFO 用例 `pending` 恒 16、`qlen` 恒 2）。
现在喂**队尾**未收齐的那条（GX 是严格顺序流，只有队尾可能缺参数）。

**实测**：f≤2140 逐帧 100% 一致（与标定最优同窗口）；f=2150 起 MAD 32.5→216.6
（本地画面落后）；931 项单测 0 失败；900 帧三标记一致；帧 100 81.9%。

### 21-B9yi（续25）— 队列诊断设施 + f=2141 停摆定位（GX 队列卡在 `0x34(1/32)`）

**新增**：`gx_state()`（FIFO 字数 / 工作周期余额 / 队列长度 / 待收参数数）接到
runner 的 `NDS_FTRACE`（每帧多一行 `gxfifo=… busy=… q=… pend=… stat=…`）；
`NDS_GXDBG=1` 时在「队头缺参数」处打印队列快照 `gxstuck: …` / `gxqueue: …`。

**现场**（f=2141-2152）：

```
gxfifo=500 busy=0 q=43 pend=992 stat=8E000000
gxqueue: 34(1/32) 34(1/32) …（43 条全 34、各只收到 1 个参数）
```

ARM9 卡在 GXSTAT bit27 忙等（`0x0200468C-94`、`0x020046B8-C0`），引擎永远等不到
`0x34` 的另外 31 个参数。

**两个实验（均已回退）**：

1. 按 melonDS `CmdNumParams[]` 把 `0x34` 当 32 参数（并补 `0x2A/2B`、`0x30-0x33`、
   `0x70-0x72`）→ **渲染全黑**（f≥2000 MAD 765）：游戏实际按「`0x34` + 1 参数」发流。
2. bit27 只在 `SWAP_BUFFERS` 置位（melonDS 口径）→ 停摆消失但本地冲在参考核前面
   （f=2120 起 MAD≈589）⇒ 还缺 **3D 管线成本**（顶点/多边形摊到引擎周期）。
   另试 `NDS_GXPOLY=500/2000`（每三角形固定成本）对该窗口**无影响** ⇒ 瓶颈不在
   光栅化计费，而在队列解析/提交路径。

**当前保留**：仅诊断设施；行为回到 f≤2140 逐帧 100% 一致、931 项单测 0 失败。

### 21-B9yi（续27）— bit27 只在交换缓冲后置位 + FIFO 按条目节流（GX 停摆解除）

**改动**：

1. `gx_t.swap_busy`：只有 `SWAP_BUFFERS` 之后的管线期间才置 `GXSTAT bit27`
   （melonDS 全文件只有那一处置位、`FinishWork()` 排空时清位）；
   此前「每次入队都置忙」会在游戏「发命令 + 第 1 个参数 → 等 bit27 → 再发剩余参数」
   的写法上锁死（实测 `q=113 pend=744`）。
2. `gx_fifo_can_accept()`：FIFO 满按 melonDS `CmdPIPE` 的 **112 条条目**口径——
   队列未满可收；满但仍有命令缺参数也可收；只有「满且无待填参数」才挡。
3. 模式 7 DMA 每次只推一个字，由 `dma_gx_resume()` 续推（对齐 `CheckFIFODMA()`）；
   `test_dma_gx_fifo_mode` 改成「泵引擎 + 续跑 DMA」。

**结果**：修复前 f=2135-2152 ARM9 卡在 `0x020046B8-C0` 的 bit27 忙等；修复后
f=2135-2140 在 ITCM 游戏代码里正常推进（与参考核同段 ctrace 形态一致）。
931 项单测 0 失败、900 帧三标记一致。

### 21-B9yi（续32/33）— GX 命令流模型纠正：3D 管线从「一个三角形都画不出」到与参考核同型

**现象**：`--headless-frames 7300` 的 `gx3d` 摘要恒为 `tri=0 fb-nz=0`，
但新增的 `NDS_GXHIST=1` 命令直方图显示游戏明明提交了 8142 次 `BEGIN_VTXS`、
4.1 万个顶点命令 —— 也就是「命令都收到了，几何一个都没执行」。

**根因（两层）**：

1. **参数个数表不完整**（`gx_cmd_nparams` 的 default 返回 0）：
   `0x2A TEXIMAGE_PARAM` / `0x2B PLTT_BASE` / `0x30-0x33` /
   **`0x34 SHININESS`（实际要 32 个参数）** / `0x70-0x72` 全部缺失。
   漏掉 `0x34` 以后，紧跟它的 31 个参数字被当成命令字解析，命令流从此错位。
   已按 melonDS `CmdNumParams[256]` 逐项补齐。
2. **队列模型错**：本地把「每一次端口写」当成一条完整命令（`gx_enqueue` +
   立刻喂 1 个参数），并且把后续参数字喂给**队尾**。melonDS 的 `CmdFIFO` 是
   **条目流**：每次写入产生一个 `{Command, Param}` 条目；一条需要 n 个参数的命令
   吃掉紧随其后的 n-1 个条目（**不管那些条目来自哪个写入端口**，其命令字节被忽略）。
   实测证据：游戏在 f≈300 连续写 64 次端口 `0x040004D0`（值都是 0）——
   这是 **2 条各带 32 个参数的 SHININESS**；本地把它拆成 64 条缺参数的 SHININESS，
   队头永久停在 `0x34(1/32)`，`gx_run_due()` 在队头直接 break，队列之后再没推进过。

**改动**：

* 按 melonDS 重写队列：`gx_entry_t` 条目环 + 参数收集状态机（命令 + 参数条目）+
  「参数已收齐」的按成本执行队列；
* `gx_write32()` 的 GXFIFO 分支**逐句直译** `GPU3D::WriteToGXFIFO()`
  （32 位字里最多 4 个命令字节按「一次写派发一个条目」展开、0 字节 NOP 跳过）；
* 端口写保持 `CmdFIFOEntry{Command=(addr&0x1FC)>>2, Param=val}` 语义；
* `gx_fifo_free_words()/gx_fifo_can_accept()` 改为「执行队列 + 未解码条目 +
  收集中命令」的口径；NOP（`0x00`）不再计入 `cmd_count`/直方图。

**怎么验证**：

* 1500 帧：`0x34` 从 64 降到 2（= 2 条真命令），命令总数 2595→2533；
  3000 帧：`tri=13449`（此前恒 0）、`0x40/0x41 = 5097/5099`（成对出现）。
* 7600 帧与参考核（melonDS 906e9eb + 本次新加的 `REF_GXHIST` 统计）同输入脚本对照，
  命令分布同型：`0x20` 12.94M vs 12.52M、`0x22` 18.0M vs 17.4M、
  `0x40/0x41` 3.516M/3.5162M vs 3.4015M/3.4015M、`0x50` 2501 vs 2491、
  `0x30-0x33` 各 ≈34.7k vs ≈33.6k（整体 +3.3% 均匀）。
  （口径提醒：melonDS 的直方图按**条目**计数，参数条目会带上当前命令字节，
  所以 `0x16/0x17/0x34` 这类多参数命令在参考核里天然是本地「按真命令计数」的 ~n 倍。）
* `tests/test_nds.c` 931 项 0 失败。

**结果**：✅ 3D 管线复活 —— 7600 帧里 913 万个三角形真的进了光栅化（此前 0），
`gx3d fb-nz` 从 0 变非 0；f=3000 的顶屏从「几乎全黑」变成完整场景。

**已知遗留**：本地是**平色**光栅化（不采样纹理、无光照/雾/裁剪），而游戏大量使用纹理
（`TEXCOORD` 1740 万条、`TEXIMAGE_PARAM` 4.9 万条）。只贴 3D 的场景（如 f≥7000）
本地仍偏黑：几何覆盖了屏幕，但颜色取的是 `COLOR` 命令值（多为 0），参考核取的是纹理颜色。
**下一步**：3D 纹理采样 + 3D 图层 alpha/颜色口径（melonDS：18 位色，alpha=0 才透明）。

### 21-B9yi（续34）— 3D 渲染链路按 melonDS 全面对齐：矩阵 / 视口 / 深度 / 纹理

**背景**：续33 把 GX 命令流打通后，3D 管线虽然「在跑」（7600 帧 913 万个三角形），
但画面基本是黑的。本轮用新加的 `gxpath` 计数（纹理/平色/落地三角形数、写出像素数、
`w<=0` 顶点数）逐项定位，发现渲染链路有四处与真机语义不符，全部按 melonDS 逐句对齐。

**1. 矩阵语义全错（最大的一处）**：本地把命令参数按「列主序」转置装载、
相乘方向也是反的。melonDS 的实际语义是：

| 命令 | melonDS | 本地原来 |
|------|---------|---------|
| `0x16/0x17` LOAD | 参数**按原顺序**直接填入 `m[0..n-1]`（4x3 补第 4 列 `[0,0,0,1]`） | 按列主序转置 |
| `0x18/0x19/0x1A` MULT | `m = s * m` | `m = m * s` |
| `0x1B` SCALE | 每一**行**乘对应分量 | 单位阵乘 `m` |
| `0x1C` TRANS | `m[12..15] += (平移量经当前 m 变换)` | 单位阵乘 `m` |

  **实测（f=4400，同一输入脚本）**：`w<=0` 顶点 4,957,873 → **160,186**；
  真正写出像素的三角形 1,012 → **22,703**；写出的像素总数 11,049 → **725,756,189**。

**2. 视口 Y 轴翻转**：DS 的 `VIEWPORT` 参数里 `y0/y1` 是「屏幕坐标、0=底」的
  （GBATEK：y0<y1、y 轴倒置），melonDS 用 `191-y1` 作上边缘、高 `y1-y0+1`。
  本地直接用 `y0` 当上边缘、高 `y2-y1` ⇒ **整幅 3D 画面被垂直镜像**且少一行。
  修正后屏幕中心由 (127,95) 变为 (128,96)（单测期望值同步更正）。

**3. 纹理采样**（此前完全没实现，3D 只能画平色，而游戏 1740 万条 `TEXCOORD`
  都在用纹理）：
  * `0x2A TEXIMAGE_PARAM` / `0x2B PLTT_BASE` / `0x22 TEXCOORD` 落地；
  * 7 种格式全部实现（4 色 / 16 色 / 256 色 / A3I5 / A5I3 / 直色 16 位 / 4x4 压缩），
    采样、环绕（bit16-19）、`alpha0`（bit29）逐条对齐 `SoftRenderer3D::TextureLookup`；
  * **纹理坐标变换**（`TEXIMAGE_PARAM` bit30，游戏实测=1）：
    `TexCoords = RawTexCoords × TexMatrix`；
  * 透视校正插值（`u/w`、`v/w` 线性插值后除以插值出的 `1/w`）；
  * VRAM 侧新增**纹理槽**（128KB/槽）与**纹理调色板槽**（16KB/槽）读取：
    VRAMCNT 的 E/F/G `mode 3` 映射补上（实测游戏写 `VRAMCNT_F=0x83` ⇒ 调色板槽 0；
    `VRAMCNT_A=0x83`/`VRAMCNT_B=0x8B` ⇒ 纹理槽 0/1）。

**4. 深度缓冲**：此前完全没有深度测试，远处多边形会盖住近处。
  按 melonDS `FinalZ` 公式实现 24 位 Z（小 = 近）：
  `z = (Zc/Wc * 0x4000 + 0x3FFF) * 0x200`，钳 0..0xFFFFFF；
  多边形 `POLYGON_ATTR` bit14（相等测试）走容差比较，否则「新片元更近才通过」；
  交换缓冲时把深度置最远（真机靠游戏画清屏多边形，本地无裁剪/清屏多边形语义，
  故按「每帧重置」近似，见 gx.h 注释）。

**5. 3D 图层 alpha**：melonDS 的 `Output3D` 把 alpha 放在像素高字节，
  `DrawBG_3D()` 以「alpha==0 才透明」判可见。本地此前按「颜色==0 视为无几何」，
  于是所有涂成黑色（0x0000）的**不透明**多边形都被当成没有几何。
  新增并行 alpha 平面 `fba[]`（`fb[]` 仍为 RGB555，旧出图/用例口径不变），
  纹理 alpha × 多边形 `POLYGON_ATTR` alpha（bits16-20）合成。

**新增诊断**：`gxpath`（tex/flat/drawn/px/vtx0w + 当前 texparam/pltt/polyattr）、
`NDS_NO3D=1`（不合成 3D 图层，用于判定「某段画面是不是 3D 撑的」）。

**怎么验证**：`build\test_nds.exe`（931 项 0 失败，视口中心两项期望值按新口径更正）；
f=4400 的 `gxpath` 三个计数（见上）；f≤3200 逐帧画面统计与改动前**完全一致**
（199,139,104 / 169,166,169 / 71,108,139 / 0 / 203,194,194 …）⇒ 早期里程碑无回归；
`NDS_NO3D=1` 时 f=4000 顶屏回到全黑 ⇒ 现行画面确实由 3D 图层提供。

**结果**：✅ 3D 图层从此前「0 像素」变成**铺满整屏**（`gx3d fb-nz` 1157 → 49151），
渲染链路（矩阵/视口/裁剪口径/纹理/深度/alpha）与 melonDS 逐项对齐。

**已知遗留（下一步）**：画面已由 3D 撑起，但几何仍不对——现在是一大片色带/楔形，
而不是参考核那种带贴图的场景。仅 411-826 个三角形通过深度测试却铺满整屏，
说明**多边形级图元处理仍有问题**（近平面裁剪、条带/扇形顶点批次、以及
melonDS 的 `VertexPipeline`/`StallPolygonPipeline` 节拍都可能相关）。
下一步：把两边「同一帧的多边形列表」（顶点屏幕坐标 / w / z / uv / attr）dump 出来逐条对照。
