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

## 2026-09-05 · 21-B7 — EXMEMCNT / WRAMCNT 内存控制寄存器

- **背景/定位**：B6 后 headless 还剩 3 条 ARM9 未知 IO：半字读 `0x04000204/205`
  （EXMEMCNT）、字节写 `0x04000247=03`（WRAMCNT）。melonDS 直接启动语义：
  Shared WRAM 初始全给 ARM7（WRAMCNT=3），EXMEMCNT 初值 0xE880。
- **做了什么**：
  - 新建 `src/io/memctl.h/.c`（功能文件，日志仍归 iolog）：`memctl_t` 持
    `exmem[2]`（ARM9/ARM7 各一份 16 位值）与 `wramcnt`。
  - EXMEMCNT：16 位寄存器按字节访问（0x204 低字节 / 0x205 高字节）；ARM9 可写
    bit15/11/7-0，bit13/14 只读保留，写后把 bit7-14 镜像给 ARM7；ARM7 只能改自己
    低 7 位（bit0-6）。直接启动初值 `0xE880`。
  - WRAMCNT：ARM9 在 0x04000247 读写，ARM7 在 0x04000241 只读；值只取低 2 位
    （0=全 ARM9，1=ARM9 高半 + ARM7 低半，2=互换，3=全 ARM7）。
  - `io_t` 增 `memctl` 字段；`io_create` 调 `memctl_reset`；`io_read8/write8`
    路由 `memctl_is_addr`。
  - `CMakeLists.txt` 的 `ndscore` 加入 `src/io/memctl.c`。
- **怎么验证**：`test_wramcnt_regs`（初值、双核读写分流、ARM7 只读视图、EXMEMCNT
  高低字节 + 半字掩码）+ `test_wramcnt_split`（Shared WRAM 全量/半量四档切分）；
  `test_nds.exe` **587 项检查 0 失败**；真 ROM `--headless 3000000` 不再打印
  `io: ... unknown addr=04000204/205/247`。
- **观察/遗留（B8 素材）**：headless 终点仍为 ARM9 PC=0200B840、ARM7 PC=037FC0B0。
  ARM9 在 0x0200B834 轮询事件位（0x027FFC00 基址 + 偏移 0x38C 的 bit12），ARM7
  停在 0x037FC0A0/B0 轮询 0x027FFFF0——两核各自的“事件状态/命令口”语义待 B8 解码。

## 2026-09-05 · 21-B9h — VBlank bit0 + Timer0-Timer3 溢出置 IF

- `irq.h`：`IO_IF_VBLANK` 从 bit3 改回硬件 bit0（早期阶段把 bit3 当 VBlank 是简化，
  但真机 bit3=Timer0，FFXII 的 `ie=0x00042009` 同时含 bit0/bit3，必须按硬件位走）。
- `timer.h/.c`：`timer_advance` 返回 16 位溢出；`io_advance_timers` 在 TMxCNT_H
  bit6（IRQ 使能）时把溢出置到当前核 IF 的 bit3-bit6。
- FFXII 启动代码 `0x02008C70` 配置 TM0：CNT_H=0x00C1（1/64 分频 + IRQ 使能），
  headless 跑到约 419 万 ARM9 周期时溢出被正确置 IF，handler 能正常进入/恢复。

## 2026-09-05 · 21-B9i — IPCFIFOCNT 合并写修复（错误应答不吞使能位）

- FFXII 启动代码对两核各写 CNT：低字节 0x08（清发送队列）、高字节 0xC4。
  0xC4 = bit15 使能 + bit10 收非空 IRQ + bit14 错误应答，一次写完成“清错并配置”。
- `fifo_cnt_write` 旧实现见 bit14 就只清错误位、不再更新可写控制位，使 `cnt9/cnt7`
  恒为 0，FIFO 发送被 fifo_send 静默忽略。修复为：控制位每次写都更新，bit14 仅作
  应答清除。
- 真 ROM 验证：ARM9 两条 FIFO 命令（0x80004106/0x40402806）能入队，ARM7 IF18
  置位并触发 IRQ。

## 2026-09-05 · 21-B9j（前半）— 定时器重载值 + ARM7 也能收到 VBlank

- `timer.h/.c`：CNT_L 写入同时保存为 reload；CNT_H 使能沿（0→1）从 reload 起跳，
  溢出后回到 reload（此前写 CNT_H 一律清 0，FFXII 用“先 CNT_L 后 CNT_H”配置
  的溢出周期全部失效）。
- `io.c`：`io_set_vblank` 同时置 ARM9/ARM7 两套 IF——VBlank 是 LCD 信号，两个中断
  控制器都会收到；FFXII 的 ARM7 IE bit0 开着，ARM7 现在能持续收到帧事件。
