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

## 修复：十字 demo 两缺陷（随 5 阶段验收发现）

窗口 demo 检查出十字不完整/粗细不一，根因在 `drawprog`（`main.c` 与
`test_cpu_draw_vram` 共用同一套程序）：

1. **竖线缺下半屏**：终点 `r4 = 起点 + 0xC000` 只到 y=95；改 `+ 0x18000` 到末行。
2. **横竖粗细不一**：横线 1 行 vs 竖线 2 列；横线改双行嵌套循环（y=95、y=96）。

`test_cpu_draw_vram` 新增断言覆盖：`(128,191)`、`(0,95)`、`(0,96)`、`(128,96)`、
`(128,97)`、`(255,95)`、十字外 `(10,10)/(10,190)`，全部通过（48 项检查 0 失败）。

## 5.4 用例：死循环保活

- 程序：`B self`（`0xEAFFFFFE`）。跑 10 万步断言不崩、PC 稳定停在原地、cycles=100000。

## 5.5 README：如何跑测试

- `README.md` 新增「运行测试」小节：配置/构建/`ctest`/直接运行 `test_nds.exe`、退出码含义、新增用例方法。

## 5.6 可选文档：未来 devkitARM（仅文档）

- 新增 `docs/06-devkitarm.md`：devkitARM 是什么、当前为何不需要、未来接入清单、
  与真机地址/阶段 9 的关系。只写文档不实现。

## 6.2 — 用例：中断寄存器 IME / IE / IF

- 写读回：IME/IE 32 位写读一致；`io_set_vblank` 后 IF bit3=1。
- **IF 写 1 清除**：写 `0xFFFFFFFF` 全清、只写 `0x8` 只清 VBlank 位（写 0 的位不受影响）。

## 6.3 / 6.4 — 用例：VBlank 标志 + 最小 IRQ 检测

- `IF == 0x0008` 即 VBlank 已置位。
- pending 四态：只挂起→0；+使能→0；+总开关→1；程序清 IF→0。验证「IF&IE&IME 缺一不可」。

## 6.5 — 用例：定时器 0-3

- `run_program` 型驱动（B self 死循环）跑 640 步：TM0(1:1)=640、TM1(1:64)=10、TM2(禁止)=0。
- 修 `timer.c` 字节偏移 bug（`% IO_TIMER_STRIDE`，见 iolog）。

## 6.6 — 用例：KEYINPUT

- 未按键读 `0xFFFF`（高 4 位恒 1）；按 A 读 `0xFFFE`；按 UP+B 读 `0xFFBD`（按下=0）。

## 6.7 — 用例：等 VBlank 轮询 + 读键程序

- 等 VBlank 程序两轮跑：不置 VBlank 停在轮询循环（PC 在 0x0C-0x18、不写屏）；置 VBlank 后走出循环、写黄屏停机（PC=0x28、VRAM[0]=0xFF00）。
- 读键程序：按 A 写蓝 `0x001F`、未按写红 `0x7C00`。
- 修 3 个手写汇编编码错误（ADD 立即数旋转、STR 寄存器位序、B offset），排查靠 `exec_set_trace(1)` 逐步打印（详见 iolog）。

## 7.2/7.3 — 用例：DMA0 寄存器 + 立即模式

- 寄存器写读回：SAD/DAD/CNT_L/CNT_H 4 项。
- 立即模式：
  - 字块拷贝：Main RAM 4 个字 → VRAM，逐字断言 + 自动清使能（CNT_H 读回只剩 32BIT 位）。
  - 半字填色：SRC_FIX 固定源，8 个像素全 `0x7C00`。

## 7.4 — 用例：CPU 程序触发 DMA 填 VRAM

- 机器码程序：配 SAD(颜色单元)/DAD(VRAM)/CNT(`0x81000080`=SRC_FIX|ENABLE+128) 触发，停机。
- 断言 128 像素全红 + 自动清使能。
- 修 `ADD r1,r1,#0x1000` 编码：`0xE2811410`（imm8=0x10,ROR8）实得 `0x10000000`，
  正确 `0xE2811A01`（imm8=0x01,ROR20→0x1000）。教训：立即数要用 `arm_rotate` 反推验证。

## 8.2/8.4 — 用例：双核 step + 交错调度

- 新增 `run_cpu7` 驱动（与 `run_program` 对称，操作 `nds->cpu7`）。
- `test_dual_core_step`：两核各跑 NOP，step 后 PC 均 +4、`is_arm7` 正确。
  （初版用 `B self` 断言 PC 前进，实际 B self 死循环 PC 原地打转，改 NOP。）
- `test_interleave`：`i%3==2` 跑 ARM7 的交错 6 步，ARM9 cycles=4、ARM7 cycles=2。

## 8.3 — 用例：ARM7 WRAM + 入口取指

- `test_arm7_wram`：`0x03800000` 写读回、越界（`0x03900000`）读 0。
- `test_arm7_fetch`：WRAM 写 `MOV r0,#5`，`cpu_reset(cpu7, 0x03800000)` 后 step，r0=5、PC+4。

## 8.5 — 用例：中断按 CPU 分流

- `test_irq_split`：手动切 `bus->active_is_arm7`，ARM9 写 IME/IE 后 ARM7 读得独立清零值、
  ARM9 读回自己的值，验证 `irq[2]` 分流。

## 8.6 — 用例：IPC FIFO 收发/状态位/中断

- `test_fifo_basic`：使能两核 FIFO 后，ARM9 发 `0xDEADBEEF` → ARM7 收得原值（反向 `0x11223344` 同理）；
  空队列 send/recv empty 位、发送后 send 非空、对方 recv 非空。
- `test_fifo_irq`：使能 send-empty IRQ → IF17 置位；使能 recv-not-empty IRQ 后由 ARM7 发一字 →
  ARM9 的 IF18 置位（边沿触发）。

