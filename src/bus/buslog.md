# bus 模块日志

## 2.2 — bus 结构体与 Main RAM / VRAM / IO 数组

- 新建 `src/bus/bus.h` / `bus.c`（模块目录结构见 `docs/01`）。
- `bus_t` 内含 `main_ram[4MB]`、`vram[656KB]`、`io[64KB]`，`bus_create` 用 `calloc` 清零（未写区域读 0）。
- `nds_t` 挂入 `bus_t *bus`；`nds_create` / `nds_destroy` 负责创建与释放。
- `CMakeLists.txt` 加入 `src/bus/bus.c`。

## 2.3 — bus_read8 / bus_write8 + 地址换算

- 核心 helper `bus_resolve()`：判断 `addr` 落在哪个区间，填出「区间数组指针 + 偏移」。
- 换算规则：区间内下标 = `addr - 区间基址`（如 `0x02000100 - 0x02000000 = 0x100`）。
- 用「先减后比较」避免无符号下溢误命中；未映射/IO 读 0、写忽略，任意地址不崩。
- main.c 加临时自测块（阶段 5 起换正式单测）。

## 2.4 / 2.5 — read16 / write16、read32 / write32（小端）

- 逐字节走 read8/write8 再按小端拼拆：`b0 | b1<<8 | b2<<16 | b3<<24`。
- 自测：写 `0xABCD` 读回 `0xABCD`（字节序 `CD AB`）；写 `0x12345678` 读回原值（字节序 `78 56 34 12`）。

## 2.6 — VRAM 区间映射

- `bus_resolve` 增加 VRAM 分支（基址 `0x06000000`，656KB），换算与 Main RAM 相同。
- 自测：VRAM 写 `0xBEEF` 读回 `0xBEEF`。

## 2.7 — IO 桩

- `bus_resolve` 显式列出 IO 区间（`0x04000000` 起 64KB），读 0、写忽略，为后续实现寄存器留位。
- 自测：写 `0x11` 到 IO 后读回 `0x00`，不崩。

## 2.8 — 与装载衔接

- main.c 移除临时 `arm9_ram` 静态数组，改为 `nds_create` 后经 `bus_write8` 把 ARM9 镜像逐字节写入 Main RAM（地址 = 头 `ram` 字段）。
- 用 `bus_read32` 从 RAM 读回镜像首字验证：`FE FF FF EA` → `EAFFFFFE`，装载-读回闭环成立。

## 6.2 — IO 桩 → io 模块路由

- `bus_t` 移除 `io[64KB]` 桩数组，改持 `io_t *io` 指针（前向声明 `typedef struct io io_t;`）。
- `bus_read8/write8` 对 IO 区间（`0x04000000` 起 64KB）转发给 `io_read8/io_write8`（真实寄存器语义），未挂 io 时读 0 兜底；其余地址仍走 `bus_resolve`。
- `nds_create` 创建 io 后 `bus->io = nds->io`，逆序销毁。桩语义（未实现寄存器读 0 写忽略）由 io 模块保留，原 IO 桩自测不回归。

## 8.3 — ARM7 WRAM 映射 + active_is_arm7 + FIFO RECV/SEND 整字路由

- `bus_t` 新增 `arm7_wram[64KB]`（`0x03800000` 起）与 `int active_is_arm7`（当前访问者身份，由
  `cpu_step` 在取指前设置）。`bus_resolve` 增加 ARM7 WRAM 分支。
- `bus_read8/write8` 转发 IO 时把 `active_is_arm7` 传给 `io_read8/io_write8`，使中断寄存器
  （IME/IE/IF）与 FIFO CNT 按 CPU 分流（`irq[0]`=ARM9、`irq[1]`=ARM7）。
- **FIFO 整字路由**：IPC FIFO RECV 地址 `0x04100000` 在 IO 区间之外，`bus_read32` 检测该地址
  整体走 `io_recv32`；SEND 地址 `0x04000188` 是 32 位寄存器，`bus_write32` 检测该地址整体走
  `io_send32`——避免拆成 4 字节破坏队列/被忽略。
