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