## 8.7 — 用例：双核 FIFO 传值 + 底屏体现

- ARM7 程序（WRAM）：发 `0x7C00`（红）到 SEND；ARM9 程序（Main RAM）：读 RECV 后写底屏 VRAM。
- 两程序都用「数据区预写地址 + LDR 加载」避免复杂立即数（SEND/RECV 地址不易用 MOV 立即数表示）。
- 先跑 ARM7 发送、再跑 ARM9 接收写屏；断言两核停机 PC 与底屏首像素 `0x7C00`。

## 全量验证

`ctest --test-dir build` 与直接运行均通过。
阶段 6 收尾时 70 项检查 0 失败；阶段 7 收尾时 90 项检查 0 失败；阶段 8 收尾时 **117 项检查 0 失败**；阶段 9 收尾时 **148 项检查 0 失败**。

## 9.2 — 用例：显示控制寄存器 + 调色板 RAM

- `test_disp_regs`：主/副 DISPCNT（32 位）、BGxCNT（16 位）、滚动 HOFS/VOFS 写读回；
  调色板 RAM 主 `0x05000000`/副 `0x05000400`；副 BG VRAM 窗口 `0x06200000` ↔ 物理 `vram[0x40000]`。

## 9.3 — 用例：直色位图渲染

- `test_bitmap_render`：mode 5 + BG2 直色位图，写红/蓝两像素，断言 RGB888 输出与未写区黑。

## 9.4 — 用例：Mode 0 tile 图层

- `test_tile_render`：16 色 tile 像素(0,0)=索引1 → 红，其余透明露绿背景；切 256 色同理。

## 9.5 — 用例：副引擎（Engine B）对称渲染

- `test_engine_b`：副引擎直色位图（`0x06200000`）+ tile 渲染各验一次（副调色板 `0x05000400`）。
- **踩坑（测试）**：前一步位图数据残留在 `0x06200000` 会污染 tile 位面，tile 用例前需显式清
  位面 1-3，只留位面 0 的 `0x80`。

## 9.6 — 用例：OBJ 最小（一个 sprite）

- `test_obj_render`：OAM[0] 配 8×8 sprite，16 色在 (20,10) 显示红、透明/屏外露背景；切 256 色显示绿。
- **踩坑（测试）**：OAM 默认全 0 = 原点可见 sprite，需先对 128 条 OAM 统一置 bit9 禁用再配 entry 0。

## 9.7 — 用例：自造数据 2D 场景验收

- `test_2d_scene`：顶屏 8bpp tile+tilemap+调色板 拼「红 tile0 + 绿 tile1 + 透明底」+ OBJ 蓝方块；
  底屏副引擎 8bpp tile 纯青。整条链路只经 DISPCNT/BGxCNT/OAM 寄存器，无线性 FB 写入。

## 10.2 — 用例：移位操作数

- `test_shifts`：LSL/LSR/ASR/ROR 立即与寄存器移位、ROR#0=RRX、移位进位更新，逐类断言。
- **踩坑（测试）**：`ADD r10,r1,r2,LSL r9` 编码 Rs（bit11-8）误写 0，改 `0xE081A912`。

## 10.3 — 用例：更多数据处理

- `test_more_dataop`：MVN/BIC/ADC/SBC/RSB/RSC 结果、TST/TEQ/CMN 只动标志不写 Rd。

## 10.4 — 用例：MRS/MSR

- `test_mrs_msr`：MRS 读 CPSR/SPSR、MSR 字段写（控制字节 + 标志字节）、模式位保护、SPSR 写读回。
- **踩坑（测试）**：`MSR SPSR,r4` 编码 bit22（SPSR 选择）误写 0，实为 `MSR CPSR`；改 `0xE16FF004`。

## 10.5 — 用例：LDM/STM

- `test_block_transfer`：LDMIA/STMDB（PUSH/POP 别名）、写回、寄存器列表位图，逐寄存器断言。

## 10.6 — 用例：乘法

- `test_mul`：MUL/MLA、UMULL/UMLAL/SMULL/SMLAL 高低 64 位结果。
- **踩坑（测试）**：UMULL 编码 U-bit 反了（`0xE0C65190`→`0xE0865190`）；UMLAL 需先给 RdHi/RdLo 初值。

## 10.7 — 用例：字节/半字访存

- `test_byte_halfword`：LDRB/STRB/LDRH/STRH 边界与字节序、LDRSB/LDRSH 符号/零扩展、前后变址。

## 10.8 — 用例：SWI

- `test_swi`：SWI 后 `cpu->swi_num` 记录编号、PC 前进。

## 10.9 — 用例：SWP

- `test_swp`：SWP 交换后 Rd=旧内存值、内存=新寄存器值。
- **踩坑（测试）**：SWP 编码 Rd（bit15-12）与 Rm（bit3-0）位序写反，改 `0xE1002091`。

## 10.10 — 用例：MRC/MCR

- `test_coprocessor`：MCR 写 CP15 寄存器、MRC 读回一致（按 CRn 索引桩）。

## 10.11 — 用例：综合真码

- `test_stage10_integration`：PUSH/POP + SWI + MUL + STRH 循环搬 VRAM，断言 PC 停机与 VRAM 结果。
- **踩坑（测试）**：`MOV r4,#0x7C00` 编码误用 `0xE3A0447C`（带旋转），改 `0xE3A04C7C`（无旋转）。
- 阶段 10 收尾全量 **204 项检查 0 失败**。

## 11.3 — 用例：Div / Sqrt

- `test_bios_div_sqrt`：正/负除法（商/余/绝对值商）与开方（100/2/max）。参数经 `cpu->r[]` 直接预置，
  程序只需 `SWI Div` / `SWI Sqrt`（`0xEF090000`/`0xEF0D0000`）。

