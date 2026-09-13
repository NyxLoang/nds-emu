# 参考核（melonDS）对照工具

本项目用一个**独立构建的 melonDS 参考核**做行为对照的「oracle」：
本地模拟器的某个阶段到底是「跟真机一致」还是「自己跑偏了」，用它来判定。

## 为什么放在仓库里

21-B9yi(续27) 曾经把参考核源码 + 我们打的诊断补丁 + 参考帧缓存全部放在
`%TEMP%\melonds-ref`，结果被 Windows 的临时文件清理清掉，逐像素对照一度不可用。
因此**补丁版 harness 必须进版本库**（本目录），重建时从 melonDS 官方仓库重取源码，
再把 `ref_harness.cpp` 覆盖进 `src/`。

## 重建步骤

```powershell
# 1. 取源码（本仓库记录用的 commit：906e9eb）
git clone https://github.com/melonDS-emu/melonDS "$env:TEMP\melonds-ref"

# 2. 生成构建（关掉 Qt/OpenGL/JIT，留 GDB stub，release 级别即可）
cmake -G Ninja -S "$env:TEMP\melonds-ref" -B "$env:TEMP\melonds-ref\build-core" `
      -DBUILD_QT_SDL=OFF -DENABLE_GDBSTUB=ON -DENABLE_OGLRENDERER=OFF `
      -DENABLE_JIT=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo

# 3. 把本目录的 harness 放进 src/，并在 src/CMakeLists.txt 里加一个 refhead 目标
#    （add_executable(refhead ref_harness.cpp) + 链接 core/teakra）
Copy-Item tools\ref\ref_harness.cpp "$env:TEMP\melonds-ref\src\ref_harness.cpp" -Force

# 4. 构建
ninja -C "$env:TEMP\melonds-ref\build-core" refhead
```

> 本机工具链：`%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin`
> （cmake / ninja / gcc / g++ 都在里面）。

## harness 支持的环境变量