- 新增 `[case 21-B9j]`：TM3 CNT_L=0xFFF0、CNT_H 使能+IRQ，15 步不溢出、第 16 步
  置 IF bit6 且回 reload；6.3 用例补 ARM7 IF 也收到 VBlank 的断言。
- 全量 **664 项检查 0 失败**。ARM9 忙位仍未清，B9j 后半继续追 service6 完成回执。

## 2026-09-06 · 21-B9j（后半）— SPI device1（固件 Flash）最小实现

- 定位：ARM7 service6（0x03804D7C）的事件处理器 0x03804A54 实际在操作 **SPI
  寄存器 0x040001C0/1C2**，且 SPICNT 高字节 0x89 的 device 位 = 1（固件 Flash），
  不是此前实现的触摸屏 device 2。
- `touch.c`：非触摸设备写 SPIDATA 时，若 device=1 则按“空固件”回读 0xFF，
  让 FFXII 的命令序列能走完；`touch.h` 增加 `SPICNT_DEVICE_FW`。
- 新增 `[case 21-B9j] SPI device1 最小回读`：使能+device1+READ 命令后 SPIDATA
  回 0xFF（1 项）。
- 全量 **665 项检查 0 失败**。真 ROM 效果：ARM9 首次真正离开 0x0200EA90 忙等，
  推进到 0x0200B838/0x0200F1BC 的后续服务循环；ARM7 也进入 0x027F6xxx 继续处理。

## 2026-09-06 · 21-B9j+ — SPI device1 固件完整状态机 + 合法用户区

- 诊断日志确认 FFXII 的真实 SPI 流：READ=0x03 后跟 3 字节大端地址；首字节地址
  本身就是 0x03（读 0x3FE00），旧状态机把它误判成新命令，导致镜像全部错位。
- `touch.h` 状态改为“片选保持 + 事务字节位”模型：`fw_cs/fw_cmd/fw_addr/
  fw_addr_n`；每次 SPIDATA 写按 HOLD 位决定事务是否续接，命令字节只在事务开头
  解析，READ 收满 3 字节地址后逐字节回读并自增地址。
- 模拟器没有 firmware.bin，按 DS 出厂语义合成最小固件：0x1D=主机型号、
  0x20/21=用户设置偏移 0x7FC0、0x2C/2D=Wi-Fi 长度、0x36-3B=MAC、
  0x3C/3D=频道；0x3FE00/0x3FF00 两个用户设置镜像带 version/语言/触摸校准/
  Update Counter，0x72 处 CRC-16/IBM 与内容一致，其余按擦除态 0xFF。
- 新增 `[case 21-B9j+]`：按 FFXII 真实序列验证镜像 0 首字节、0x20 偏移、
  RDSR、镜像 1 尾部 counter+CRC 等 11 项；全量 **675 项检查 0 失败**。
- 真 ROM：ARM9 从 0x0200EA88 继续推进，卡带数据装载循环可读完整块。

## 2026-09-06 · 21-B9k — ARM9 硬件除法/开方 + ROMCTRL 忙位

- 新增 `math.h/.c`（0x04000280-2BF，仅 ARM9）：DIVCNT/DIV_NUMER/DENOM/
  RESULT/REM（64 位）与 SQRTCNT/SQRT_RESULT/PARAM；写控制或操作数即触发计算，
  瞬时模型 busy 恒 0，除零/溢出按真机钳制，DIV0 标志看完整 64 位除数。
- `cartbus.c`：ROMCTRL bit31 是“块传输忙”——命令激活后置 1，块内 CARD_DATA
  读完最后一字才回落 0；FFXII 用 CPU 循环同时轮询 bit23(DRQ) 与 bit31。
- 新增 `[case 21-B9k]`（除法三模式/除零/溢出/开方）与 15.2 的 4 字节块忙位
  用例；全量 **700 项检查 0 失败**。真 ROM：卡带读块循环结束，ARM9 继续进入
  后续启动服务。

## 2026-09-06 · 21-B9n — POWCNT1/2、POSTFLG、WIFIWAITCNT 默认值

- 新增 `power.h/.c`：ARM9 POWCNT1（0x04000304）默认 0x820F（LCD/2D/3D/
  EngineB/换屏全开）、ARM7 POWCNT2 默认 0x0001（喇叭开）、两核 POSTFLG
  默认 1、ARM7 WIFIWAITCNT 默认 0x0030；写掩码与 bit0 粘住语义按真机。
- 直接启动语义对齐 melonDS：此前这些寄存器读 0，FFXII 的启动服务把 LCD/3D
  状态误判为“未上电”，提前进了空闲任务。
