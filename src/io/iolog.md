# io 模块日志

> 覆盖：IO 寄存器区（`0x04000000` 起）的真实寄存器语义。
> 按模块结构规则拆功能文件：`irq.h/.c`（中断）、`timer.h/.c`（定时器）、`key.h/.c`（按键），
> 对外接口 `io.h/.c` 负责按地址分发；bus 在 IO 区间转发到 `io_read8/io_write8`。

## 6.2 — 模块创建 + bus 路由

- **做了什么**：
  - `io_t` 聚合三个功能单元：`irq_t irq` + `nds_timer_t timer[4]` + `keypad_t keypad`。
  - `io_read8/io_write8` 按地址区间分发：IRQ 区 → `irq.c`、定时器区 → `timer.c`、按键区 → `key.c`；未实现寄存器读 0 写忽略（沿用阶段 2 桩语义）。
  - `bus_t` 去掉 `io[64KB]` 桩数组，改持 `io_t *io` 指针；`bus_read8/write8` 对 IO 区间转发给 io 模块。`nds_create` 创建 io 并 `bus->io = nds->io` 建立联系，逆序销毁。
  - `CMakeLists.txt` 的 `ndscore` 加入 `io.c/irq.c/timer.c/key.c`。
- **IRQ 寄存器实现**（`0x04000208/10/14`）：
  - IME/IE/IF 按 32 位寄存器存储、按字节读写；**IF 写 1 清除**（`ifl &= ~val`）。
  - `irq_pending()`：`(IF & IE) != 0 && IME bit0`，三者缺一不可。
- **怎么验证**：`test_irq_regs`——IME/IE 写读回、IF 置 VBlank 位、IF 写 `0xFFFFFFFF` 全清、只清 VBlank 位，5 项断言全过。

## 6.3 — 指令计数产生 VBlank（IF bit3）

- `io_set_vblank()` → `irq_set_vblank()` 把 `IF |= 0x0008`。
- 主循环每帧跑完 `steps_per_frame` 步后调用（真机是显示硬件自动置位，这里用指令计数近似）。
- **怎么验证**：`test_vblank_flag` 断言 `IF == 0x0008`；`test_wait_vblank` 程序轮询 IF 后能走出循环。

## 6.4 — 最小 IRQ 检测（不进异常向量）

- 只检测不跳转：`io_irq_pending()` 返回「是否真会发生中断」。主循环首次检测到打印一次 `irq: IRQ pending (VBlank ...)`。
- 真正跳 `0x18` 异常向量留到阶段 10+（计划 6.4 明示「可先只置位」）。
- **怎么验证**：`test_irq_pending` 四种组合（只 IF / IF+IE / 三者齐 / 清 IF）断言 pending 0→0→1→0。

## 6.5 — Timers 0-3 启停 / 分频 / 计数

- 每个定时器 4 字节：`TMxCNT_L`（base+0/1，计数值）、`TMxCNT_H`（base+2/3，控制）。
- 控制位：bit7=使能、bit0-1=分频（1/64/256/1024）；**写 CNT_H 重启计数器**（cnt_l/acc 清零）。
- `cpu_step` 每条指令调 `io_advance_timers()`（一条指令 ≈ 一个周期）；`timer_advance` 用内部累加器 `acc` 实现分频：攒够分频数才给 cnt_l +1，16 位自然回绕。
- **踩坑修复**：`timer_read8/write8` 原本用「全局偏移」（`addr - 0x04000100`）算字节位移，TM1 起偏移 ≥6 导致 `<<32` 未定义行为、控制字节写不进。改为 `% IO_TIMER_STRIDE` 取相对本定时器的字节偏移。
- **怎么验证**：`test_timers` 跑 640 步——TM0（1:1）计 640、TM1（1:64）计 10、TM2（禁止）计 0。

## 6.6 — KEYINPUT + SDL 键映射