- **怎么验证**：`test_arm7_wram`（写读回 + 越界读 0）、`test_irq_split`（两核 IME/IE 独立）、
  `test_fifo_basic`（跨核收发一字）。

## 9.2 — 调色板 RAM / OAM / VRAM 固定窗口

- 新增内存区间（配合真 2D PPU）：
  - **调色板 RAM** `0x05000000`（2KB，`palette[0x800]`）：BG/OBJ 颜色查表，主 `0x05000000`+副 `0x05000400`。
  - **OAM** `0x07000000`（2KB，`oam[0x800]`）：OBJ 属性内存，主 `0x07000000` 1KB + 副 `0x07000400` 1KB。
  - **VRAM 固定窗口**（各 128KB，对应 libnds vramDefault bank 分配）：副 BG `0x06200000`→`vram[0x40000]`
    （bank C）、主 OBJ `0x06400000`→`vram[0x20000]`（bank B）、副 OBJ `0x06600000`→`vram[0x60000]`（bank D）。
  - `0x06000000` 仍保持 656KB 全量映射（向后兼容 + 主引擎全量访问）。
- 验收：`test_disp_regs` 里验证副 BG 窗口写 `0x06200000` 与物理 `vram[0x40000]` 读回一致。

## 15.4 — 卡带数据端口 CARD_DATA 整字路由

- `bus.h` 加 `BUS_CARD_DATA`（`0x04100010`，4 字节数据输入端口）；`bus_read32` 命中即转发
  `io_card_data_read32`，`bus_write32` 命中即转发 `io_card_data_write32`（写为占位忽略）。
- 与 8.3 的 FIFO RECV/SEND 同理：`0x04100010` 在 IO 区间外、且是「读一次游标 +4」的有副作用端口，
  不能拆成 4 字节逐字节读，必须整字路由。
- 验收：`test_card_dma` / `test_card_program` 经 `bus_read32(CARD_DATA)` 连续读卡带数据正确。

## 19.2 — 几何命令区整字路由（GXFIFO / 命令端口）

- `bus_write32` 增几何区（`0x04000400..0x040005FF`）ARM9 整字转发：`addr∈[GX_GXFIFO, GX_CMD_PORT_END) && !active_is_arm7`
  时整体走 `io_gx_write32`，不再拆成 4 字节——否则会破坏「命令字 + 参数字」的 40 位命令语义。
- 该区间与音频寄存器重叠：ARM9 视角是几何命令 FIFO/命令端口，ARM7 视角仍是音频（`active_is_arm7=1`
  时 `bus_write32` 走原拆字节路径 → `io_write8` → `snd_write8`）。
- 验收：`test_gx_fifo`（GXFIFO 命令流经 `bus_write32` 出图）；原 `test_snd_*` 切 ARM7 视角后全过。

## 20.2 — LCDC 分配 VRAM 窗口（显示捕获目标）

- `bus.h` 增 `BUS_LCDC_VRAM_BASE=0x06800000`、`BUS_LCDC_VRAM_SIZE=512KB`；`bus_resolve` 新增分支映射到
  `vram[0]`（与 `0x06000000` 主 VRAM 同物理区），供 `render_capture` 把顶屏出图写回 LCDC VRAM。
- 验收：`test_capture_render` 捕获后从 `0x06800000` 读回顶屏 RGB555。

## 2026-09-05 · 21-B1 — Shared WRAM 主区 + 镜像映射

- **做了什么**：`bus.h` 新增 32KB Shared WRAM 常量与 `bus_t.shared_wram[32KB]`；`bus_resolve` 增加两个分支：
  主区 `0x03000000` 与镜像区 `0x037F8000` 各自 32KB 换算到**同一物理数组**，写主区可从镜像读回、
  写镜像也可从主区读回（真机同 RAM 双地址别名）。`tests/test_nds.c` 新增 `test_shared_wram`
  （8/16/32 位读写、双向别名、末字节/末字边界、主区越界读 0），注册为 `[case 21-B1]`。