## 11.4 — 用例：CpuSet / CpuFastSet

- `test_bios_cpuset`：CpuSet 32 位拷贝 4 字、16 位固定源填充 8 半字、CpuFastSet 拷贝 4 字。
  控制字 `r2` 按 bit24(固定源)/bit26(宽度) 组合。

## 11.5 — 用例：BitUnPack / LZ77 / RL / Huffman

- `test_bios_decompress`：手造压缩流解压断言——BitUnPack（含偏移+零标志）、LZ77（"ABCABCABC"）、
  RL（"AAAAABBBBB"）、Huffman（"ABBA"，树：根两子均数据）。

## 11.6 — 用例：Halt / IntrWait / VBlankIntrWait

- `test_bios_wait`：自定义驱动（非 `run_program`）在等待中注入 `io_set_vblank`——
  未置位时 PC 停在 SWI，置位后前进并清 IF 位；IntrWait 验证 `IME=1`；Halt 验证 `(IE&IF)!=0` 才返回。

## 11.7 — 用例：综合真码

- `test_stage11_integration`：LZ77 解压到 VRAM → Div → Sqrt 串行，断言 VRAM 内容与寄存器。
- 阶段 11 收尾全量 **256 项检查 0 失败**。

## 12.2 — 用例：异常向量

- `test_exception_vector_undef`：向量表装到 Main RAM（`vector_base=0x02004000`），`LDC`（模拟器未实现）
  触发未定义异常，断言 PC=vec+0x04、模式=UND、SPSR 保存旧 CPSR、LR=PC+4。
- `test_exception_vector_swi`：未知 SWI（`SWI 0`）落入 SWI 向量 0x08，模式切 SVC。

## 12.3 — 用例：模式切换 + SPSR 分槽/恢复

- `test_mrs_msr` 改写：`MSR CPSR_c` 可切 IRQ 模式（旧版「模式位保护」断言废止），SPSR 改为
  `spsr[5]` 分槽读写（SPSR_IRQ 与 SPSR_SVC 互不影响）。
- `test_spsr_restore`：`LDMIA sp!, {pc}^` 加载 PC + 用 SPSR 恢复 CPSR。

## 12.4 — 用例：CP15 c1 控制向量基址

- `test_cp15_control`：MCR 写 c1 的 V 位（bit13）联动 `vector_base`（0→低向量、0x2000→高向量）。

## 12.5 — 用例：IRQ 真实响应

- `test_irq_response`：向量表装 handler（`SUBS pc, lr, #4`），预置 IE/IF/IME 后取指前触发 IRQ——
  断言 PC=vec+0x18、模式=IRQ、I 置位、SPSR 保存 User CPSR；再执行 handler 返回断言 PC/模式/I 恢复。
- **踩坑（回归）**：给 CPU 加真实 IRQ 后，阶段 11 的 `test_bios_wait`（Halt 分支）因残留 `IME=1` +
  手动置 `IE/IF` 而误入 IRQ 向量；在 Halt 分支显式 `IME=0` 隔离 HLE 轮询语义与真实 IRQ 路径。
- **踩坑（回归）**：`exec_step` 对「写 PC 的指令」仍 `r[15]+=4`，导致 `SUBS pc`/`LDM {pc}^`/`LDR pc`
  的跳转目标被 +4 覆盖；修复后异常返回正确。
- 阶段 12 收尾全量 **282 项检查 0 失败**。

## 13.3 — 用例：Thumb 数据处理

- `test_thumb_dataproc`：15 条 16 位指令（MOV #imm8、AND/EOR/ORR/MUL/NEG、高寄存器 MOV/ADD）一条条手编，
  断言各寄存器结果与 PC 推进 2×N。新增 `thumb_write/thumb_start/thumb_stop` 辅助（写 16 位程序、置 T 位、复位）。

## 13.4 — 用例：Thumb 访存