- `KEYINPUT`（`0x04000130`，16 位只读）：低 12 位是按键位，**按下=0**，高 4 位恒 1。
- `io_set_keyinput(pressed)` 内部取反：`input = (~pressed & 0x0FFF) | 0xF000`；写忽略。
- `main.c` 事件循环捕获 `SDL_KEYDOWN/UP`，把 SDL 键码映射成 NDS 键位（`z/x/s/d/a/f/回车/退格/方向键`），维护 pressed 掩码后 `io_set_keyinput`。窗口模块保持不解耦机器（映射放组合根 main.c）。
- **怎么验证**：`test_keyinput` 三种按键组合读回；`test_key_program` 程序读 KEYINPUT 按 A 显示蓝、未按显示红。

## 6.7 — 等 VBlank 轮询 + 读键程序

- **等 VBlank 程序**（手写机器码）：
  `MOV r0,#0x04000000 → ADD +0x200 → ADD +0x14` 得 IF 地址 → `LDR r1,[r0]` → `ANDS r2,r1,#8` → `BNE` 跳出 → 写 VRAM 黄色 → `B self`。
  测试两轮：不置 VBlank 跑 64 步停在轮询循环（PC 在 0x0C-0x18、不写屏）；`io_set_vblank` 后从头跑走出循环、写屏停机（PC=0x28、VRAM[0]=0xFF00）。
- **读键程序**：读 KEYINPUT → `ANDS r3,r1,r2`（r2=1 即 A 键）→ 按下写蓝 `0x001F`、未按写红 `0x7C00`。
- **踩坑（手写汇编编码错误，都是「字节位序」问题）**：
  1. `ADD r0,r0,#0x200` 写成 `0xE2800802`（rot4=8→ROR16→加了 `0x00020000`），正确 `0xE2800C02`（rot4=12→ROR24）。
  2. `STR r4,[r3]` 两度写错：`0xE5844000`（Rn=4）与 `0xE5843000`（Rn=4、Rd=3），正确 `0xE5834000`（Rn 在字节1低半字节、Rd 在字节2高半字节）。
  3. `B 0x28` 写成 offset 1（跳到 0x2C），正确 offset 0。
  排查方法：`exec_set_trace(1)` 逐步打印 `STR r3, [r4]` 暴露真实解码结果。
- **怎么验证**：两个用例全过，阶段 6 累计 70 项检查 0 失败（ctest 通过）。

## 7.2/7.3 — DMA 功能文件 + 立即模式搬运

- 新建 `src/io/dma.h/.c`：`dma_channel_t`（sad/dad/cnt_l/cnt_h），只实现 DMA0 一条通道。
- 寄存器：SAD=`0x040000B0`、DAD=`0x040000B4`、CNT_L=`0x040000B8`（字数，0 按 0x4000）、
  CNT_H=`0x040000BA`（控制）。CNT_H 是 16 位寄存器 = 真机 32 位 CR 的高 16 位，
  因此 bit15=使能、bit10=32 位块、bit8=源固定、bit6=目的固定、bit11-13=模式（只支持立即）。
- **立即模式**：写 CNT_H 高位字节且使能位置位、模式=立即时，`dma_transfer` 同步拷贝 N 个
  字/半字（`bus_read/write16/32` 走完整地址换算），源/目的默认递增、可固定；搬完自动清使能。
- `io_t` 增加 `dma_channel_t dma` 与 `struct bus *bus` 反指（`nds_create` 里 `io->bus = nds->bus`），
  io_read8/write8 路由 DMA 区；`CMakeLists` 的 ndscore 加入 `dma.c`。
- **踩坑**：`dma.h` 参数列表里首次出现 `struct bus` 会被 C 视为参数域内新类型（与 bus.h 的
  `struct bus` 不同），需在头文件顶部先 `struct bus;` 前向声明；搬运要写 bus，用非 const 指针。
- **怎么验证**：`test_dma_regs` 4 项、`test_dma_copy` 字块拷贝 4 项 + 自动清使能 + 8 像素填色，全过。

## 7.4 — CPU 程序触发 DMA 填 VRAM

- 手写机器码程序：r0=DMA0 基址 → STR 写 SAD(颜色单元) → DAD(VRAM) → 一次 STR 32 位写
  CNT=`0x81000080`（SRC_FIX|ENABLE + 字数 128）触发 → 停机。测试断言 128 像素全红 + 自动清使能。
