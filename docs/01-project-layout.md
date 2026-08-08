# 本仓库目录说明

```
test/
├── CMakeLists.txt      # 怎么编译整个项目
├── README.md           # 如何构建与运行
├── docs/               # 零基础中文笔记（你正在读）
├── src/
│   ├── main.c          # 程序入口：开窗口、主循环
│   ├── window/         # 窗口/渲染器生命周期、缩放、退出（window.h/.c/windowlog.md）
│   ├── menu/           # 菜单 UI、字体、文本渲染（menu.h/.c/menulog.md）
│   ├── nds/            # 整机状态 nds_t（把各模块捏在一起，nds.h/.c/ndslog.md）
│   ├── cpu/            # ARM 解释器（先 ARM9，后 ARM7）
│   ├── bus/            # 内存读写：地址 → 哪一块数组
│   ├── cart/           # 打开 .nds，把代码拷进 RAM
│   ├── ppu/            # 把显存变成屏幕像素
│   ├── irq/            # 中断、定时器、按键
│   └── dma/            # DMA（硬件搬运内存）
├── tests/              # 用 C 数组机器码做的单元测试
└── homebrew/           # 自制验收数据（嵌入机器码 / 假 ROM）
```

## 模块目录约定

每个功能模块一个目录 `src/<模块名>/`，源码与日志同目录：内含 `<模块名>.h`、`<模块名>.c`、`<模块名>log.md`（如 `src/cpu/` 内含 `cpu.h`、`cpu.c`、`cpulog.md`）。`main.c` 通过 `#include "<模块名>/<模块名>.h"` 引用（`src/` 为 include 根）。

## 数据怎么流

1. `cart` 读文件 → 填进 `bus` 管理的 RAM
2. `cpu` 从 `bus` 取指令并执行
3. 程序写 VRAM → `ppu` 读出来 → `main` 用 SDL 画到窗口

先把文件夹名对上号即可，细节在后面各篇文档展开。
