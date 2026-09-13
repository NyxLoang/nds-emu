# nds-emu

一个学习用 NDS 模拟器（C11 + CMake + SDL2），按 `NDS-微步骤拆解.md` 分阶段开发。
目标 ROM：`tools/Z 最终幻想12 亡灵之翼 6.5全剧情修正版(简)(PGCG汉化组+巴士汉化组)(1024Mb).nds`。

## 依赖

- **工具链**：C11 编译器 + CMake 3.16+（本机用 WinLibs 的 gcc 16.1 / cmake 4.4）
- **SDL2 2.26.3 / SDL2_ttf 2.24.0**：源码包放在 `tools/sdl2/`、`tools/sdl2_ttf/`，
  由 CMake FetchContent 在配置时自动解压编译，构建过程完全离线，不访问 GitHub。

## 构建

```sh
cmake -S . -B build
cmake --build build --target nds-emu test_nds
```

## 怎么玩（窗口模式）

```sh
build/nds-emu.exe "tools/Z 最终幻想12 亡灵之翼 6.5全剧情修正版(简)(PGCG汉化组+巴士汉化组)(1024Mb).nds"
```

| 输入 | 作用 |
|------|------|
| `Z` `X` `S` `D` | NDS 的 A / B / X / Y |
| `A` `F` | L / R |
| `Enter` `Backspace` | START / SELECT |
| 方向键 | 十字键 |
| **按住 `Tab`** | **快进 ×4**（跳过过场用；快进时音频静音、SPU 跟随模拟时间，`NDS_FF_MUL` 可改倍数） |
| **鼠标按住底屏** | **触摸屏**（按下=触点、拖动=笔移动、抬起=抬笔） |
| 鼠标点顶部菜单栏 | 切换缩放（1x–4x）/ 语言（中/英） |

退出时会把存档写回与 ROM 同名的 `.sav`。

窗口模式可打印实测帧率（含渲染/音频宿主开销）：

```sh
build/nds-emu.exe "<ROM>" --frames 20000 --key-frame 1 --key-mask 0x3FF --key-period 120 --fps-every 2000
#   fps: frames=0..600  3363 ms  59.5 fps      ← 默认：帧节奏把每帧对齐真机 59.83 fps
#        phases: events=1 ms  emulation=623 ms  other=2739 ms
#        render: ppu=231 ms  sdl(clear/menu/present)=68 ms
```

**帧节奏（frame pacing）**：默认把窗口帧率对齐 NDS 真机 **59.8261 Hz**（16.715 ms/帧）。
原因：轻场景下模拟器能跑到 150–265 fps，**快于真机**，而音频回调是按真实时间驱动 SPU
⇒ 声音会与画面脱节；对齐后音画同步（`fps:` 显示 ~59.8，多出来的时间是 `other` 里的
节奏等待，不是卡顿）。

```sh
# 测「不设上限」的真实性能（自动化压测同样用这个）：
NDS_NOSYNC=1 build/nds-emu.exe "<ROM>" --frames 600 --fps-every 300
#   fps: frames=0..300  1130 ms  265.5 fps
#   fps: frames=300..600  1742 ms  172.2 fps
# 快进：--speed 2（上限 119.65 fps；场景太重时跑不满属正常）
```

## 命令行开关（常用）

| 开关 | 说明 |
|------|------|
| `--frames N` | 窗口模式跑满 N 帧自动退出（冒烟测试），退出时写回 `.sav` |
| `--speed N` | 帧节奏倍速：`1`=真机速度（默认）、`2`=2 倍速快进、`0`=不限速（等同 `NDS_NOSYNC=1`） |
| `--snd-wav 文件.wav` | 无头模式把**模拟音频**导成 WAV（32768 Hz/16bit/立体声，时间轴=模拟周期，同脚本产物逐字节相同） |
| `--headless-frames N` | 无头模式跑 N 帧（不开窗口/音频），用于长跑、对照、批量验证 |
| `--headless N` / `--headless-cycles N` | 按步数/周期跑（bring-up 诊断用） |
| `--shot 文件.bmp` | 结束时把双屏截图写成 BMP（顶屏在上）；**窗口模式也生效**（退出时存图） |
| `--shot-every N --shot-prefix P` | 每 N 帧存一张 `P_00000.bmp`（画面时间线） |
| `--stats-every N` | 每 N 帧打印双屏统计（非黑像素数 + 均值 RGB） |
| `--screen-hash-every N` | 每 N 帧打印双屏**画面指纹**（统计「跑过多少张不同画面」） |
| `--key-frame F --key-mask M --key-period P` | 键盘脚本：第 F 帧起每 P 帧按 12 帧 |
| `--key-random SEED [--key-period P]` | 随机按键浸泡（每 P 帧取一个随机键掩码） |
| `--touch-frame F --touch-x X --touch-y Y --touch-period P` | 触摸脚本（底屏像素坐标） |
| `--touch-drag X1,Y1,X2,Y2 [--touch-drag-steps N]` | 拖拽手势（按帧分 N 步移动后抬起，可周期重复） |
| `--touch-random SEED [--touch-period P]` | 随机位置轻触浸泡 |
| `--dump 前缀` | 结束导出 `_mainram/_arm7wram/_sharedwram/_vram/_itcm/_dtcm` 内存镜像 |
| `--watch LO-HI` / `--watch-r LO-HI` | 写/读监视：打印访问者 PC/LR/SP（最多 4 组区间） |

## 诊断开关（环境变量）