- 新增 `[case 21-B9n]`（默认值/掩码/粘位，12 项）；全量 **716 项检查 0 失败**。
- 真 ROM：ARM9 不再停在 0x0200957C 空闲死锁，继续跑 ARM7 启动服务与
  0x027FFF8C 信箱握手。

## 2026-09-06 · 21-B9o — 双核定时器拆成两套（真机共 8 个）

- 真机 NDS9/NDS7 各有 4 个定时器，IO 地址相同但寄存器实例独立；旧实现只有
  一套 `timer[4]`，两核配置互相覆盖，ARM7 声音/服务代码等自己的 Timer0
  溢出时可能等到的是 ARM9 的配置。
- `io_t` 的 timer 改为 `timer[2][4]`；io 读写按访问者选套，`io_advance_timers`
  增加 is_arm7 参数（cpu_step 每步推进当前核的定时器）。
- 21-B9j TM3 用例改查 ARM7 套 `timer[1][3]`；全量 **716 项检查 0 失败**。
- headless 复跑：ARM7 的 0x03807584 请求等待仍在（说明该卡点是更高层的
  ARM7 请求队列/信箱语义，不是定时器实例冲突）。

## 2026-09-06 · 21-B9p — DMA 完成中断接线

- `dma.h` 的 `DMA_CNT_IRQ` 注释此前写明“暂不接线”；真机 DMA 搬完且
  CNT bit14 置位时会产生 DMA0-3 完成中断（IF bit8-11）。
- `dma.c` 在每次实际传输（立即写 / VBlank / 卡带触发）结束后，按当前访问核把
  IF 对应位置 1；repeat 模式每次触发同样置位。
- 15.3 用例补 DMA0 完成中断断言；全量 **718 项检查 0 失败**。

## 2026-09-06 · 21-B9q — DMA 控制器按核拆成两套

- 与定时器同理，真机 ARM9/ARM7 各有 4 条 DMA 通道（同址、实例独立）；旧实现
  只有一套，ARM9 卡带/显示 DMA 会覆盖 ARM7 的搬运配置。
- `io->dma` 改为 `dma[2]`；io 读写按访问者选套，`io_card_dma_check` 带核参数，
  VBlank 触发对两套都点火（真机两核都收到帧事件）。
- 全量 **718 项检查 0 失败**；headless 复跑 ARM7 请求等待仍在。

## 2026-09-06 · 21-B9u — VCOUNT / DISPSTAT 只读帧状态

- FFXII ARM7 任务注册路径频繁读 0x04000006（VCOUNT）计算任务截止点；此前
  模拟器按未知 IO 读 0，所有任务截止点相同、就绪队列排序退化。
- `io_t` 增加 `vcount`，`io_set_vblank` 把行号复位到 0（随后由 B9v 的
  逐行推进驱动），0x04000006/07 只读返回低/高字节；DISPSTAT（主/副）bit0
  在 VBlank 后置位。验证 **729 项检查 0 失败**。

## 2026-09-06 · 21-B9v — 逐行 VCOUNT + VCount 匹配中断

- VCOUNT 改为逐行推进（runner 每 ~4000 步调 `io_advance_scanline`），一帧
  263 行；DISPSTAT 写低字节只接受 bit3-5 IRQ 使能、高字节为比较值。
- vcount 到达比较值时置 DISPSTAT bit2；若 bit5 使能，向两核 IF bit2 置位。
- 真 ROM：FFXII ARM7 IF bit2 handler 为 0x37FDDF0（周期任务调度器），补上后
  C0240046 系列回执开始按帧出现，ARM9 重新进入 0x020119xx 卡带读循环。
  验证 **731 项检查 0 失败**。

## 2026-09-06 · 21-B9za — IF bit21（GX FIFO）接线
- irq.h 增加 `IO_IF_GXFIFO`（bit21）；GXSTAT bits30-31 写模式后，io 层在
  GX 写操作后按“FIFO 空”同步置/清 IF21（参考 ARM9 IF9 的 0x200000）。
## 2026-09-06 · 21-B9zc — io_advance_cart（卡带就绪时钟接入 cpu_step）

- ARM9 每个指令周期推进 `cartbus_advance`；就绪边沿复用现有卡带 IRQ/DMA 接线。
- 原“写 ROMCTRL 立即可用”的边沿判断保留在写入路径，真正就绪由时钟推进触发。

## 2026-09-06 · 21-B9wd — VRAMCNT 读写接线

- 0x04000240-246/248-249 在 ARM9 视角读回 vramcnt、写时调 `bus_set_vramcnt`；
  ARM7 视角保留 0x04000241=WRAMCNT 的旧语义，避免和 VRAMCNT B 冲突。

