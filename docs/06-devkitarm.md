# 未来可选：devkitARM 工具链（仅文档，不实现）

> 本文只记录「以后若想用真 NDS 工具链编译 homebrew 该怎么接」，当前阶段**不做**。

## 它是什么

[devkitARM](https://devkitpro.org/) 是 NDS/GBA 等 ARM 平台的交叉编译工具链 + 运行时库。
装上后可以：

- 用 C/汇编编写真正的 NDS homebrew 程序；
- 编译出标准 `.nds` 卡带镜像（带 ARM9/ARM7 头）；
- 提供 `nds.h` 等头文件，地址常量（VRAM、REG_DISPCNT…）与真机一致。

## 为什么现在不需要

当前开发方式是**手工汇编指令字写进 C 数组**（见 `tests/test_nds.c`）。优点：

- 不依赖任何外部工具链，随时可编译可测；
- 每条指令的编码都亲手写过，理解最透彻（符合本项目的学习目标）；
- 验收用例（清屏/画矩形/死循环）足以覆盖当前实现的指令子集。

## 以后若要接入，怎么做

1. 安装 devkitARM（Windows 用 devkitPro 一键安装器），把 `DEVKITARM` 环境变量指到安装目录。
2. 写一个最小 homebrew（如屏幕 `SetMode` + `BG` 刷色块），用 `arm-none-eabi-gcc` 编译出 `.nds`。
3. 用现有 `nds-emu homebrew/xxx.nds` 装载运行（阶段 1 的装载链路已支持任意 `.nds`）。
4. 对比真机/模拟器画面，反向打磨 CPU/PPU 精度。

### 接入清单（未来做时才逐条勾选）

- [ ] 安装 devkitPro + devkitARM，验证 `arm-none-eabi-gcc --version`
- [ ] 建立 `homebrew/` 下独立的 Makefile/工程（不动模拟器构建）
- [ ] 最小出图例程（线性 framebuffer 模式）
- [ ] 用 `nds-emu` 装载运行并截图对比

## 与真机地址的对应关系

当前模拟器的 framebuffer 约定是**教学简化**（顶屏 = VRAM 起始 256×192 RGB555，
见 `docs/05-framebuffer.md`），与真机「VRAM bank + DISPCNT 控制 + BG 层」不同。
未来做阶段 9「真 2D PPU」时会逐渐对齐真机显示控制寄存器；届时 devkitARM 编译的
homebrew 才有意义——在那之前，本模拟器只消费自己的线性 framebuffer 约定。