| 变量 | 作用 |
|------|------|
| `REF_FRAMES=N` | 跑多少帧（默认 6000） |
| `REF_STATS_EVERY=N` | 每 N 帧打印双屏统计（非黑像素数 + 均值 RGB），与本地 `--stats-every` 同口径 |
| `REF_KEY_FRAME/REF_KEY_MASK/REF_KEY_PERIOD` | 按键注入脚本，与本地 `--key-frame/--key-mask/--key-period` 一致（`(f-KEY_FRAME)%PERIOD<12` 为按下） |
| `REF_SHOT_EVERY=N` | 每 N 帧把两块 256×192 帧缓冲写成 `%TEMP%\ref_fb_<frame>.bin` |
| `REF_STOP_IDLE=1` | 恢复旧行为：命中游戏空闲任务（0x02009570-0x02009590）就提前退出 |
| `REF_GXHIST=1` | 进程结束前打印 GX 命令码直方图（`refgxhist:` 行），与本地 `NDS_GXHIST` 对照 |
| `REF_POLYDBG=1` | 帧号 ≥3800 后打印前 6 个顶点的**裁剪空间坐标**与矩阵（对应本地 `NDS_POLYDBG=1`） |
| `REF_GXDBG_FRAME=N` / `REF_MAT_FRAME=N` | 只 dump 第 N 帧窗口的**矩阵类命令序列** / 打印该帧的 `proj/pos/tex` 关键元素（对应本地 `NDS_GXDBG_FRAME` / `NDS_MAT_FRAME`） |
| `REF_PROJDBG_FRAME=N` | 在 [N,N+20] 帧内，投影矩阵一变就打印「上一条命令 + 新矩阵」（对应本地 `NDS_PROJDBG`） |
| `REF_IODUMP_FRAME=N` | 打印第 N 帧的 2D 显示寄存器 + VRAMCNT + 颜色特效寄存器（对应本地 `NDS_IODUMP_FRAME`） |
| `REF_WAV=路径.wav` | **把参考核 SPU 输出录成 WAV**（32768Hz/16bit/立体声），与本地 `--snd-wav` 同口径对照「声音」（21-B9yi 续86） |
| `REF_RAMDUMP_FRAME=N` | 额外在第 N 帧 dump 主内存（`%TEMP%\ref_f<N>_mainram.bin`）。固定帧号只有 10 个，这个用来**二分定位分叉帧**（21-B9yi 续91） |
| `REF_WATCH_LO=0206C200` / `REF_WATCH_HI=0206C210` / `REF_WATCH_MAX=N` | **可配置写监视**（16 进制、半开区间）：ARM9/ARM7 的 8/16/32 位写都打一行 `refwatch arm9 w32 a=… v=… pc=… lr=… f=…`，格式对齐本地 `--watch`，于是「同一个地址谁来写、写什么」可以逐行对照（21-B9yi 续108） |
| `REF_FIFO_LOG=1` / `REF_FIFO_MAX=N` | **IPC 发送流日志**：每次写 0x04000188 打一行 `fifolog arm9 a=… v=… pc=… lr=… f=…`（默认上限 20000），与本地 `--watch 04000188-0400018C` 对齐，用来逐条对照两核消息流（21-B9yi 续108） |
| `REF_PCHIT_LO=0200EE4C` / `REF_PCHIT_HI=0200EF00` / `REF_PCHIT_MAX=N` | 挂 `ARM9Read16/32` 的**地址命中**日志（`refpc arm9 …`）。**已知限制**：本版 melonDS 的**指令取指走 ARM.h 的内联 `CodeRead16/32→BusRead*`**，不进虚拟 `NDS::ARM9Read*` ⇒ 抓不到取指（实测对确定会执行的地址也是 0 命中），只对**数据读**有效；要追「执行了哪段代码」得给参考树打补丁（21-B9yi 续108 记录了这个负结果） |
| `REF_INSTRSTAT=N` | 每 N 帧打一行 `refinstr: f=… i9=… i7=… b9=… b7=…`（两核**累计指令条数** + 其中**执行在 BIOS 区**的条数；与本模拟器的 `NDS_INSTRSTAT=N` 同口径）。需要先给参考树打 `melonds-armstat.patch`（见下） |
| `REF_PCHOT=1` | 退出前打印两核的 **PC 热点前 16 名**（`refhot9:` / `refhot7:`，`pc=… cnt=… (%)` + `total=`），与本模拟器 `NDS_PCHOT=1` 的 `hot9:`/`hot7:` 同口径。同样需要 `melonds-armstat.patch`（21-B9yi 续108p） |
| `REF_CARTSTAT=N` | 每 N 帧打一行 `refcart: f=… reads=…`（**0x04100010 ROM 数据口累计读次数**），与本地 `NDS_CARTSTAT=N` 的 `cartro:` 同口径；用来比较两边「从 ROM 取数」的时间线（21-B9yi 续108i） |
| `REF_CARTLOG=1` | **卡带传输级日志**：`refcartlog: f=… START cmd=… len=… romcnt=…` / `… END pos=… len=… cnt=…`，与本地 `NDS_CARTLOG2=LO-HI` 的 `cartlog2:` 行字段对齐。需要先打 `melonds-cartlog.patch`（见下） |

### 可选补丁：`melonds-armstat.patch`（每帧指令数，21-B9yi 续108e）

`REF_INSTRSTAT` 依赖 melonDS 的 ARM 核里一个诊断计数器（`ARM::InstrCount`，在
`ARM::Execute` 的解释器循环里每条 +1）。这不是 melonDS 上游代码，应用方式：

```powershell
git -C "$env:TEMP\melonds-ref" apply <仓库路径>\tools\ref\melonds-armstat.patch
ninja -C "$env:TEMP\melonds-ref\build-core" refhead
```

为什么值得打：这是判断「时序口径」差异最直接的量。实测（同键脚本）：

```text
稳定窗口 f=50..250 的每帧增量：本地 ARM9 3.63k / ARM7 14.5k，参考核 3.77k / 15.6k
⇒ 稳态每帧指令量两边只差 4%~7%（**不是**「本机跑得飞快」那种量级）
但累计到 f=50：本地 ARM9 15.41M vs 参考核 12.77M（+20%）、ARM7 1.01M vs 1.46M（-15%）
⇒ 差异集中在**开机段**（两边开机进度本就差几帧），稳态并不偏。
```

**`b9`/`b7`（BIOS 区指令数）是个很有用的负结果**（21-B9yi 续108h）：f=50 时参考核
只执行了 ARM9 BIOS **3,341** 条、ARM7 BIOS **25,806** 条 —— 即 melonDS **也是 HLE**
SWI 的，并不真跑 FreeBIOS 代码。所以「本地 HLE 掉 SWI ⇒ 工作量比参考核少」这条假设
**被证伪**，开机段的指令数差异另有原因（下一步查「开机被哪个事件/IO 门控」）。

