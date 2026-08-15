# 本仓库目录说明

```
test/
├── CMakeLists.txt        # 怎么编译整个项目（ndscore 核心库 + nds-emu + test_nds）
├── README.md             # 如何构建与运行
├── DEVLOG.md             # 开发日志总索引（模块日志清单 + 按时间索引）
├── NDS-微步骤拆解.md      # 开发计划主文档（微步拆解 + 进度）
├── docs/                 # 零基础中文笔记（00 概念系列 + 01 目录 + 各阶段专题）
├── src/                  # 源码根（include 根：main.c 用 "模块/模块.h" 引用）
│   ├── main.c            # 程序入口：参数解析 + 装载编排 + 主循环 + 键位映射
│   ├── audio/            # 主机侧 SDL 音频：设备 + 回调（调 snd_render 取样本）
│   ├── demo/             # 无 ROM 时的演示脚手架（2D 出图 + 音频提示音）
│   ├── runner/           # headless 诊断 runner（--headless N / --trace 定位卡点）
│   ├── window/           # SDL 窗口/渲染器生命周期、缩放、退出
│   ├── menu/             # 菜单 UI（SDL_ttf 文本/下拉/缩放）
│   ├── nds/              # 整机状态 nds_t（把 bus/cpu/cpu7/io 捏在一起）
│   ├── cpu/              # ARM 解释器（cpu 接口 + arm9/arm7/exec/thumb 功能文件）
│   ├── bus/              # 内存读写：地址 → 区间换算（Main RAM/VRAM/WRAM/调色板/OAM/IO）
│   ├── cart/             # 卡带（cart 接口 + arm9/arm7 头解析 + key1 解密 + cartbus + save）
│   ├── bios/             # BIOS HLE（bios 接口 + arith/mem/decompress/wait 功能文件）
│   ├── io/               # IO 寄存器区（io 接口 + irq/timer/key/dma/fifo/disp/touch 功能文件）
│   ├── ppu/              # 显示（ppu SDL 侧 + render 纯渲染）
│   ├── snd/              # 音频硬件 + 混音（纯计算，不含 SDL）
│   └── gx/               # 3D 几何引擎（GXFIFO + 矩阵/顶点变换 + 光栅化）
├── tests/                # 统一测试入口 test_nds.c（只链 ndscore，不碰 SDL）
└── homebrew/             # 自制验收数据（make_fake_rom.py + mini.nds）
```

## 模块目录约定

每个功能模块一个目录 `src/<模块名>/`，源码与日志同目录：内含 `<模块名>.h`、`<模块名>.c`、`<模块名>log.md`（如 `src/cpu/` 内含 `cpu.h`、`cpu.c`、`cpulog.md`）。`main.c` 通过 `#include "<模块名>/<模块名>.h"` 引用（`src/` 为 include 根）。

若模块内有多个子功能，按功能拆成 `<功能>.h/.c` 功能文件；`<模块名>.h/.c` 作对外接口文件，外部只依赖接口文件（模块间解耦），功能文件各司其职（模块内高内聚）。例：`src/cart/` 含接口 `cart.h/.c` + 功能 `arm9`、`arm7`、`key1`、`cartbus`、`save`。

## 两层分层：核心库 vs 宿主

- **`ndscore` 核心库**（`src/` 下除 main/audio/demo/runner/window/menu/ppu.c 外的模块）：纯模拟逻辑，**不依赖 SDL**，供 `nds-emu` 与 `test_nds` 共用。
- **宿主侧**（`main.c` + `window/` + `menu/` + `audio/` + `demo/` + `runner/` + `ppu.c`）：SDL 窗口/渲染/音频设备/演示/诊断，只有 `nds-emu` 链它们。

一个易混点：`snd/`（音频硬件 + 混音，纯计算）与 `audio/`（主机侧 SDL 设备 + 回调）是两件事——前者在核心库，后者在宿主侧，`audio` 回调从 `snd_render` 取样本。

## 数据怎么流

1. `cart` 读文件 → 填进 `bus` 管理的 RAM
2. `cpu` 从 `bus` 取指令并执行
3. 程序写 VRAM → `ppu`（`render`）读出来 → `ppu.c` 用 SDL 上传纹理 → `window` 画到窗口
4. 音频：游戏写寄存器 → `snd` 混音 → `audio` 回调送声卡
5. 无 ROM 时：`demo` 写演示数据 → 上面的显示/音频链路

先把文件夹名对上号即可，细节在后面各篇文档展开。
