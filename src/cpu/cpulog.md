# cpu 模块日志

## 3a.2 — arm_cpu_t

- 新建 `src/cpu/cpu.h` / `cpu.c`（模块目录结构见 `docs/01`）。
- `arm_cpu_t`：`nds`（所属机器）、`r[16]`（r15 即 PC）、`cpsr`、`cycles`。
- `cpu_create(nds, reset_pc)` / `cpu_destroy` / `cpu_reset(reset_pc)`。
- `nds_t` 前向声明 `arm_cpu_t` 并挂入 `nds->cpu`；`nds_create` 中创建。

## 3a.3 — cpu_fetch

- `cpu_fetch()` 按 `r[15]` 从 bus 读 32 位指令字（复用阶段 2 的 `bus_read32`）。

## 3a.4 / 3a.5 — cpu_step + 死循环识别

- `cpu_step()`：取指 → 译码高 4 位条件码（`insn>>28`）与 3 位大类（`insn>>25 & 7`）。
- 识别「无条件 B（AL + 101 大类）且立即数 = -1」：目标 = `PC+8-8 = PC`，原地打转。
- `deadloop_reported` 标志：死循环只打印一次，避免主循环刷屏。

## 3a.6 — 未实现指令

- 其它指令：打印机器码与 PC，PC += 4 继续（阶段 3b 逐类实现）。

## 3a.7 — 主循环接入

- 装载镜像后 `cpu_reset(nds->cpu, hdr.arm9.entry)`。
- 主循环每帧执行固定 8 步，每 60 帧打印 PC/cycles。

## 修复：mini.nds 布局错误

- 假 ROM 原设 `ram=0x02000000` 但 `entry=0x02000800`，入口落在镜像外（读 0）。
- `make_fake_rom.py` 改为 `ram = entry = 0x02000800`，重新生成 `mini.nds`；
  取指读回 `EAFFFFFE`（死循环指令），PC 稳定停在入口。