## 2026-09-06 · 21-B9we — H=0x80(LCDC)/0x82(BBG 扩展调色板) 切换

- VRAMCNT H 写入序列为 82→80→82：0x80 期间 CPU 经 0x0689C000 写扩展调色板，
  0x82 后渲染器从 bank H 槽 2 读取同一份数据。io 层只透传寄存器，映射由 bus 完成。

## 2026-09-06 · 21-B9wo — 定时器按经过周期推进（事件等待期补偿）

- **证据**：参考核在开场字幕阶段（frame600-1400）TM0/TM1 的 CNT_H=0x00C1
  （使能+IRQ+1:64）且计数器持续增长；本地旧模型 `timer_advance()` 每次调用
  只给累加器 +1，而调用点固定在 `cpu_step`（一条指令一次），等待/跳帧期间
  定时器完全不涨，字幕/过场节奏明显偏慢。
- **修复**：
  - `timer_advance(t, cycles)`：累加器一次加 `cycles`，while 循环补足分频
    计数（支持一次跳过多个周期）；`io_advance_timers(io, core, cycles)` 同步。
  - `cpu_step` 传 `step_cycles`；事件驱动 runner 在“单核等待/双核等待导致
    时间跳跃”时，给等待核补 `io_advance_timers(..., delta)`（delta 为
    tm.now 的系统周期增量）。
- **效果与验证**：全量 **818 项检查 0 失败**（TM0 1:1、TM1 1:64、TM3 溢出
  用例语义不变）。真 ROM 同一步数 13 亿步时，字幕页从修正前 frame≈1475
  提前到 frame≈1380（约 5-6%），ARM7 仍稳定停 0x1158；字幕仍比参考慢，
  剩余差异属于尚未完成的周期成本模型（分支/访存多周期开销）。

## 2026-09-12 · 21-B9wr — KEYCNT 按键中断 + NDS7 卡带 DMA 触发模式

- **KEYCNT（0x04000132）**：两核各一份（`io->keycnt[2]`）。bit0-9 = 键掩码，
  bit14 = IRQ 使能，bit15 = AND/OR 选择。`io_set_keyinput` 在输入变化时按
  melonDS `CheckKeyIRQ` 口径做边沿检测（“上次不匹配→现在匹配”置 IF bit12）；
  AND 模式下掩码内全部键都按下才算匹配，OR 模式下任一键按下即匹配。
  FFXII 在标题/菜单之外还用它做按键唤醒，不实现会让关键等待卡住。
- **NDS7 卡带 DMA 模式**：ARM9 的 DS 卡带触发号是 5（CNT 高半字 bit11-13），
  NDS7 的定义不同——`CheckDMAs(1,0x12)` 判定 `((cnt>>12)&3)|0x10 == 0x12`，
  即 bit13-12=2。新增 `dma_fire_card(dma,bus,is_arm7)` 按核选择匹配口径，
  `io_card_dma_check` 改调它；旧实现只按 ARM9 的 5 匹配，ARM7 的卡带 DMA
  永不触发（FFXII ARM7 实测 CNT_H=AF00 → bit13-12=2，正好是 0x12）。
- 测试：`[case 21-B9wr]` 8 项（模式字段、未激活不搬、4 字内容、搬完清使能、
  ARM9 通道不被误触发）；KEYCNT 4 项（OR 模式、AND 模式部分/全部按下）。

## 2026-09-12 · 21-B9ws — HALTCNT（0x04000301，ARM7）

- `power_t` 增加 `halt_req`：ARM7 写 HALTCNT（bit7=1 进暂停；0xC0=Sleep，
  0x40=GBA 模式本地按暂停处理）时记录请求；`cpu_step` 在 ARM7 取指前消费：
  仍满足 `(IF & IE) != 0` 就保持暂停（`step_cycles=0`，事件驱动里让出时间片），
  有中断待处理就清请求继续执行——与 melonDS `HaltInterrupted(1)` 同口径
  （ARM7 唤醒不看 IME）。
- BIOS 的 SWI 6 Halt / SWI 7 Stop / CustomHaltPost 与游戏直接写 HALTCNT
  走同一条路径（`power_halt_request`）。

## 2026-09-12 · 21-B9wu — DMA 搬运期间按属主核分流 IO

- **证据**：FFXII 进 3D 段后 GX 只收到寄存器设置类命令、一条顶点命令都没有；
  追踪发现 ARM9 写 `0x04000400` 被总线当成「ARM7 视角」（=音频通道寄存器）。
  `dma_transfer()` 走 `bus_read/write`，但搬运期间没有设置 `active_is_arm7`，
  沿用了上一步 CPU 留下的值——ARM9 的显示列表 DMA 就这样被写进音频寄存器丢掉。