- **怎么验证**：`cmake --build build --parallel` 编译通过；`test_nds.exe` **528 项检查 0 失败**；
  真 ROM `--headless 20000000` 重跑：ARM7 不再在 `0x037F8xxx` 逐 0 漂移，卡点解除；
  新暴露下一个 gap——ARM7 写 `0x04000180/81`（IPCSYNC）被当作未知 IO 忽略，留待 B2。
- **结果**：✅ 用户验收通过（2026-09-05）。

## 2026-09-05 · 21-B7 — Shared WRAM 按 WRAMCNT 双核切分

- **背景**：B1 把 0x03000000 / 0x037F8000 无条件映射给两核同一 32KB，真机则按
  WRAMCNT 低 2 位分权（默认 3=全给 ARM7，ARM9 直接启动后看不到 Shared WRAM）。
- **做了什么**：`bus_resolve` 的两个 Shared WRAM 分支改走新增 `bus_shared_resolve`：
  按 `bus->io->memctl.wramcnt & 3` 与 `active_is_arm7` 选择「内存指针 + 掩码」，
  换算沿用 melonDS 的 `addr & mask` 重复别名方式：
  - mode 0：ARM9 全 32KB；ARM7 改见自己的 ARM7 WRAM（主区=低 32KB、镜像=高 32KB）。
  - mode 1/2：双方各 16KB，且各自窗口在镜像区同样重复。
  - mode 3：ARM7 全 32KB，ARM9 读 0、写忽略（直接启动默认）。
  未分得的窗口继续遵守项目「读 0 / 写忽略」约定。
- **怎么验证**：`test_shared_wram` 先切 WRAMCNT=0 再验旧双向别名不回归；新增
  `test_wramcnt_split` 覆盖 mode 0/1/2/3 的双核可见性、镜像别名与 ARM7 WRAM 别名。
  `test_nds.exe` **587 项检查 0 失败**。
- **结果**：✅ 自测通过；真 ROM 3 条未知 IO 消除，双核轮询卡点留给 B8。

## 2026-09-05 · 21-B8 — ARM9 DTCM/ITCM + Main RAM 镜像双核别名

- **解码结论（修正 B6）**：B7 后 ARM7 轮询 `0x027FFFBC==0x7F`、ARM9 轮询
  `0x027FFF88/8C` 事件位，是 FFXII 自研信箱协议的正常握手。真正卡点是内存归属：
  0x027E0000-0x027E3FFF 被复位例程配成 ARM9 DTCM（`MCR c9,c1,0` = 0x027E000A，
  c1 bit16 使能），栈建在 DTCM 里；0x01FF8000 起 32KB 是 ARM9 ITCM，复位用
  0x02078020 描述表把系统例程拷过去。ARM7 拷贝到 0x027E0000 实际写 Main RAM
  镜像——真机/ melonDS 的 ARM7 都能命中 0x02000000-0x02FFFFFF 主存窗口，B6 的
  “仅 ARM9 可见”只是在掩盖 DTCM 缺失。
- **做了什么**：
  - `bus` 新增 `arm9_dtcm[16KB]`（CP15 可配置）与 `arm9_itcm[32KB]`（固定
    0x01FF8000）；`bus_resolve` 对 ARM9 先判 ITCM/DTCM，再走 Main RAM 镜像。
  - `bus_set_arm9_dtcm()` 提供 CP15 写后的映射同步；镜像分支去掉
    `!active_is_arm7`，ARM7 恢复访问 0x024-0x027 主存别名。
  - `tests` 的 21-B3 用例改为“ARM7 写镜像落主存”，新增 21-B8 用例覆盖
    DTCM/ITCM 隔离（594 项 0 失败）。