| 变量 | 说明 |
|------|------|
| `NDS_PCHOT=1` | ARM9 热点 PC 直方图（显著拖慢，默认关） |
| `NDS_PROF=1` | 阶段剖析（需 `-DNDS_PROF=ON` 配置的构建） |
| `NDS_NORAST=1` | 只跳过像素级光栅化（行为无关，用于量化光栅化占比） |
| `NDS_NOFAST=1` | 关闭访存快路径（A/B 对照） |
| `NDS_NORENDER=1` / `NDS_NOAUDIO=1` | 窗口模式跳过宿主渲染 / 不打开声卡 |
| `NDS_NOSYNC=1` | 关闭帧节奏（全速运行，用于压测；默认按真机 59.83 Hz 限速） |
| `NDS_UNKIOSUM=1` | 无头跑完打印**全部未知 IO 地址**（含读写方向），用于跟参考核寄存器表核账 |
| `NDS_FF_MUL=4` | 快进倍数（默认 4；按住 `Tab` 生效） |
| `NDS_FF_HOLD=1` | 启动即视为按住快进（自动化验证/基准用，等价于一直按着 `Tab`） |
| `NDS_SNDSTAT=1` | 按秒打印混音输出非静音样本数与峰值 |
| `NDS_GXHIST` / `NDS_BGDBG` / `NDS_MAT_FRAME` / `NDS_POLYDBG` / `NDS_IODUMP_FRAME` … | GX/2D/IO 诊断（细节见 `docs/21-rom-bringup.md`） |

## 运行测试

统一入口 `tests/test_nds.c`，只链核心库 `ndscore`（不依赖 SDL、不开窗口）：

```sh
ctest --test-dir build          # 或直接 ./build/test_nds.exe
```

当前规模：**931 项检查、0 失败**（覆盖 CPU 指令集/Thumb、内存总线与 VRAM 映射、DMA、
中断/定时器、卡带协议、存档芯片、触摸 SPI、音频混音、2D PPU（含 16 色图块格式）、3D GX 等）。
新增用例：在 `tests/test_nds.c` 加 `test_xxx(nds)` 并在 `main` 调用，断言用
`CHECK_EQ(name, got, want)`。

## 当前状态（均有证据，详见 `docs/21-rom-bringup.md`）

| 维度 | 状态 |
|------|------|
| 画面 | 默认帧节奏输出真机 59.83 fps（音画同步）；**满速能力 70–265 fps**（重场景/轻场景，`NDS_NOSYNC=1` 可看）；渲染改动**逐像素零回归**（截图 SHA-256 不变） |
| 键盘 | 全部按键可用（多轮长跑） |
| 触摸 | **四层闭环**：ARM7 按 ~24 次/帧轮询 SPI → 位置不同读数不同且与手算吻合 → 自然输入下改变游戏进程 → UI 层可见反应（画面指纹分叉 + 像素差） |
| 音频 | SDL 回调按真实时钟推进；`--snd-wav` 导出的 WAV 有明确结构（开机静音→标题音乐淡入→过场 BGM，lag-1 自相关 0.999 属乐音），同脚本**逐字节可复现**；试听片段 `docs/audio/` |
| 存档 | 读（启动加载 8192B）+ 头写入（"BLIZ"）+ 协议单测 + `.sav` 写回；**完整游戏内存档待人工**（判定：窗口退出时 `savechip:` 的 `nonzero` 从 24 明显变大） |
| 健壮性 | 20 万帧量级（固定/随机/多种子）无停摆；未知 IO 始终 139 种、无未实现 SWI/指令 |
| 进度深度 | 自然输入 20000 帧跑过 **64–67 张不同画面**（含过场、战斗地图等） |

**仍需人工确认的两项**：①战斗内拖拽下令；②游戏内存档菜单保存。
两者都有客观判定手段，**不用读代码**：

- **游戏内存档**：玩到能存档的地方存一次，然后关窗口，看退出时那行
  `savechip: … nonzero(vs 0xFF)=…`——只要这个数**从 24 明显变大**（例如几百/上千），
  就说明游戏真的写进了存档芯片（`24` = 目前「只写过 BLIZ 头」的初始态）；
- **战斗内拖拽下令**：看画面/指令是否变化；想留证据就加 `--shot out.bmp`
  （退出时把双屏存成 BMP）或 `--screen-hash-every 250`（打印画面指纹）。

另外：**窗口模式与无头模式同脚本跑同一帧，截图逐字节相同**（f=300 与 f=2000 均已验证），
所以「自动化测量看到的」和「你玩到的」是同一套口径。

## 目录结构

见 `docs/01-project-layout.md`；阶段学习文档在 `docs/`（`00`…`21`），
Phase B bring-up 全过程与结论在 `docs/21-rom-bringup.md`。

## 开发日志

- **总索引**：[`DEVLOG.md`](DEVLOG.md)（只做索引）
- **模块详情**：各模块目录下的 `<模块名>log.md`（如 `src/mainlog.md`），模板见 `devlog/_template.md`
- **微步骤与验收约定**：`NDS-微步骤拆解.md`（含「验收内容 + 验收步骤」固定格式）

## 分支工作流

采用 git-flow 精简版：

```
main ──────────── 只放稳定版本（受保护，只能 PR 合并）
develop ───────── 日常开发主线（默认分支）
  ├─ feature/*   功能分支：从 develop 拉出，完成后 PR 合回 develop
  └─ debug/*     Bug 修复分支：从 develop 拉出，修复后 PR 合回 develop
```

- **develop**：GitHub 默认分支，所有开发都在此进行。
- **main**：受保护分支，`enforce_admins=true` 且要求 1 个 review，只能通过 PR 合并。
- **Bug 修复**：从 `develop` 拉 `debug/<名字>`，修复自测后 PR 合回 `develop`。
- 本机推送需走代理 `http://127.0.0.1:7897`（已写入本仓库 `http.proxy`）。