- **修复**：`dma_transfer/dma_write8/dma_fire` 增加 `is_arm7` 参数，搬运前后
  切换并恢复 `bus->active_is_arm7`；DMA 完成中断也按属主核写 IF。
- 新增 `[case 21-B9wu]`：故意把 `active_is_arm7` 污染成 ARM7 再触发 ARM9 的
  VBlank DMA，断言命令仍进 GX（`cmd_count` +2、`mt_mode=2`）且标志原样恢复。

## 2026-09-12 · 21-B9wv — KEYINPUT 复位值（默认“全部松开”）

- **证据**：`--dump` 的两核 IO 快照对照发现 `0x04000130` 本地读 0x00000000、
  参考读 0x000003FF——本地 `keypad_t` 由 calloc 得到 0，被游戏读成「所有键都按住」
  （START/A 一直按着）。修好后真 ROM 时间线整体后移约 200 帧、ARM9 在
  frame1800-2000 落到与参考一致的卡带读循环 `0x020119xx`。
- **修复**：新增 `key_reset()`（= `key_set_pressed(k, 0)`，低 12 位全 1、高 4 位全 1），
  `io_create()` 里调用；单测 `[case 6.7]` 增加“复位后默认全松开”。

## 2026-09-12 · 21-B9ww/21-B9wx — DMA 模式 7（GX FIFO）+ NDS RTC

- **DMA 模式 7 = GX 命令 FIFO**（`dma.h` 的 `DMA_START_GXFIFO`）：melonDS 在
  `StartMode==0x07` 时走 `GPU3D.CheckFIFODMA()`——真机的显示列表就是「源在主存、
  目的固定 0x04000400」的 DMA。本地旧实现只认立即模式，模式 7 永不触发，
  GX 收不到几何命令。现在与立即模式一样同步整体搬运（命令内容等价）。
  单测 `[case 21-B9ww]` 3 项。
- **RTC（0x04000134 RCnt / 0x04000138 串行数据）**：新增 `src/io/rtc.{h,c}`，
  按真机 3 线协议实现（bit0 数据 / bit1 时钟 / bit2 片选 / bit4 方向；片选拉高
  开始一次传输，时钟下降沿采样或移出；首字节是命令，0x60 高半字节按位反转表
  解码；支持状态寄存器 1/2、日期时间 7 字节、闹钟、时钟微调、自由寄存器）。
  走时由 runner 按 `tm.now / 33513982` 每秒推进。复位为 2026-09-12 12:00:00
  （24 小时制）。单测 `[case 21-B9wx]` 6 项（状态寄存器写读、日期 BCD、RCnt）。
  诊断：`io: rtc-reads=N` 显示游戏对该区的读取次数（本步前 2300 帧 112,610 次，
  说明它在轮询 RTC；实现后事务能正常结束）。

## 2026-09-12 · 21-B9xa — IME 只保留 bit0 + 声音驱动诊断

- **IME（0x04000208）**：只有 bit0 有效（melonDS `IME = val & 1`）。旧实现按字节
  写入整字，读回会出现 `0x04000001` 这种高位垃圾，跨核对 dump 时误报差异；
  现在只保留 bit0（功能不变，dump 与参考一致）。
- **声音驱动诊断**：`io: snd-cnt-writes=N` 统计 ARM7 写 SOUNDxCNT 最高字节
  的次数（含 start 位）。FFXII 开局后该计数持续增长说明 ARM7 声音驱动在跑；
  排查「ARM7 卡在 0x037FEFD0 轮询 SOUNDxCNT bit31」时用它判断通道是否被重触发。

## 2026-09-12 · 21-B9xc — VBlank 时序对齐真机（第 192 行起）

- **证据**：本地 IF9/IF7 在帧边界总带着 VBlank 位（`00280005`/`00080001`），
  参考核同一阶段是 `00280000`/`00000000` —— 说明参考的 VBlank 在帧内就被
  处理掉了。查 runner：`runner_ev_frame`（帧边界）调用 `io_set_vblank`，
  而真机一帧 263 行、**第 192-262 行才是 VBlank**。
- **修复**：`io_set_vblank` 不再把 VCOUNT 归零，改由新增的
  `io_frame_boundary()`（帧边界）负责归零并清 DISPSTAT bit0；
  `runner_ev_line` 在 `vcount == 192` 时触发 VBlank IRQ 与 VBlank DMA。
  事件驱动与旧的 `--headless` 步进路径都按这个模型。
- **效果**：IF 模式与参考一致（VBlank 帧内被消费），而且**标题画面仍然
  在 frame1900 与已验收截图逐像素 0 差异**（本轮重新截图比对）。