- **真 ROM 效果**：`--headless 2000000` 越过旧信箱死等；ARM9 跑到 0x02009EC0
  （打开中断），ARM7 首次调 SWI 0x0E；下一卡点是 BIOS/IRQ 高向量与 CRC16。

## 2026-09-05 · 21-B6 — Main RAM 无缓存镜像仅 ARM9 可见

- **背景/定位**：B5 后 ARM9 在约 cycle 463,554 弹回 0xE1C010B0（`LDMFD sp!` 后
  `BX r14`）。给 LDM trace 加 `base`/`pc_new` 输出，确认 LR 槽地址 0x027E3B34；再用
  总线写监视抓到污染者：**ARM7 的块拷贝循环（0x02380124-130）在约 cycle 141,868
  把 0xE1C010B0 写进该槽**，覆盖了 ARM9 保存的返回地址 0x02008370。
- **根因**：0x02400000-0x027FFFFF 是 ARM9 为了绕过缓存而设的 Main RAM 无缓存镜像，
  **ARM7 的内存图里没有这一段**。B3 曾把镜像无差别映射给两核，导致 FFXII 的 ARM7
  块拷贝落进真实 RAM、覆盖 ARM9 放在镜像区的栈。
- **做了什么**：`bus_resolve` 的镜像分支加 `!bus->active_is_arm7`——ARM7 视角读写该区
  读 0、写忽略（与真机一致）；`bus.h` 注释补“ARM9 专用镜像”说明；`exec.c` 的
  LDM/STM trace 增补基址与弹出 PC（bring-up 常备诊断）；删除临时写监视。
  `tests/test_nds.c` 增 2 项：ARM7 写镜像被丢弃、主存不被污染（用 FFXII 栈槽偏移
  0x3E3B34 验证）。
- **怎么验证**：`test_nds.exe` **559 项检查 0 失败**；真 ROM `--headless 20000000`：
  ARM9 不再弹 PC=0xE1C010B0，全程停在主存代码区 0x0200B8xx/0x020093A0；ARM7 进入
  0x037FC0xx 轮询循环不再跑飞。
- **遗留（B7 素材）**：ARM9 新增 3 条未知 IO（读 0x04000204/205、写 0x04000247=03），
  ARM7 停在轮询循环，待下一步排查。

## 2026-09-05 · 21-B3 — Main RAM 无缓存镜像（0x02400000-0x027FFFFF）

- **做了什么**：`bus.h` 新增 `BUS_MAIN_RAM_MIRROR_BASE 0x02400000`（注释说明是真机
  Main RAM 的无缓存别名区）；`bus_resolve` 增加一个分支：0x02400000 起 4MB 换算到
  **同一个 `main_ram` 数组**同偏移，写主区可从镜像读回、写镜像也可从主区读回
  （与 Shared WRAM 双别名同一套思想）。`tests/test_nds.c` 新增 `test_main_ram_mirror`
  注册为 `[case 21-B3]`：双向别名、FFXII 栈区 0x027E3F80 往返、首字节/末字边界、
  越界读 0。
- **怎么验证**：编译通过；`test_nds.exe` **552 项检查 0 失败**；真 ROM
  `--headless 20000000`：ARM9 从“弹 PC=0 后在零区漂移（旧终点 0x132612F8）”变为
  **全程停在主存代码区 0x0200B9xx**；trace 在 cycle 126,634 看到
  `PC=020008F8 insn=E8BD8010 LDM r13!, ...`，函数正常返回（修复前是弹回 0）。
- **遗留**：ARM7 仍在约 cycle 231,000 后按原轨迹跑飞（`BX r12 -> 0x038043C9` 未切
  Thumb，B4）；ARM9 停在 0x0200B9xx 附近循环，疑似等待 ARM7 握手结果，待 B4 重跑观察。
- **结果**：✅ 用户验收通过（2026-09-05）。