- **踩坑（ARM 立即数旋转再翻车）**：`ADD r1,r1,#0x1000` 最初编码 `0xE2811410`（imm8=0x10,
  rot4=4→ROR8）实际得到 `0x10000000`；正确是 `0xE2811A01`（imm8=0x01, rot4=10→ROR20，bit12=0x1000）。
  教训：构造立即数时用 `arm_rotate` 反推，或直接验证 `imm8 ROR (rot4*2) == 期望值`。
- 阶段 7 累计 90 项检查 0 失败（ctest 通过）。

## 8.5 — 中断按 CPU 分流（IME/IE/IF 两套）

- `io_t` 的 `irq_t irq` 改为 `irq_t irq[2]`（`[0]`=ARM9、`[1]`=ARM7）；`io_read8/io_write8` 增
  `is_arm7` 参数，中断区按 `irq[is_arm7]` 选实例（IME/IE/IF 同址 `0x04000208/10/14`，按访问者分流）。
- `io_set_vblank` 仍只置 ARM9 的 IF（VBlank 是 ARM9 显示事件）；`io_irq_pending` 检测 ARM9 视角（主循环用）。
- **为什么必须分流**：真机 ARM9/ARM7 各有独立中断控制器；不拆则两核读同一套 IF/IE/IME 会串扰，
  且 FIFO 中断 IF17/18 无法落到正确核。
- **怎么验证**：`test_irq_split`——ARM9 写 IME/IE，ARM7 读得独立清零值，ARM9 读回自己的值。

## 8.6 — IPC FIFO 完整（队列 + CNT 状态位 + IF17/18）

- 新建 `src/io/fifo.h/.c`：两条 16 字环形队列 `from9`（ARM9 写、ARM7 读）、`from7`（ARM7 写、ARM9 读），
  与各自 CNT 实例 `cnt9/cnt7`（16 位）。
- **寄存器布局（GBATEK）**：`IPCFIFOCNT=0x04000184`（bit0 发送空/bit1 发送满/bit2 送空 IRQ/bit3 清发送/
  bit8 收空/bit9 收满/bit10 收非空 IRQ/bit14 错误/bit15 使能）、`IPCFIFOSEND=0x04000188`（写 32 位）、
  `IPCFIFORECV=0x04100000`（读 32 位，IO 区间外）。
- 语义：未使能写 SEND 忽略；读空 RECV 返回 0 并置错误位；写满 SEND 置错误位；CNT 空满位实时算、
  其余位存 cnt 实例。
- **FIFO 中断（边沿触发）**：条件 `(CNT.2 & CNT.0)` 0→1 置 IF17（送空）、`(CNT.10 & !CNT.8)` 0→1 置
  IF18（收非空）。因一方发送 → 另一方接收非空，任何 FIFO 操作后对两核都做边沿检测（`io_fifo_update_irq_all`）。
- io 层新增 `io_recv32/io_send32` 供 bus 整字路由；`io_write8` 的 CNT 写、`io_recv32/io_send32` 后统一
  更新两核 IF。
- **怎么验证**：`test_fifo_basic`（跨核收发 + 空/非空状态位）、`test_fifo_irq`（IF17 送空、IF18 收非空）。
- 踩坑：`fifo.h` 参数列表首次出现 `struct irq` 需顶部先前向声明（同 7.2 的 `struct bus`）。

## 9.2 — 显示控制寄存器 disp 功能文件 + 调色板/OAM/VRAM 窗口

- 新建 `src/io/disp.h/.c`：`disp_t` 持主/副各一套 DISPCNT（32 位）、4 个 BGxCNT（16 位）、
  4 组滚动 HOFS/VOFS（16 位）；`disp_is_addr` 判断是否落在显示寄存器区间，`disp_read8/write8`
  按小端字节访问（`write_byte16` 对 16 位寄存器按字节写入）。