- 单测 `[case 21-B9xc]` 6 项（IRQ 两核、DISPSTAT 主副、帧边界归零与清标志）。

## 2026-09-12 · 21-B9xi — ARM7 BIOS 保护值（0x04000308）与 bring-up 诊断

- **证据**：参考核 `ARM7IORead16(0x04000308)` 返回 `ARM7BIOSProt`，直启为
  `0x1204`；本地此前的临时补丁写在 `io_read8` 里，且 `0x04000308` 落在
  `IO_POWER_END` 之外（不归 power 模块管），NDS9 侧不该看到它。
- **修复**：`power.h` 新增 `IO_POWER_BIOSPROT=0x04000308` /
  `IO_POWER_BIOSPROT_DIRECTBOOT=0x1204` 与 `power_t.biosprot`；
  `power_read8/write8` 只对 ARM7 生效（真 BIOS 启动时值为 0 才允许写入，
  melonDS 同口径），`io_read8/write8` 的 power 分支只放 ARM7 过 `0x308/309`。
- **新增诊断**（都用于本次 bring-up 对照）：
  `io: rtc-reads / snd-cnt-writes / snd-bias-writes`、
  `ipc: sends9/sends7/c187` + 最近 32 条发送的环形明细与 `ipc9:`（ARM9 最近 16 条）、
  runner 里的 `disp-change`（DISPCNT/子屏 DISPCNT 每次变化）与
  `timer-change`（TM1CNT/IE9 控制字变化）。
- **这一轮诊断得到的结论**（详见 `docs/21-rom-bringup.md` 的 21-B9xi 节）：
  直启表 `0x027FFC30` 的值来自 ARM9 把运行时签名表 `0x0207995E` 拷进去
  （参考 `0xFFFF`、本地 `0x0000`）；IPCW 报文里参考核每帧 5 条
  （4 条 `C02400x6` 心跳 + 1 条 `0000C187`），本地只有 4 条、`c187=0`。

## 2026-09-12 · 21-B9xj — ARM7 独立 DISPSTAT（0x04000004 按核分流）

- **证据**：新加的 `--watch 04000004-04000006` 显示 **ARM7 每帧几十次写
  `0x04000004`**（PC=037FE0B0/037FE0BC/037FDE14，值如 `1C0D/5E0C/A00C/E12D`，
  是它在按扫描线分段做时序控制）。本地两核共用一份 DISPSTAT，于是这些值
  落到了 ARM9 的寄存器上：本地 io9 快照 `0x04000004=00001C28`（VCount=0x1C、
  VCount IRQ 使能 bit5），参考核是 `0000000A`；对应地本地 `IF9` 常驻
  bit2（VCount 匹配中断）而参考核没有。
- **修复**（对齐 melonDS `GPU::DispStat[0]/[1]` 与 `SetDispStat/SetDispStatIRQ`）：
  `disp_t` 增 `dispstat7`；`disp_read8/disp_write8` 增 `is_arm7` 参数，
  `0x04000004/05` 按访问者分流；可写位改为「全部 − 只读位 0x0047」，
  即 bit3/4/5/7-15（bit7 是 VCount 比较值的 bit8）；
  `io_advance_scanline` 按三份寄存器（ARM9 / ARM7 / 副引擎）各自比较 VCount，
  匹配改成 melonDS 的**边沿触发**语义（标志已置位不再触发），中断挂给该核。
- **效果**：本地 `IF9` 的常驻 bit2 消失；`0x04000004` 不再被 ARM7 污染
  （帧 2200 为 `0000000C`，参考核 `0000000A`）。

## 2026-09-12 · 21-B9yi — 卡带传输结束中断 + 预取 FIFO 接线

- `io_advance_cart` 每 ARM9 指令推进卡带时钟：`cartbus_advance` 取到新字时
  置两核卡带 IRQ（既有的 DRQ 边沿）**并**触发卡带 DMA 检查；
- 新增「传输结束」路径：`cartbus_end` 在 `AUXSPICNT` bit14（传输完成中断使能）
  置位时把 `cartbus.end_irq` 立起，由 `io_advance_cart` 转成两核的卡带 IRQ
  （对应 melonDS `ROMEndTransfer` 里的 `SetIRQ(Num, TransferIRQ)`）；
- 说明：卡带寄存器本体在 `src/cart/cartbus.c`，本模块只负责「按周期推进 +
  把就绪/结束事件接到中断与 DMA 触发线上」。

## 2026-09-12 · 21-B9yi — 卡带中断语义修正（去掉「就绪边沿挂中断」）

