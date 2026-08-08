# tests 模块日志

> 统一测试入口：`tests/test_nds.c`。只链核心库 `ndscore`（不依赖 SDL/窗口），
> 由 `ctest --test-dir build` 收集，也可直接运行 `build/test_nds.exe`。

## 5.1 统一测试入口

- CMake 重构：抽 `ndscore` 静态库（nds/bus/cpu/exec/cart/arm9/arm7，均无 SDL 依赖）；
  `nds-emu`（+SDL 部分）与 `test_nds`（仅核心）都链它；`enable_testing()` + `add_test`。
- 迷你测试框架：`CHECK_EQ(name, got, want)` 宏 + 全局统计（g_checks/g_failures），
  退出码 0=全过 / 非 0=有失败；`run_program()` 通用驱动（装程序 → 跑 CPU → 到停机/上限）。
- 从 `main.c` 迁移已验证的自测：
  - `test_bus_rw`：8/16/32 位小端读写、字节序、VRAM 区间、IO 桩（原阶段 2）；
  - `test_arm_instructions`：22 条指令序列 + 位运算 + 写 VRAM 综合（原阶段 3b）；
  - `test_cpu_draw_vram`：顶屏黄十字 + 底屏纯绿（原阶段 4.5）。
- `main.c` 移除 `selftest_3b` 与断言工具，保留显示 demo（测试图 + CPU 十字/绿屏）作窗口内容。

## 5.2 用例：清屏

- 程序：整屏 256×192 填约定底色 `0x0000`（一次 STR 32 位写 2 像素，计数 24576 字循环）。
- 测试先把顶屏 framebuffer 全涂 `0xFFFF`，跑程序后逐像素断言全为 `0x0000`，
  证明清屏确实由 CPU 覆盖完成。98308 步跑完。

## 5.3 用例：画矩形

- 程序：先整屏清黑，再在左上象限 x∈[0,128)、y∈[0,96) 画红色实心矩形。
- **发现并修复 2 像素 bug**：STR 是 32 位写 = 2 个 RGB555 像素，若只把颜色放进低半字
  （`0x7C00`），则每 2 像素只有第 1 个着色——矩形/十字实际是竖条纹（6144=96×64 个奇数像素全黑）。
  修复：颜色构造为两个字半同值（`0x7C007C00`，MOV + ORR 两条指令）。
- 逐像素全屏扫描：矩形内全红、矩形外全黑，123272 步跑完。

## 5.4 用例：死循环保活

- 程序：`B self`（`0xEAFFFFFE`）。跑 10 万步断言不崩、PC 稳定停在原地、cycles=100000。

## 5.5 README：如何跑测试

- `README.md` 新增「运行测试」小节：配置/构建/`ctest`/直接运行 `test_nds.exe`、退出码含义、新增用例方法。

## 5.6 可选文档：未来 devkitARM（仅文档）

- 新增 `docs/06-devkitarm.md`：devkitARM 是什么、当前为何不需要、未来接入清单、
  与真机地址/阶段 9 的关系。只写文档不实现。

## 全量验证

`ctest --test-dir build` 与直接运行均 40 项检查 0 失败。