- io 层路由：`io_read8/io_write8` 在 `disp_is_addr` 命中时转 `disp_read8/disp_write8`。
- bus 侧配合新增区间（见 buslog 9.2）：调色板 RAM `0x05000000`、OAM `0x07000000`、
  VRAM 固定窗口（副 BG / 主 OBJ / 副 OBJ）。
- 验收：`test_disp_regs` 11 项全过（主副 DISPCNT/BGxCNT/滚动读写、调色板、VRAM 窗口映射）。

## 15.3 — DMA 补全：4 通道 + 地址控制 + repeat + 触发源

- `src/io/dma.h/.c` 从单通道 `dma_channel_t` 扩为 `dma_t`（`ch[4]`，`IO_DMA_COUNT=4`），
  SAD/DAD/CNT_L/CNT_H 按 `0x0C` 步进（DMA0 `0x040000B0`、DMA1 `0x040000BC`、DMA2 `0x040000C8`、DMA3 `0x040000D4`）。
- 新增控制位：源/目的「递增/递减/固定」三态（`DMA_CNT_SRC_DEC`/`DMA_CNT_DST_DEC`、`DMA_CNT_SRC_FIX`/`DMA_CNT_DST_FIX`）、
  `DMA_CNT_REPEAT`、`DMA_CNT_IRQ`（暂不接线）、`DMA_CNT_MODE_SHIFT` 触发源（立即/VBlank/HBlank/卡带）。
- `dma_fire(dma, bus, start_mode)` 遍历 4 通道，命中 `start_mode` 且使能者触发；`dma_transfer` 搬完非 repeat 才清使能。
- `io_set_vblank` 现在会 `dma_fire(VBlank)`；新增 `io_card_dma_check`（卡带就绪时 `dma_fire(Card)`）。
- **怎么验证**：`test_dma_channels`（多通道独立 + 源递减 + VBlank 触发）全过。

## 15.4 — 卡带总线接线（io/bus/nds + 卡带 IRQ）

- `io_t` 增加 `cartbus_t cartbus` 字段；`io_create` 里 `cartbus_init`，`io_attach_cart` 转发给 `cartbus_attach`。
- `io_read8/io_write8` 在 `cartbus_is_addr` 命中时转 `cartbus_read8/write8`；新增 `io_card_data_read32/write32`
  供 bus 的 `BUS_CARD_DATA`（`0x04100010`）整字路由。
- 卡带命令激活（ROMCTRL bit31 写 1）时：`irq_set_card` 置两核 IF bit19（`IO_IF_CARD_DONE`）+ `io_card_dma_check`
  触发卡带 DMA。`io_set_vblank` 增 `dma_fire(VBlank)`。
- `bus.c/h`：加 `BUS_CARD_DATA`，`bus_read32/write32` 命中即转发 `io_card_data_read32/write32`。
- `main.c`：装载 ROM 并解密安全区后 `io_attach_cart(nds->io, cart->data, cart->size)`，借指针给卡带总线。
- **怎么验证**：`test_card_dma`（卡带 DMA 8 字搬 RAM + DRQ + 使能自清 + IF bit19）全过。

## 16.2 — 存档芯片接线（io_attach_save / io_get_save）

- `cartbus` 阶段 15 的 AUXSPICNT/AUXSPIDATA 本阶段接上真 SPI 语义：写 AUXSPIDATA 低字节触发存档芯片的
  `save_transfer`（仅当 AUXSPICNT bit13+bit15 选中），bit6=0 传完自动撤片选复位命令（详见 cartlog 16.2）。
- `io_t` 已含 `cartbus`（内含 `save_t save`），故 io 层只补两个薄接口：`io_attach_save`（配置芯片类型）、
  `io_get_save`（拿芯片指针给 main 做 .sav 持久化）；`io_destroy` 在 `free(io)` 前 `cartbus_destroy` 释放存档缓冲。
- **怎么验证**：`test_save_spi_regs`（经 AUXSPICNT/AUXSPIDATA 总线读写存档）全过。

## 17.2 — 触摸屏 SPI（touch 功能文件 + io 接线）

- 新建 `src/io/touch.h/.c`：`touch_t` 持 `spicnt`（16 位）、`spidata`（8 位）+ TSC 命令状态机
  （`cmd/channel/mode8/result/reply_idx`）+ 触摸位置（`adc_x/adc_y/down`）。