- **问题（实测）**：`io_advance_cart` 与 `io_write8` 的卡带分支此前在**数据就绪
  （DRQ）边沿**就挂两核 IF bit19；melonDS 的 `RaiseDRQ()` 只置 ROMCnt bit23 并
  `CheckDMA()`，卡带中断只在**传输结束**（`ROMEndTransfer`，且 AUXSPICNT bit14
  使能）时挂出。后果：ARM9 每取一个字就被中断一次（实测空闲期每帧 6 次
  `0x02009580` 处的 IRQ、单帧内间隔仅 ~170 周期），游戏任务调度相位被打乱，
  最终在「等 ROMCTRL 空闲」轮询里死锁（两核停在 IRQ 模式、IPC FIFO 积压 16 条）。
- **修正**：两条路径都只保留 `io_card_dma_check`（DMA 触发），中断改由
  `cartbus_end → end_irq`（传输结束 + AUXSPICNT bit14）统一挂出。
- **效果**：按键离开标题后不再死锁；ARM9 在第 1900 帧后与参考核一样进入空闲任务
  （`cpsr=0000001F`、`if9=0x00200000`、IPC `fifo=0/0`）。
- 单测改写：`card no irq on DRQ`（DRQ 不挂中断）+ `card irq on transfer end`
  （传输结束挂中断）；全量 **930 项检查 0 失败**。

**21-B9yi（续）— 卡带 DMA 两个真 bug**（读来源统计 `src=cpu|dma` 后锁定）：

1. **重复模式地址不前进**：`dma_transfer` 只用局部 `src/dst` 不写回通道；
   卡带 DMA 用 `AF000001`（模式 5 + 重复位 + 源固定/目的递增）逐字搬 512 字节时
   每个字都写到同一个 dword。修复：搬完写回 `dma->sad/dad`。
2. **DRQ 未置位也触发卡带 DMA**：melonDS 的 `CheckDMA()` 第一行即
   `if (!(ROMCnt & (1<<23))) return;`；本地无条件触发 ⇒ 把 FIFO 残留值
   （`0xFFFFFFFF`）搬进游戏缓冲。修复：`dma_fire_card()` 先查 DRQ。

验证：**930 项 0 失败**；900 帧里程碑标记与参考核一致；2000 帧无回归。

### 21-B9yi（续12）— 卡带 DMA 改成参考核语义（去瞬间推进 + 电平敏感重检 + 中断只挂 ARM9）

**动机**：续11 的逐事件对照证明「两侧是同一段代码、同一组寄存器值」，差别只在
推进方式；再往下必须把本地卡带 DMA 的**触发模型**换成 melonDS 的模型，而不是
继续在旧模型上加阈值。

**改动**（`src/io/dma.c`、`src/io/io.c`）：

1. **删掉 21-B9zb 的「瞬间推进卡带时钟」hack**：`dma_transfer()` 每次从
   `CARD_DATA` 取数前都 `cartbus_advance(cb, 100000)`，等于把卡带取数等待一次跳过
   ⇒ 同一笔传输在本地只用参考核 1/4 的帧数跑完（实测 **205 块/帧 vs 参考 51**）。
   参考核的卡带 DMA 不推进卡带时钟：数据由卡带侧按 `xfercycle*(4+gap)` 取进 2 字
   FIFO 并 `RaiseDRQ()`，DMA 只是把已就绪的字搬走。数据是否就绪由 DRQ 门控保证。
2. **`dma_fire_card()` 改为电平敏感的「搬完再检查」**：对齐 melonDS `DMA::Run9()`
   收尾的 `if (StartMode == 0x05) NDSCartSlots[0]->CheckDMA();`，而 `CheckDMA()`
   第一行就是「DRQ 未置位直接返回」⇒ FIFO 里还有字就立刻再搬一个。旧实现只在
   「卡带又取到一个字」的边沿触发一次，FIFO 装满 2 字后卡带 `data_late` 停止取数、
   DMA 也不再被拉起 ⇒ 最后 1 个字没人读、传输永远不结束。循环有界（≤8 轮，
   每轮至少搬 1 字、FIFO 最多 2 字）。
3. **卡带完成中断只挂 ARM9**：melonDS 是 `SetIRQ(Num, IRQ_CartXferDone)`，`Num` =
   卡带槽所属核（NDS 只有槽 0 = ARM9）。旧实现两核都挂 ⇒ 本地 ARM7 的 IF 永远
   留着 bit19（实测 1880-1912 帧 `if7=00080000`，参考为 0），污染 ARM7 的
   Halt 唤醒条件（`IF & IE`）。修正后 `if7=00000000`、`ie7=0104009D` 与参考**逐位相同**。

**诊断设施**（本轮新增/增强）：

