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
| **鼠标按住底屏** | **触摸屏**（按下=触点、拖动=笔移动、抬起=抬笔） |
| 鼠标点顶部菜单栏 | 切换缩放（1x–4x）/ 语言（中/英） |

退出时会把存档写回与 ROM 同名的 `.sav`。

窗口模式可打印实测帧率（含渲染/音频宿主开销）：

```sh
build/nds-emu.exe "<ROM>" --frames 20000 --key-frame 1 --key-mask 0x3FF --key-period 120 --fps-every 2000
#   fps: frames=0..2000  28018 ms  71.4 fps
#        phases: events=7 ms  emulation=25114 ms  other=6763 ms
#        render: ppu=4055 ms  sdl(clear/menu/present)=2708 ms
```

## 命令行开关（常用）

| 开关 | 说明 |
|------|------|
| `--frames N` | 窗口模式跑满 N 帧自动退出（冒烟测试），退出时写回 `.sav` |
| `--headless-frames N` | 无头模式跑 N 帧（不开窗口/音频），用于长跑、对照、批量验证 |
| `--headless N` / `--headless-cycles N` | 按步数/周期跑（bring-up 诊断用） |
| `--shot 文件.bmp` | 结束时把双屏截图写成 BMP（顶屏在上） |
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
| 画面 | 窗口模式 70–91 fps（12000 帧平均 80）；渲染改动**逐像素零回归**（截图 SHA-256 不变） |
| 键盘 | 全部按键可用（多轮长跑） |
| 触摸 | **四层闭环**：ARM7 按 ~24 次/帧轮询 SPI → 位置不同读数不同且与手算吻合 → 自然输入下改变游戏进程 → UI 层可见反应（画面指纹分叉 + 像素差） |
| 音频 | SDL 回调按真实时钟推进；活跃段混音非静音 |
| 存档 | 读（启动加载 8192B）+ 头写入（"BLIZ"）+ 协议单测 + `.sav` 写回；**完整游戏内存档待人工** |
| 健壮性 | 20 万帧量级（固定/随机/多种子）无停摆；未知 IO 始终 139 种、无未实现 SWI/指令 |
| 进度深度 | 自然输入 20000 帧跑过 **64–67 张不同画面**（含过场、战斗地图等） |

**仍需人工确认的两项**：①战斗内拖拽下令；②游戏内存档菜单保存。
两者都有客观判定手段：画面指纹（`--screen-hash-every`）与 `savechip:` 一行
（见 `docs/21-rom-bringup.md` 续77/78/80）。

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