- **寄存器**：`SPICNT=0x040001C0`（16 位：bit0-1 波特率、bit7 Busy 只读恒 0、bit8-9 设备选择=2 触摸、
  bit11 Hold 保持片选、bit15 使能）、`SPIDATA=0x040001C2`（8 位，写即启动一次传输）。
- **TSC 协议**：写 SPIDATA 的字节若 bit7=1 则是新命令——解码通道（bit6-4：X=5/Y=1/Z1=3/Z2=4/电池=2/麦克风=6/温度=0,7）
  与分辨率（bit3：0=12 位 1=8 位），算出 ADC 结果；随后按回复字节序号回传（12 位：`(result>>5)&0x7F`、
  `(result&0x1F)<<3`；8 位：取高 8 位 `result>>4` 后 `>>1`、`(bit0)<<7`），之后填充 0。
- **瞬时模型**：写即完成、Busy 恒 0；未保持片选（Hold=0）时传输后复位命令状态。
- 未按下按协议回 `X=000h`、`Y=FFFh`；电池通道在 NDS 接 GND 恒 0。
- `io_t` 增 `touch_t touch` 字段；`io_read8/io_write8` 在 `touch_is_addr`（0x040001C0..C3）命中时转
  `touch_read8/touch_write8`；新增外部信号 `io_set_touch(adc_x, adc_y, down)`（供窗口鼠标/触摸事件驱动）。
- `CMakeLists.txt` 的 `ndscore` 加入 `touch.c`。
- **怎么验证**：`test_touch_unit`（12/8 位回传 + 未按下 + Hold 复位）、`test_touch_spi_regs`（经总线读写坐标）、
  `test_touch_program`（CPU 程序读出 X/Y）全过。

## 18.2 — 音频寄存器路由（snd 接线）

- `io_t` 增 `snd_t snd` 字段；`io_read8/io_write8` 在 `snd_is_addr`（`0x04000400..0x04000505`）命中时
  转发到 `snd_read8/snd_write8`（音频寄存器虽在 IO 区，但语义/混音在独立模块 `src/snd/`，详见 sndlog）。
- `io.h` 引入 `snd/snd.h`；`CMakeLists.txt` 的 `ndscore` 加入 `snd.c`。
- **怎么验证**：`test_snd_regs`（经总线读写 SOUNDCNT/SOUNDBIAS + 通道寄存器）全过。

## 19.2 — 3D 几何寄存器路由（gx 接线）

- `io_t` 增 `gx_t gx` 字段；`io_create` 里 `gx_reset`（矩阵置单位阵 + 视口默认 (0,0,255,191) + GXSTAT 置 FIFO 空）。
- `io_read8/io_write8` 增 `gx_is_addr && !is_arm7` 路由（DISP3DCNT `0x04000060` + GXSTAT/RAM_COUNT `0x04000600..07`）；
  `snd_is_addr` 由「不区分 CPU」收紧为 `&& is_arm7`（音频由 ARM7 控制）。
- 新增 `io_gx_write32`（转发 `gx_write32`）：几何区 `0x04000400..0x040005FF` 是 32 位命令区，需整字转发。
- `bus.c` 的 `bus_write32` 增几何区 ARM9 整字转发（`addr∈[0x04000400,0x04000600) && !active_is_arm7` → `io_gx_write32`），
  避免拆字节破坏「命令字 + 参数字」的 40 位命令语义；ARM7 侧仍走音频（`bus_write32` 拆字节 → `snd_write8`）。
- **怎么验证**：`test_gx_fifo`（GXFIFO 命令流出图）+ 原 `test_snd_*` 切到 `active_is_arm7=1` 后仍全过。

## 20.1 — 仿射背景寄存器（disp 扩展）

- `disp_t` 增 `bg_pa[2][4]` / `bg_ref[2][2]`（主）+ `_sub` 镜像（副）；`disp.h` 定义
  `IO_BG_AFFINE_BASE=0x04000020`（主）/`IO_BG_AFFINE_SUB_BASE=0x04001020`（副），块内布局：
  BG2 的 PA/PB/PC/PD（4×16b）+ X/Y（2×32b），其后 BG3 同样 0x10 字节。