- `test_thumb_memory`：直接预置 `r0=基址` 单步验证 STR/LDR 字、STRB/LDRB 字节、STRH/LDRH 半字、
  SP 相对、寄存器偏移、LDR 字面量池（[PC,#0] 读 base+4 处 32 位常量）。

## 13.5 — 用例：Thumb 分支/切换/SWI + 块操作

- `test_thumb_branch`：BX 偶/奇地址切换 T 位；BEQ 命中/未命中；B 无条件；BL（PC/LR/T）；BLX（切 ARM）；
  SWI #0x09 Div。
- `test_thumb_stack`：PUSH {r0-r3,lr}（STMDB sp!）、POP {r0-r3}（LDMIA sp!）、STMIA/LDMIA 写回。

## 13.7 — 用例：Thumb 真码写 VRAM + 调 SWI

- `test_thumb_vram`：LDR 字面量取 VRAM 基址 → MOV #0x1F → STRH 写 VRAM[0] → MOV 准备被除数/除数 →
  SWI Div → B . 保活；断言 VRAM 像素 0x001F 与 Div 结果。
- **踩坑（测试）**：① `BX r1` 误编 `0x4708`（Rm 落在 bits5-3 被忽略，实为 BX r0），改 `0x4701`；
  ② `MOV r1,r3` 误编 `0x4631`（Rs=6 而非 3），除数为 0 触发 bios_div 的除零防御分支，改 `0x4619`。
- 阶段 13 收尾全量 **336 项检查 0 失败**。

## 14.2 — 用例：KEY1（Blowfish）块加解密 + 密钥表自检

- `test_key1_roundtrip`：`key1_table_checksum()` 累加和 == `0x000803BA`（0x1048 字节之和，防表被误改）；
  `key1_bswap32(0x12345678) == 0x78563412`；level 1/2/3 各做「`key1_encrypt64` → `key1_decrypt64` 还原」
  且断言密文 ≠ 明文。
- 说明：`key1_encrypt64/decrypt64`、`key1_init_keycode`、`key1_table_checksum` 为 `src/cart/key1.h` 公开接口，
  测试直接调用（不依赖 nds 实例）。

## 14.2 — 用例：安全区加密→解密往返 + encryObj 魔数

- `test_key1_secure_area`：0x800 字节填可辨认模式 → `key1_encrypt_secure_area(gamecode)` →
  断言头 8 字节与 body 均改变 → `key1_decrypt_secure_area` → 断言返回 1、头 8 字节 == `"encryObj"`、
  body 与明文一致。覆盖 level2 剥外层 + level3 解全部的双层协议。

## 14.5 — 用例：自制含加密安全区的 ROM 完整装载

- `test_secure_area_load`：内存里造一份最小 ROM（头 gamecode `0x00C` + ARM9 四字段 `0x020`、安全区 `0x4000`），
  `key1_encrypt_secure_area` 加密安全区 → `cart_decrypt_secure_area` 解密 → 逐字节拷进 Main RAM →
  断言 RAM 里魔数 + `cpu_reset(entry)` 后 PC 正确。用 `put_le32` 小端写头字段。
- 阶段 14 收尾全量 **360 项检查 0 失败**。

## 15.2 — 用例：卡带总线命令读（cartbus）

- `test_cartbus_read`：`io_attach_cart` 挂内存 ROM → 写 `B7` 命令（大端地址）到 CARD_COMMAND →
  写 ROMCTRL 激活（bit31）→ 断言 DRQ（bit23）置位 → 连续 `bus_read32(CARD_DATA)` 读回 4 字节小端，
  游标 +4、越界返回 `0xFFFFFFFF`；块读完 DRQ 自清。

## 15.3 — 用例：DMA 4 通道 + 地址控制 + VBlank 触发

- `test_dma_channels`：DMA0/1 独立搬运不串扰（源递增/固定）；源递减模式（`SRC_DEC` 从高地址倒读）；
  VBlank 触发（先 `io_set_vblank` 再 `dma_fire`）启动延迟 DMA。

## 15.4 — 用例：卡带 DMA 集成（DMA0 从 CARD_DATA 搬 RAM）

- `test_card_dma`：配 DMA0（源=CARD_DATA 固定、目的=RAM、32 位、卡带触发）→ 写 `B7` 命令 + 激活
  ROMCTRL → 卡带就绪触发 DMA 搬 8 字 → 断言数据一致、DRQ 状态、使能自清、IF bit19（卡带完成）置位。

## 15.5 — 用例：CPU 程序 DMA 读卡带（综合）

- `test_card_program`：真实 ARM 指令设 DMA0 → 预写 `B7` 命令 → `STR` 激活 ROMCTRL → 卡带就绪 →
  DMA 搬 4 字到 RAM；断言 PC 停机（0x02000044）、word[0..3] 与 ROM 数据一致、使能自清。
- 阶段 15 收尾全量 **393 项检查 0 失败**。

## 16.2 — 用例：存档芯片 SPI 状态机（EEPROM / Flash）

- `test_save_eeprom`：直接喂 `save_transfer` 序列——WREN 置 WEL、RDSR 读回 `0x02`、WRITE 0x0100 写 4 字节、
  READ 读回一致；验证状态寄存器 bit1、16 位地址累积。
- `test_save_flash`：RDID 连回 3 字节 JEDEC ID（`20 20 12`）、PE 页擦除后读 `0xFF`、PP 写 4 字节读回一致、
  再写 `0x0F` 验证「与」语义（`0x12 & 0x0F = 0x02`）。

## 16.2/16.4 — 用例：经 AUXSPICNT/AUXSPIDATA 总线读写存档

- `test_save_spi_regs`：`io_attach_save(EEPROM_8K)` → 写 AUXSPICNT `0xA040` 选片 → WREN → 撤片选 →
  再选片 → WRITE 地址 0x20=0xAB → 撤片选 → 再选片 → READ + 哑元 → 读 AUXSPIDATA 得 0xAB；
  断言芯片缓冲 `data[0x20]=0xAB`、总线读回 0xAB。

## 16.3 — 用例：存档持久化（.sav 写读回）

- `test_save_persist`：写 `test_save_tmp.sav` → `memset` 破坏内存 → `save_load_file` 重载 →
  断言 3 个字节一致；`remove` 清理临时文件。

## 16.4 — 用例：CPU 程序经 AUXSPICNT/AUXSPIDATA 读写存档（综合）

- `test_save_program`：真实 ARM 指令（STRB/LDRB）走完整存档流程——选片(0xA040) → WREN → WRITE 0x20=0xAB →
  撤片选 → 选片 → READ 0x20 → LDRB 收数据到 r3 → 停机；断言 PC=0x02000088、r3=0xAB、芯片缓冲一致。
- **踩坑（测试）**：STRB 编码 = `0xE5C00000 | Rd<<12 | Rn<<16 | offset`、LDRB = `0xE5D00000 | Rd<<12 | Rn<<16 | offset`
  （对照阶段 15 的 STR 字编码 `0xE58`/`0xE59` 加 bit22 得字节变体）。
- 阶段 16 收尾全量 **423 项检查 0 失败**。

## 17.2 — 用例：TSC 触摸屏 SPI 状态机（单元）

- `test_touch_unit`：直接 `touch_write8/touch_read8` 驱动——SPICNT 写 `0x8A00`（使能+触摸+保持片选）读回一致；
  未按下 X 读回 0x000、Y 读回 0xFFF（首/次字节 0x7F/0xF8）；按下 (0x2AB,0x2CD) 后 12 位拼装
  `((b1&0x7F)<<5)|(b2>>3)` 还原 X/Y；8 位模式（`0xD8`）回传高 8 位 `0x2AB>>4=0x2A` 的两字节；清 Hold 后
  非命令字节回传 0（命令状态复位）。

## 17.2 — 用例：经 SPICNT/SPIDATA 总线读触摸坐标（集成）

- `test_touch_spi_regs`：`io_set_touch(0x2AB,0x2CD,1)` → `bus_write16(SPICNT,0x8A00)` → 写命令 0xD0/0x90 +
  哑元 → `bus_read8(SPIDATA)` 两字节拼装还原 X/Y；未按下（`io_set_touch(...,0)`）Y 读回 0xFFF。

## 17.3 — 用例：CPU 程序经 SPICNT/SPIDATA 读出 X/Y 坐标（综合）

- `test_touch_program`：真实 ARM 指令（MOV/ADD/STRB/LDRB + AND/MOV 移位/ORR）走完整流程——SPICNT=0x8A00 →
  命令 0xD0 读 X 两字节拼 12 位到 r6 → 命令 0x90 读 Y 到 r7 → 停机；断言 PC=0x02000078、r6=0x2AB、r7=0x2CD。
- **踩坑（测试）**：12 位拼装需移位指令——`MOV r3,r3,LSL #5` 编码 `0xE1A03283`、`MOV r4,r4,LSR #3` 编码
  `0xE1A041A4`（数据处理的移位在 bit11-7 立即数 + bit6-5 移位类型）。
- 阶段 17 收尾全量 **443 项检查 0 失败**。

## 18.2 — 用例：音频寄存器读写（SOUNDCNT/SOUNDBIAS + 16 通道）

- `test_snd_regs`：`bus_write16/read16` SOUNDCNT `0x807F`、SOUNDBIAS `0x0200` 写读回；通道 0 全套
  CNT/SAD/TMR/PNT/LEN 写读回；通道 1 TMR（16 字节步进）独立；越界地址（主控区上界 `SND_END`）读 0。

## 18.3 — 用例：混音器（PCM8/PCM16/ADPCM/PSG + 静音/主音量/单发停止）

- `test_snd_mix`：SOUNDCNT=0x807F + SOUNDBIAS=0x200 下——
  - 无通道静音 → 输出 0；PCM8 样本 0x40 → 满偏右声道 out=0x4000；PCM16 样本 0x4000 → 0x4000；
  - IMA-ADPCM 头 0x20（init=16384）+ 数据 nibble 0 → 0x4000；PSG 方波（duty=0）满幅 → 0x7FC0；
  - 主音量 0 静音；单发 PCM8（len=1/tmr=1）1 采样后 bit31 自清。
- **踩坑（测试）**：ADPCM 通道初写 CNT 用了 PCM8 的格式位（0x90），应带格式 ADPCM=2（bit29-30）→
  `0xD07F007F`；各通道逐个测完要写 CNT=0 停掉，避免下一条混音叠加前通道。

## 18.4 — 用例：CPU 程序配置通道 0 并读回音频寄存器（综合）

- `test_snd_program`：真实 ARM 指令（MOV/ADD/STRB/LDR/STR）——r0=0x04000400，逐字节写 CNT=0x907F007F
  （STRB 写高字节触发 bit31 启动复位），写 SAD=0x02000000/TMR=0x1000/LEN=4，LDR 读回 r4..r7 → 停机；
  断言 PC=0x02000050、r4=0x907F007F、r5=0x02000000、r6=0x1000、r7=4。
- **踩坑（测试）**：32 位常量 `0x907F007F` 非合法立即数，改逐字节 STRB 拼装（触屏测试同法）；
  `0x02000000`/`0x1000` 可用单条 MOV（`0xE3A02780`/`0xE3A02C10`）。
- 阶段 18 收尾全量 **467 项检查 0 失败**。

## 19.2/19.3 — 用例：顶点变换（pos×proj → 透视除 → 视口映射）

- `test_gx_transform`：`gx_reset`（pos/proj=单位阵、视口=(0,0,255,191)）后直接 `gx_transform_vertex`——
  (-1,-1)→(0,191)、(1,-1)→(255,191)、(-1,1)→(0,0)、(0,0)→(127,95)（整数截断），8 项断言全过。

## 19.4 — 用例：三角形软件光栅化 + 3D 图层合成

- `test_gx_raster`：`gx_raster_tri(10,10,100,10,10,100,红)` 断言内点 (50,50)=红、外点 (150,150)/(5,5)=0。
- `test_gx_layer`：红三角 + mode5 直色位图黑背景，`render_frame` 在 DISP3DCNT 未使能时 (50,50) 露黑、
  使能（bit13）后覆盖为红 `0xFFF80000`，验证 3D 图层合成进顶屏。

## 19.5 — 用例：GXFIFO 命令流 → 变换 → 光栅化出图（综合）

- `test_gx_fifo`：`bus_write32` 发完整命令流——MTX_MODE 投影(0) → MTX_LOAD_4x4 diag(1/4,1/4,1,1) →
  MTX_MODE 位置(1) → MTX_IDENTITY → COLOR 红 → VIEWPORT 全屏 → BEGIN_VTXS 三角 + 3 个 VTX_16
  （(0,0)/(4,0)/(0,-4)）→ END_VTXS；断言三角形 (127,95)-(255,95)-(127,191) 内点 (200,120)=红、
  外点 (200,160)/(120,100)=0。
- **踩坑（回归）**：音频由 ARM7 控制、几何由 ARM9 控制（0x04000400 同址分流）——原 `test_snd_regs/mix`
  默认 ARM9 视角写音频、`test_snd_program` 用 ARM9 核写音频，加 gx 分流后需分别改为
  `active_is_arm7=1` 与改用 `run_cpu7`（ARM7 核），否则音频寄存器写入被几何区整字转发吞掉。
- 阶段 19 收尾全量 **483 项检查 0 失败**。

## 20.1 — 用例：仿射背景寄存器读写 + MODE2 仿射出图

- `test_affine_regs`：主引擎 BG2 PA-PD（1.7.8）+ X/Y 参考点（1.19.8）写读回；负参考点写
  `0xFFFFFF00` 读回 `0x0FFFFF00`（bit28-31 无效）；BG3 参数块（+0x10 偏移）+ 副引擎（+0x1000）。
- `test_affine_render`：MODE2 + BG2 仿射，单位矩阵 + 参考点(0,0) 时屏幕=纹理；2× 缩放（PA=PD=2.0）后
  红 tile 盖 0..3、绿盖 4..7、外透明；断言像素值精确匹配。

## 20.2/20.3 — 用例：混合/亮度/窗口/捕获

- `test_blend_regs`：混合三件套（BLENDCNT/BLDALPHA/BLDY）+ MASTER_BRIGHT + WIN0H/WININ/WINOUT +
  DISPCAPCNT + 副引擎镜像，写读回断言。
- `test_blend_render`：BG0 红像素 + 蓝背景——混合关闭直出；模式1 Alpha（EVA=EVB=8）红+蓝各半 `0xFF780078`；
  模式2 增亮灰 `0x4210→0xFFB8B8B8`；模式3 减暗 `→0xFF404040`；MASTER_BRIGHT 增亮因子 8 `0xFF808080→0xFFBCBCBC`。
- `test_capture_render`：顶屏直色位图（红/蓝两像素）+ DISPCAPCNT 使能，渲染后 LCDC VRAM（`0x06800000`）
  读出 `0x7C00/0x001F`，且使能位自清。
- `test_window_render`：WIN0 限定左上 8×8，窗内只开 BG0 → 红，窗外全关 → 露黑背景。
- **踩坑（测试）**：`test_blend_render` 初版 BG0CNT 未设屏幕块1，tilemap 与字符数据同落 `0x06000000`
  重叠导致红像素不画；改 `BGCNT_COLORS_256 | (1<<SCREEN_BASE_SHIFT)` 后通过。
- 阶段 20 收尾全量 **521 项检查 0 失败**。

## 2026-09-05 · 工程：测试入口自动切 UTF-8 控制台

- **做了什么**：`test_nds.c` 顶部增加 Windows 条件包含 `<windows.h>`，`main()` 第一条 `printf` 前调用
  `SetConsoleOutputCP(CP_UTF8)`，避免测试用例名等中文在 936(GBK) 控制台下乱码。
- **怎么验证**：`cmake --build build --parallel` 编译通过；直接运行 `build/test_nds.exe`，
  528 项检查 0 失败（B1 Shared WRAM 用例仍在工作区，未提交）。
- **结果**：✅ 编译与自动化测试通过（终端中文显示待用户按验收步骤确认）。

## 2026-09-05 · 21-B2 — IPCSYNC 同步寄存器用例

- `test_nds.c` 新增 `[case 21-B2]`：`test_ipcsync_regs`（双核 out 交叉 + 字节访问 +
  只读低字节）与 `test_ipcsync_irq`（bit13 请求 → 对端 IF16、enable 门控、写 1 清除），
  共 16 项检查。
- 全量 **544 项检查 0 失败**；真 ROM headless 不再报 `0x04000180/81` 未知 IO。
- **结果**：✅ 用户验收通过（2026-09-05）。

## 2026-09-05 · 21-B6 — Main RAM 镜像 ARM7 不可见用例

- `test_main_ram_mirror` 增 2 项：ARM7 视角写镜像区被丢弃（读回 0）、主存未被污染
  （FFXII 栈槽偏移 0x3E3B34）；ARM9 视角双向别名用例不回归。
- 全量 **559 项检查 0 失败**；真 ROM：ARM9 不再因 ARM7 块拷贝覆盖栈而弹 0xE1C010B0。

## 2026-09-05 · 21-B7 — EXMEMCNT/WRAMCNT + Shared WRAM 切分用例

- `test_nds.c` 新增 `[case 21-B7]`：
  - `test_wramcnt_regs`：EXMEMCNT 双核初值 0xE880、半字可写位与字节访问、ARM9
    写高 7 位镜像给 ARM7、ARM7 只改自己低 7 位；WRAMCNT ARM9 0x247 写 /
    ARM7 0x241 只读，共 13 项检查。
  - `test_wramcnt_split`：Shared WRAM mode 3 全给 ARM7（ARM9 盲写被忽略）、
    mode 0 全给 ARM9（ARM7 转看 ARM7 WRAM 别名）、mode 1/2 半区互换 + 镜像重复
    别名，共 13 项检查。
  - `test_shared_wram`（B1 回归）开头补切 WRAMCNT=0，其余断言不变。
- 全量 **587 项检查 0 失败**；真 ROM headless 不再报 EXMEMCNT/WRAMCNT 未知 IO。

## 2026-09-05 · 21-B5 — Thumb BX 回归 + trace

- `thumb.c` 普通 Thumb 指令新增逐条 trace（bring-up 诊断）；修复 BX/BLX 寄存器号解码
  （bit6:3），`BX lr=0x4770` 不再被当成 `BX r8`。
- 更正早期 13.5「踩坑」记录：BX r1 的规范编码是 `0x4708`（Rm 在 bit6:3），旧测试用的
  `0x4701` 只是迁就旧解码器；测试改回 0x4708 并新增 BX lr 用例 2 项。
- 全量 **557 项检查 0 失败**；真 ROM IO 区刷屏消失，ARM9 新卡点（栈 LR 被污染）记入 B6。

## 2026-09-05 · 21-B4 — ARM BX 奇地址切 Thumb 用例

- `test_nds.c` 新增 `[case 21-B4]`：`test_arm_bx_thumb`（Thumb 区 MOVS r1,#5 +
  自循环，ARM 侧 ORR #1 后 BX），断言 T=1、r1=5、PC 进 Thumb 区，共 3 项检查。
- 全量 **555 项检查 0 失败**；真 ROM：ARM7 不再按 ARM 误译 0x038043C9，旧 0x0626D1xx
  漂移终点消失，进入 Thumb 区后的二次跑飞另记 B5。
- **结果**：✅ 用户验收通过（2026-09-05）。

## 2026-09-05 · 21-B3 — Main RAM 无缓存镜像用例

- `test_nds.c` 新增 `[case 21-B3]`：`test_main_ram_mirror`（双向别名、FFXII 栈区
  0x027E3F80 往返、首字节/末字边界、越界读 0），共 8 项检查。
- 全量 **552 项检查 0 失败**；真 ROM headless：ARM9 不再弹 PC=0、不再在零区漂移。
- **结果**：✅ 用户验收通过（2026-09-05）。

## 2026-09-05 · 21-B8 — ARM9 DTCM/ITCM 用例

- `test_nds.c` 新增 `[case 21-B8]`：`test_arm9_tcm`——
  ARM9 视角 0x027E3B34 写读走 DTCM；ARM7 同址写落到 Main RAM 0x3E3B34 且不碰
  DTCM；ITCM 0x01FF8000 仅 ARM9 可写读、ARM7 写落空。共 7 项检查。
- 21-B3 镜像用例同步修正：ARM7 写 0x024-0x027 别名现在落 Main RAM（melonDS 口径）。
- 全量 **594 项检查 0 失败**；真 ROM headless 越过旧信箱死等，下一卡点排入 B9。

## 2026-09-05 · 21-B9 — BIOS SWI 0x0E GetCRC16 用例

- `test_nds.c` 新增 `[case 21-B9a]`：`test_bios_crc16`——
  Main RAM 写入 ASCII `"123456789"`；r0=0xFFFF、r1=数据地址、r2=9，ARM 态 SWI 0x0E
  得 0x4B37 且 PC 前进；r2=0 返回初值 0xFFFF；Thumb 态同参同结果；ARM7 核同样得
  0x4B37。共 6 项检查。
- 全量 **600 项检查 0 失败**；真 ROM headless：`unknown SWI 0x0E` 消失，
  ARM7 终点前进到 0x037FC89C。

## 2026-09-05 · 21-B9c — ARM9 IRQ 槽跳板用例

- `test_nds.c` 新增 `[case 21-B9c]`：`test_irq_slot_jump`——DTCM 配置到
  0x027E0000、槽 0x027E3FFC 写 0x01FF8000，IRQ 触发后断言 PC 跳到 ITCM
  handler、IRQ 模式/I/SPSR/LR 正确，再执行 `SUBS pc,lr,#4` 验证返回并恢复
  CPSR。共 8 项检查。
- 全量 **608 项检查 0 失败**；真 ROM：ARM9 不再从高向量漂移，进入 ITCM 中断
  分发器后停在 0x01FF8028（CLZ 未实现，B9d 修）。

## 2026-09-05 · 21-B9d — CLZ 前导零计数用例

- `test_nds.c` 新增 `[case 21-B9d]`：`test_clz`——单步执行 `CLZ r0,r1`，
  覆盖 0→32、0x80000000→0、0x00000001→31、0x00042009→13、0x00000008→28，
  含 PC 前进断言共 10 项检查。
- 全量 **618 项检查 0 失败**；真 ROM：IRQ 分发循环不再死等，ARM9 前进到
  0x02006488（新卡点 ARM BLX Rm，B9e 修）。

## 2026-09-05 · 21-B9e — ARM BLX Rm 用例

- `test_nds.c` 新增 `[case 21-B9e]`：`test_arm_blx_reg`——单步 `BLX r1`，
  ARM→Thumb（0x02001001）与 ARM→ARM（0x02001100）各验证 LR/T/PC 及目标首
  条指令执行，共 10 项检查。
- 全量 **628 项检查 0 失败**；真 ROM：ARM9 越过 0x02006488，回到 0x02009EC0
  主等待循环，8M 步无固定单点卡死。

## 2026-09-05 · 21-B9f — IRQ HLE 桩现场保存/恢复用例

- B9c 用例升级为 FFXII 分发器风格：handler push lr → 破坏 r1 → pop pc 返回，
  断言返回后 r1 恢复、CPSR 恢复、被中断指令重执行（case 21-B9c 现共 13 项）。
- 全量 **633 项检查 0 失败**；真 ROM：第一次 VBlank 链执行完，第二次 IRQ 前
  ARM9 落入未映射区（共享 SP 覆盖现场，B9g 修 banked r13/r14）。

## 2026-09-05 · 21-B9g — 模式私有 r13/r14 用例

- `test_nds.c` 新增 `[case 21-B9g]`：`test_banked_r13_r14`——SVC/System/IRQ
  往返切换，各模式 SP/LR 独立保留；B9c 用例补 IRQ 专用 SP 配置。共 10 项
  新增检查。
- 全量 **643 项检查 0 失败**；真 ROM：ARM9 不再跑飞，推进到 0x02007CE0
  链表遍历；新卡点=未知 IO 0x04000280-2BF（B9h）。

## 2026-09-05 · 21-B9h — LDM ^ User 槽用例 + VBlank bit0 迁移

- `test_nds.c` 新增 `[case 21-B9h]`：SVC 模式 `LDMIA r0,{r0-r14}^`，验证 User 槽
  r13/r14 被装载、SVC 私有 SP/LR 不被覆盖（6 项新检查）。
- 6.x 的 VBlank 用例/IRQ 分流/CLZ 位常量从 bit3 全部迁到 bit0，避免自造约定和
  真机硬件冲突。
- 全量 **649 项检查 0 失败**；真 ROM：任务队列不再成环，多帧 VBlank IRQ 正常
  进入/恢复。

## 2026-09-05 · 21-B9i — FIFO CNT 合并写 + ARM7 IRQ 槽用例

- `test_nds.c` 新增 `[case 21-B9i]`：
  - `test_fifo_cnt_combine`：按 FFXII 的字节序写 CNT（0x08/0xC4），验证使能与收
    IRQ 位保留、send 入队、ARM7 IF18 置位（4 项）。
  - `test_arm7_irq_slot`：0x0380FFFC 槽指向 WRAM handler，验证 IRQ 进入/破坏 r1/
    恢复现场/重执行被打断指令（6 项）。
- 全量 **659 项检查 0 失败**；真 ROM：ARM9 FIFO 命令能送达 ARM7，ARM7 handler
  不再跑飞；忙位清除路径待 B9j。

## 2026-09-05 · 21-B9j（前半）— TM3 reload / 双核 VBlank 用例

- `test_timer_reload_overflow`：CNT_L=0xFFF0 后写 CNT_H(0xC0)，15 步无 IF、第 16 步
  溢出置 ARM7 IF bit6 并回 reload（4 项）。
- 6.3 补 ARM7 IF 同步收到 VBlank（1 项）。
- 全量 **664 项检查 0 失败**；真 ROM：ARM7 现在有持续 VBlank 事件可推进状态机，
  但 service6 完成回执仍未出现，B9j 后半继续。

## 2026-09-06 · 21-B9j（后半）— SPI device1 固件回读用例

- `test_spi_fw_hle`：SPICNT=0x8900（使能+device1）后写 READ 命令，SPIDATA 回
  0xFF（1 项）。
- 全量 **665 项检查 0 失败**；真 ROM：ARM9 第一次离开忙等并推进到后续服务循环，
  ARM7 不再停在 0x0200EA90 对应等待。

## 2026-09-06 · 21-B9j+ — SPI device1 完整状态机用例

- `test_spi_fw_hle` 重写为 FFXII 真实序列：0x03+大端地址 03 FE 00（地址首字节
  也是 0x03）→ 镜像 0 的 version/favoriteColor；0x20 用户设置偏移 C0 7F；
  RDSR；0x3FF70 镜像 1 的 Update Counter=1、CRC=BAFD。共 11 项。
- 全量 **675 项检查 0 失败**。

## 2026-09-06 · 21-B9k — 硬件除法/开方 + ROMCTRL 忙位用例

- `test_math_div_sqrt`：模式 0/1/2 除法、除零 DIV0 位与商/余数、32 位
  -MAX/-1 溢出、只读位写不进去、32/64 位开方（21 项）。
- 15.2 补 4 字节块：激活后 bit31=1，读 1 字后 busy 与 DRQ 同时回落（3 项）。
- 全量 **700 项检查 0 失败**。

## 2026-09-06 · 21-B9l — SoundBias SWI 用例

- `test_bios_soundbias`：ARM7 Thumb `SWI 0x08`，r0≠0 → SOUNDBIAS=0x200，
  r0=0 → 0x000，PC 均前进 2 字节（4 项）。
- 全量 **704 项检查 0 失败**。

## 2026-09-06 · 21-B9n — 电源/启动寄存器用例

- `test_power_regs`：POSTFLG 默认 1/bit0 粘住/ARM9 bit1 可写/ARM7 bit1 恒 0；
  POWCNT1 可写掩码 0x820F、POWCNT2 掩码 0x0003；WIFIWAITCNT 默认 0x30 并可写
  （12 项）。
- 全量 **716 项检查 0 失败**；真 ROM 越过空闲死锁继续启动服务。

## 2026-09-06 · 21-B9o — 双核定时器分离用例

- 21-B9j 用例原断言 `io->timer[3]`，改为 ARM7 套 `io->timer[1][3]`（该用例
  本来就以 ARM7 视角写 TM3）。
- 全量 **716 项检查 0 失败**。

## 2026-09-06 · 21-B9p — DMA 完成中断用例

- 15.3 补：DMA0 立即搬运 1 字 + CNT bit14，搬完断言目的内存与 IF bit8（2 项）。
- 全量 **718 项检查 0 失败**。

## 2026-09-06 · 21-B9q — 双核 DMA 分离用例

- 既有 15.3/15.4 DMA 用例默认 ARM9 视角，迁移到 `io->dma[0]` 后语义不变；
  15.4 卡带 DMA 由 ARM9 侧配置仍能触发。全量 **718 项检查 0 失败**。

## 2026-09-06 · 21-B9s — ARM BLX 立即数用例

- `[case 21-B9s]`：在 Main RAM 放 `0xFA0003FE`（BLX 0x02001000），目标放
  Thumb `MOVS r0,#0`；断言 lr=base+4、T=1、PC=目标，并执行目标首条指令
  （5 项）。全量 **723 项检查 0 失败**。

## 2026-09-06 · 21-B9u — VCOUNT / DISPSTAT 用例

- `[case 21-B9u]`：VCOUNT 初始 0，`io_set_vblank` 后行号归 0、DISPSTAT bit0=1；
  DISPSTAT 可写 IRQ 使能与比较值。全量 **729 项检查 0 失败**。

## 2026-09-06 · 21-B9v — 逐行 VCOUNT / DISPSTAT 写语义用例

- VCOUNT 帧起始为 0，`io_advance_scanline` 后 +1；DISPSTAT 低字节只接受
  IRQ 使能位、高字节保存 VCount 比较值。全量 **731 项检查 0 失败**。