同一个补丁里还带了 **PC 直方图**（`REF_PCHOT=1`，21-B9yi 续108p）：
`ARM.cpp` 的解释器循环每条指令对 `RefPcHist[core][(R[15]>>4)&0xFFFF]++`，
harness 退出前打印前 16 名。分桶与本模拟器 `NDS_PCHOT=1` 完全一致，
于是「同一段游戏进度、两边各在跑哪些代码」可以直接逐行对照。

**结论（f=50 同键脚本）**：两边的热点**是同一段代码**，只是 ARM9 的地址基数不同 ——
本地在 `0x020119xx`（主存直连地址），参考核在 `0x000119xx`（ARM9 主存**镜像区**
`0x00000000-0x003FFFFF`）：桶 `1191`/`1192`/`1193` 两边都进前三
（本地 33.4%/19.8%/34.5%、参考核 11.5%/25.5%/25.5%）；ARM7 侧 `0x023801xx`
↔ `0x000801xx`、`0x037FE3xx` ↔ `0x000FE3xx` 也一一对应。
⇒ 开机段（f≤50）的指令数差（本地 ARM9 15.41M vs 参考核 12.77M、+20%）
**不是「跑了不同的代码」**，而是**同一条循环上两边的计费/分支比例不同**
（本地更集中在桶 `1191`/`1193`，参考核更集中在 `1192`），与
`NDS_INSTRSTAT`/`NDS_MEMTIM`（续108e/g）的结论一致。

**同时暴露一处待解释的 ARM7 差异**：参考核 ARM7 的第一热点是 `0x000FB82x`
（6.1%，本次窗口内没有对应的本地热点），本地 ARM7 第一热点是 `0x023801xx`
（12.0%，参考核对应桶 `0x000801xx` 也有 5.7%）⇒ ARM7 侧「跑的热点集合」
比 ARM9 侧更像真有差异，留待下一轮（与「ARM7 消息条数多 ~19%」同一线索）。

### 可选补丁：`melonds-cartlog.patch`（卡带传输级日志，21-B9yi 续108j）

```powershell
git -C "$env:TEMP\melonds-ref" apply <仓库路径>\tools\ref\melonds-cartlog.patch
ninja -C "$env:TEMP\melonds-ref\build-core" refhead
```

实测（同键脚本、开机段）：

```text
参考核 f=9  refcartlog: START cmd=B7000A4A len=512 romcnt=A1416657
            refcartlog: END   pos=512 len=512 cnt=0
            refcartlog: START cmd=B8000000 len=4   romcnt=A7416000   ← B8 = 读芯片 ID
            refcartlog: START cmd=B7000A4C len=512 romcnt=A1416657   ← 下一块（地址 +0x200）
本地（同帧）同一套命令/长度/递增规律，但**每帧 ~52 次传输**（≈45 块）
```

⇒ 两边的命令流**同构**（`B7` 512 字节块 + `B8` 芯片 ID 交替，地址按 0x200 递增），
差异纯粹在**节奏**：本地一次循环迭代能凑满一整块，参考核要 ~10 倍时间。

## 主内存逐字节对账（21-B9yi 续91）

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\ref\ramcmp.ps1 `
    -Rom "tools\Z 最终幻想12…nds" -Frame 80
```

原理：参考核在固定帧号（以及 `REF_RAMDUMP_FRAME` 指定的帧）把 4MB 主内存写成文件；
本地用 `--headless-frames N --dump` 跑到同一帧写出一份，再逐字节比较、列出差异区间。

**用它测出来的结论（2000 帧内）**：

```
f=50 … f≈297 ：两核主内存**只差约 238 字节（0.006%）**（逐帧同上）
f=298        ：差 4034 字节（0.10%）
f=299        ：差 17,724 字节（0.42%）
f=350        ：差 1,917,883 字节（45.7%）⇒ 从这里开始走成两个不同的场景
```

⇒ 本模拟器与参考核在**游戏状态层面几乎一致**，分叉起点被精确定位到 **f≈298**；
差异集中在一小撮游戏变量（首个差异字节 `0x0206C200`，另有 `0x02076FEC` 等）。
同一帧的寄存器对照显示 2D/VRAMCNT 全一致，只有 **MASTER_BRIGHT 的渐变相位**不同
（本地在下降、参考核在上升）⇒ 怀疑是某个定时/计数的累积差，属于下一步的深挖点。