| 开关 | 作用 |
|---|---|
| `NDS_CARTIO=LO-HI` | 卡带寄存器区**每次写**（含命令字节 0xA8-0xAF、AUXSPICNT 0xA1、ROMCTRL 0xA4-0xA7）+ PC |
| `NDS_DMAFBG=LO-HI` | 每个卡带 DMA 触发点：DRQ 值 + ch2/ch3 的 CNT，用来区分「没触发」/「触发了但通道没武装」 |
| `NDS_IRQLOG=LO-HI`、`NDS_MODELOG=LO-HI`、`NDS_DMALOG=LO-HI` | 帧区间化（此前只能从 0 打印，跑到 1900 帧早已刷满上限） |
| runner `NDS_FTRACE` | 每帧行增加 `ie9/ie7`，卡带行增加 FIFO（`data_count`/`data_late`） |

**实测**：卡带块速 205 → **52 块/帧**（参考 51）；DAD 推进逐帧对照（如 f=1903
本地 `020E2A44` vs 参考 `020E2840`）；第 1886-1903 帧**前 931 条命令逐条相同**；
900 帧里程碑三标记与参考一致；帧 100 画面 81.9% 不回归；**931 项单测 0 失败**。

**未解决（本步定位到机理）**：带按键时序时本地第 1904 帧起仍停摆——处理器在
「该块还剩 2 字」时就停了 DMA（`rem=504/pos=8/fifo=2/late=1`），而参考核是
「整块搬完 → 才停 DMA」（时间戳实证见 `docs/21-rom-bringup.md` 续12 第 5 节）。

### 21-B9yi（续15）— `io_advance_cart` 用满整个周期预算（加载阶段追平参考核）

**问题**：`cartbus_advance()` 每次调用最多取一个字（约 20 个卡带周期）就返回；
`io_advance_cart()` 每次只调它一次 ⇒ **剩余周期被丢掉**。ARM9 正常执行时每次只传
1-5 个周期，察觉不到；空闲期（ARM9 在 WFI，runner 按扫描线事件一次传 `delta≈2130`）
每帧只能取 ~290 字，而参考核同段 6400 字/帧 ⇒ 「等最后一块搬完」白等 ~330 帧，
表现为加载画面长时间不动。

**修正**：`io_advance_cart()` 改成循环：

```
while (剩余周期 > 0 && guard++ < 4096) {
    取一个字（cartbus_advance）
    触发该字的 DRQ 卡带 DMA（io_card_dma_check 0/1）——把 FIFO 读走、腾出位置
    剩余周期 -= 本次取数实际消耗
}
```

没有待取事件（`wait_phase != 1`，即 FIFO 满且无人读、或块已取完）时立刻退出；
净行为等价于 melonDS「卡带取数挂系统时钟事件 + 每字一次 `CheckDMA`」。

**实测**：空闲期卡带取数 290 → **1456~3686 字/帧**；f=2000 本地已进入同步读卡带
循环（`02011938`）且 DISPCNT 与参考核同步；**f=1900-2110 与参考核逐帧 100% 逐像素
相同（MAD 0.00）**；931 项单测保持 0 失败。

### 21-B9yi（续18）— 补上 HBlank 中断（IF bit1）

**证据**：两侧 ARM9 IRQ 采样（本地 `NDS_IRQLOG`、参考核 `REF_IRQLOG`，f=2090-2110）：

```
参考核 268-340 IRQ/帧；IF 位：bit1(HBlank) 3516、bit19 3741、bit21 3586、bit11 169
本地     5-6 IRQ/帧；IF 位：bit0/3/4/18/19/21 —— 没有 bit1
```

本地此前**完全不产生 HBlank 中断**（DISPSTAT bit4 使能了也没人挂 IF bit1）。
FFXII 用 HBlank 做每行流式加载，缺了它本地每帧少做一大批工作。

**实现**：`io_advance_scanline()` 里，每条扫描线（含 VBlank 期的 192-262 行，共 263 次/帧）
在 `DISPSTAT bit4` 置位时给对应核挂 `IF bit1`：主引擎 `dispstat`→ARM9、
`dispstat7`→ARM7、副引擎 `dispstat_sub`→ARM9（与既有 VCount 匹配中断同一套分流）。

**实测**：本地 f=2015-2025 变成 **268 IRQ/帧**（与参考核完全同型）；
f=2000-2110 画面仍逐帧 100% 相同；900 帧三标记一致；帧 100 画面 81.9%；931 项 0 失败。

**留下的线索**：任务现场保存次数（`--watch 02077000-02077010` vs `REF_WATCH`）两侧从第 1 帧
起就差约 20%（本地更少：f=2116 本地 87592 / 参考 116395），说明**调度节拍**本身有系统性差异，
与「本地每帧流式工作更少」的方向一致 ⇒ 下一步核对驱动节拍的定时器（TM1/扫描线）。