- `disp_affine_parse()` 把地址解析成 (engine, bg, 块内偏移)；`disp_read8/write8` 处理仿射区：
  PA-PD 16 位小端；X/Y 32 位小端，**bit28-31 无效**（写 byte3 只低 4 位有效，读回为 28 位符号值）。

## 20.2/20.3 — 混合/亮度/窗口/捕获寄存器（disp 扩展）

- `disp_t` 增 `blendcnt/blendalpha/blendy/master_bright/dispcapcnt/win0h..winout`（主）+ `_sub` 镜像；
  `disp.h` 定义 `IO_WIN0H..IO_WINOUT`（0x04000040..4A）、`IO_BLENDCNT/BLDALPHA/BLDY`（0x04000050..54）、
  `IO_DISPCAPCNT`（0x04000064）、`IO_MASTER_BRIGHT`（0x0400006C）及副引擎 `+0x1000` 偏移。
- 16 位寄存器经 `disp_reg16` 查找表（`offsetof` 映射）按字节读写；DISPCAPCNT 32 位单独处理。
- **踩坑修复**：`disp_reg16` 原按寄存器基址精确匹配，`bus_write16` 写高字节（奇地址）时匹配失败导致高字节丢失；
  改为 `addr & ~1u` 对齐基址后，低/高字节都能正确写入对应字段。
- **怎么验证**：`test_blend_regs`（主/副混合三件套 + 主亮度 + 窗口 + 捕获全读写回）全过。

## 2026-09-05 · 21-B2 — IPCSYNC 同步寄存器（0x04000180/81）

- **做了什么**：
  - `irq.h` 新增 `IO_IF_IPC_SYNC`（bit16）：真机里对端经 IPCSYNC.bit13 发来的请求对应
    IF bit16。
  - `fifo.h` 新增 IPCSYNC 常量与 `ipc_sync_t`（v[0]=ARM9、v[1]=ARM7，每核只存
    bit8-11 out + bit14 enable）；`fifo.c` 实现按访问者视角的读写：读时低 4 位 = 对端
    out（右移），高字节 = 本核 out/enable，bit13 只写不落盘；写时按 16 位小端字节合并，
    bit13 写 1 且对端 enable=1 时置对端 IF16（沿触发，与 FIFO 的 IRQ 使能门控一致）。
  - `io_t` 增 `sync` 字段；`io_read8/io_write8` 命中 0x04000180/81 时按访问者转发，
    请求目标取对端核的 irq（ARM9 请求 → ARM7 IF，ARM7 请求 → ARM9 IF）。
  - `tests/test_nds.c` 新增 `[case 21-B2]`：`test_ipcsync_regs`（双核 out 交叉读取、
    字节访问、低字节只读）+ `test_ipcsync_irq`（enable=0 不置 IF16、enable=1 置位、
    请求位不落盘、IF 写 1 清除、反向请求）。
- **怎么验证**：编译通过；`test_nds.exe` **544 项检查 0 失败**；真 ROM
  `--headless 20000000` 不再打印 `io: ... unknown addr=04000180/81`。
- **观察/遗留**：headless 终点仍与 B1 后相同（ARM7 PC=0626D1C1），说明真 ROM 对 IPCSYNC
  的访问是写 out 数据（0x0800）而非请求，未落在关键握手路径。用 `--trace` 定位到两个
  新卡点：① ARM9 栈在 0x027E3Fxx（0x02400000-0x027FFFFF 主存无缓存镜像未映射，写丢
  失），函数 `LDMFD sp!, {r4,pc}` 弹回 PC=0（约 cycle 126,634）；② ARM7 `BX r12 ->
  0x038043C9` 奇地址本应切 Thumb 却没切，随后把数据区当 ARM 码执行跑飞（约
  cycle 231,180）。两点分别排入 B3/B4。
- **结果**：✅ 用户验收通过（2026-09-05）。