**两个坑（重要）**：

* 参考核是在 `frame == N` 时 dump，此时它已经跑了 **N+1** 帧；本地 `--headless-frames N`
  只跑 N 帧 ⇒ 比较时要么用 `N+1`，要么接受一帧偏移（续91 两种都测过，结论不变）。
* 本地 `NDS_IODUMP_FRAME` 以前沿用「上一核」的上下文，而 VRAMCNT / DISP3DCNT 是
  ARM9 专属 ⇒ 帧末若最后执行 ARM7，这些寄存器会被读成 0、与参考核**假性不符**。
  续91 已改成**强制 ARM9 上下文**再读。

> 21-B9yi(续107)：本地**无头模式现在也会把存档写回 `<rom>.sav`**（此前只有窗口路径写）。
> 做「同一 ROM 跑两次再互相对账」时，先保证两次的起始 `.sav` 一致（要么都删掉、要么先备份），
> 否则第二次运行会从第一次留下的存档起步。`tools\statetest.ps1` 已经内置了这个还原逻辑。

## 运行注意（血泪教训）

* **必须沙箱外运行**：`refhead.exe` 在代码沙箱内启动会被**挂起**（进程存在、CPU 0%、
  无任何输出），看起来像「程序坏了」。实测在沙箱外运行正常（无参数时按 `main` 立刻
  返回 1）。诊断口径：`Get-Process refhead | Select-Object CPU` 一直是 0.00 ⇒ 不是慢，
  是被挂起。
* 源码树里的**诊断补丁是手写的**（`SPI.cpp` 的 `spiread`、`GPU3D.cpp` 的四个设施），
  重建时如果报 `NDS::ARM7` 之类的编译错，说明那份补丁与当时的 melonDS 头文件不一致，
  改成成员访问（`NDS.ARM7.R[15]`）即可 —— 21-B9yi 续86 就是这么修的。

## melonDS 侧需要的补丁

`melonds-gpu3d.patch` 是对 melonDS `src/GPU3D.cpp` 的全部改动（`REF_GXHIST` /
`REF_POLYDBG` / `REF_GXDBG_FRAME` / `REF_MAT_FRAME` 四个诊断设施），重建时：

```powershell
git -C "$env:TEMP\melonds-ref" apply <仓库路径>\tools\ref\melonds-gpu3d.patch
```

## 把参考帧转成图片

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\ref\fb2bmp.ps1 `
           -In "$env:TEMP\ref_fb_4000.bin" -Out build\ref_4000.bmp
```

（**21-B9yi 续108m 更正**：本脚本原来把参考核 dump 的**第一个**屏放到图片上方，而
melonDS 的 `GetFramebuffers(&top,&bottom)` 交回来的第一个其实是**底屏**
（实测它的统计与非黑像素数、均值都与本地底屏逐值相同）⇒ 输出图与本地 `--shot` 的
「顶屏在上」**上下颠倒**。已修好：现在两个脚本/截图版式一致。
脚本保持纯 ASCII —— Windows PowerShell 5.1 按 ANSI 读 `.ps1`，非 ASCII 注释会解析失败。）

## 画面逐像素对账：`tools/ref/fbcmp.ps1`（21-B9yi 续108m）

```powershell
# 1) 本地跑同一脚本、同一批帧，并**关掉主亮度**（参考核帧缓冲是 MASTER_BRIGHT 之前的）
$env:NDS_NOMB='1'; .\build\nds-emu.exe <ROM> --headless-frames 251 --key-frame 1 `
    --key-mask 0x3FF --key-period 120 --shot-every 1 --shot-prefix build\fb
# 2) 参考核：REF_SHOT_EVERY 每 10 帧导出 %TEMP%\ref_fb_<frame>.bin
# 3) 对账（注意参考核 dump 在 frame==N 时已跑 N+1 帧 ⇒ 本地取第 N+1 帧的截图）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\ref\fbcmp.ps1 `
    -Ref "$env:TEMP\ref_fb_120.bin" -Bmp build\fb_00121.bmp
```

**实测结果（同键脚本、同一批帧）**：

```text
ref f=120 vs 本地 f=121：底屏差 0/49152、顶屏差 0/49152（MAE 0.0000）⇒ 逐像素完全一致
ref f=250 vs 本地 f=251：同上（0/0）⇒ 逐像素完全一致
ref f=50  vs 本地 f=51 ：底屏差 2864、顶屏差 6614（MAE≈7~8）—— 该帧仍在淡入过渡，
                          两边游戏状态本身还有 238 B 差异，属预期
```

⇒ 只要两边游戏状态一致，**本模拟器的画面与参考核逐像素相同**（含 2D 图层/3D 图层/抗锯齿/
主亮度之前的那一级合成）。

## 已知口径差异（对照时必须记住）

* `GPU.GetFramebuffers()` 返回的是 **32 位/像素**、且是**加 MASTER_BRIGHT 之前**的
  合成结果；本地 `render_frame()` 是最终的 24 位画面。做 RGB 均值/逐像素对照时，
  本地要配 `NDS_NOMB=1` 才与之同口径。
* `906e9eb` 这一版 master 与「历史那份参考核」从 f=40 起就不一样（见
  `docs/21-rom-bringup.md` 续29），所以它只能做**结构性/早期帧**对照，
  不能当作后期帧的逐像素判定依据。

## 触摸诊断补丁（21-B9yi 续59）—— 两处小改动，用在临时参考核树里

结论先写：参考核证明「**游戏靠 EXTKEYIN bit6（PENIRQ，低有效）判断有没有触摸，
有触摸才去轮询 SPI 取坐标**」。本地此前 `EXTKEYIN` 恒返回 0x7F（永远“抬起”），
于是运行期从不读触摸数据寄存器；补上这一位后本地与参考核行为一致
（注入触摸时每秒帧读 SPI ~24 次，不触摸时一次不读）。

参考核（`%TEMP%\melonds-ref`，属临时树，改动不入库）需要两处：

1. `src/SPI.cpp`：在 `namespace melonDS {` 之后加一个全局计数
   ```cpp
   unsigned long long SPI_DataReadCount = 0;
   ```
   并在 `SPIHost::ReadData()` 的 `if (dev < SPIDevice_MAX)` 分支里
   `SPI_DataReadCount++;`（可再打印前 40 次的 PC 定位读取者）。
2. `src/ref_harness.cpp`：加触摸注入开关
   `REF_TOUCH_FRAME / REF_TOUCH_X / REF_TOUCH_Y / REF_TOUCH_PERIOD`
   （坐标是**原始 ADC**，与本地 `adc = 0x200 + (px-33)*16` 同口径），
   帧循环里到点调 `nds->TouchScreen(x, y)`、保持 12 帧后 `nds->ReleaseScreen()`
   （与本地 `--touch-*` 的按/抬时序一致）；结束打印 `refspi: datareads=…`。

实测（12500 帧、满键、f=11800 起每 40 帧点一次）：

```
参考核 不触摸：refspi: datareads=842          本地 不触摸：842（同）
参考核 注入触摸：refspi: datareads=18122      本地（修复前）：842（漏）
                                              本地（修复后）：70080/3000 帧（≈24/帧，同参考核）
```

## 本地侧对账工具：`tools/statetest.ps1`（21-B9yi 续107）

不依赖参考核，只回答一个问题：**即时存档是不是精确的**。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\statetest.ps1 `
    -Rom tools\rom_ascii.nds -Frame 1000 -Run 2000 `
    -Extra "--key-frame,1;--key-mask,0x3FF;--key-period,120"
```

三趟：①连续跑到 `Frame+Run`；②跑到 `Frame` 并存一份即时存档；③读该存档再跑 `Run` 帧。
然后把两边的**主存/VRAM/ITCM/DTCM/ARM7WRAM/共享WRAM/调色板/io9/io7** 逐个逐字节比较，
输出每项的差异字节数并给 `PASS/FAIL`。`-Extra` 是给模拟器的附加参数，用**分号**分隔
（逗号留给参数自身，如 `--touch-drag 64,128,150,90`）。

> 为什么值得单独做工具：这些实验最容易被「上一轮的残留 dump 文件」和「`--dump` 前缀丢失」
> 骗到（后者是 Windows 上 `wargv` 的 use-after-free，见 `src/mainlog.md` 续107）。
> 脚本会先清掉旧产物，缺文件就报错，并把真正跑的那条命令行回显出来。
