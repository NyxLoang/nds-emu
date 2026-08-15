# ppu 模块日志

## 4.1 — docs/05-framebuffer.md

- 新增概念文档：像素与 RGB555/888、线性 framebuffer 的存法（`y*宽 + x`）。
- 自测题：`0x7C00`=红、`0x03FF`=青、(10,20) 下标=5130、5 位扩 8 位 = `v5<<3`。

## 4.2 — framebuffer 约定

- 定稿：顶屏 = VRAM 起始 `0x06000000` 起 256×192 RGB555；底屏 = VRAM + `0x18000`。
- 常量入 `ppu.h`：`PPU_SCREEN_W/H`、`PPU_VRAM_TOP_OFFSET`、`PPU_VRAM_BOTTOM_OFFSET`。
- CPU 直接写这些地址（16 位颜色字），ppu 每帧读出来转 RGB888 上屏。

## 4.3 — ppu：缓冲转 SDL 纹理

- 新建 `src/ppu/ppu.h` / `ppu.c`。
- `ppu_t`：持有 `nds`、渲染器、顶/底屏两张 RGB888 纹理与两块主机缓冲。
- `rgb555_to_888()`：`R/G/B 5 位 << 3` 扩成 8 位，alpha 置 0xFF。
- `blit_vram_to_fb()`：按 `addr = 0x06000000 + 屏偏移 + (y*256+x)*2` 用 `bus_read16` 逐像素读。
- `ppu_render()`：读 VRAM → 转 RGB888 → `SDL_UpdateTexture` → `SDL_RenderCopy` 画在菜单栏下。
- `CMakeLists.txt` 加入 `src/ppu/ppu.c`。

## 4.4 — 顶屏测试图、底屏纯色

- `fill_test_pattern()`（主机直接写 VRAM）：
  - 顶屏：6 色横带（红黄绿青蓝品红，每 32 行一色）+ 最外一圈白边框。
  - 底屏：纯蓝 `0x001F`。
- 目的：先验证「VRAM → 纹理 → 窗口」整条管线，与 CPU 无关。

## 4.5 — 接 CPU：跑写 VRAM 的测试码再刷新

- `selftest_4_cpu_draw()`：一段手工汇编程序装入 Main RAM，让模拟 CPU 自己画图：
  - 顶屏：黄色十字（横线 y=96 整行 + 竖线 x=128 共 96 行）盖在 4.4 图案上。
  - 底屏：整屏绿色 `0x03E0`。
- 程序用 `imm8 ROR` 构造大地址（`0x06000000`、`+0x18000`、`+0xC000`、`+0x200`），用 `STR` 立即偏移写，`CMP`+`BNE` 循环终止，`B self` 停机。
- 新增 `exec_set_trace()`：批量 LDR/STR 循环前关掉逐条日志，避免数万行刷屏（默认开）。

### 修复：手工汇编的 3 个指令字编码错误

1. `ADD r4, r0, #0xC000` 误写 `E2844CC0`（Rn=4，等于 `ADD r4,r4,...`）→ 改 `E2804CC0`。
2. `ADD r5, r1, #0x18000` 误写 `E2855A18`（Rn=5，等于 `ADD r1,r5,...`）→ 改 `E2815A18`。
3. `STR r1, [r0]` 误写 `E5811000`（bit25=1 → 寄存器偏移，被当作未实现；Rn=1）→ 改 `E5801000`。
   - 现象：顶屏两条线不出现，底屏反而正常（`E5812000` 恰好是对的 `STR r2,[r1]`）。
   - 排查：临时把前 700 步打开逐条日志，看到 `STR r1, [r1+0]`，定位到位字段错位。

- 验证：回读 `top(x=128)=FF00`（黄）、`bot(0,0)=03E0`（绿），程序 99212 步到 `B self`。

## 4.6 — 简单缩放（2×）

- 复用 window 菜单已有的 scale 机制：启动时 `window_set_scale(2)` 默认 2×。
- 菜单下拉可切 1x/2x，双屏比例不变（512×824 窗口）。

## 阶段 4 验收

- 顶屏/底屏可区分（顶屏测试图+黄十字、底屏纯绿）。
- 不再靠主机 memset——画面由模拟 CPU 写 VRAM 产生。
- 窗口默认 2× 缩放，双屏仍按 256:192 比例。

## 修复：十字缺下半段 + 横竖粗细不一致（2026-08-09）

窗口 demo 检查发现十字两个缺陷，根因都在 `drawprog` 手工汇编：

1. **竖线缺下半部分**：竖线终点 `r4 = 0x06000100 + 0xC000 = 0x0600C100`，
   循环每步 `r0 += 0x200`（下一行），所以只画到 y=95 就停，缺 y=96..191 下半屏。
   改为 `ADD r4, r0, #0x18000`（终点 `0x06018100`，越过顶屏最后一行 y=191），
   竖线完整覆盖 y=0..191。新编码 `E2804A18`（`0x18 ROR 20`）。
2. **横竖粗细不一致**：横线只在 y=96 画 1 行（1px 高）；竖线画 x=128、129
   两列（2px 宽）。因为 STR 是 32 位写必然覆盖 2 像素，竖线天生 2px；
   让横线也画两行（y=95、y=96）才对称。
   - 横线改为双行嵌套循环：外层 `MOV r2,#2` 行数、内层 `MOV r3,#0x80` 每行 128 字；
     内层循环结束时 `r0` 已自动指向下一行行首（每行恰 0x200 字节），无需再调地址。
   - 新增指令编码：`ADD r0,r0,#0xBE00`=`E2800CBE`（`0xBE ROR 24`，定位到 y=95）、
     `MOV r2,#2`、`MOV r3,#0x80`、`SUBS r2/r3,#1`、外圈 `BNE` 到内层循环。

- 验证：回读 `top(x=128,y=0)=FF00`、`top(x=128,y=191)=FF00`（下半段完整）、
  `bot(0,0)=03E0`；程序 100117 步到 `B self`。
- `tests/test_nds.c` 同步更新并新增断言：竖线末行 `(128,191)`、横线两行 `(0,95)/(0,96)`、
  竖线两列 `(128,96)/(128,97)`、横线右端 `(255,95)`、十字外 `(10,10)/(10,190)` 全过。

## 阶段 9 — 真 2D PPU（弃假 FB，改寄存器驱动）

> 阶段 9 起，PPU 不再把 VRAM 当线性 framebuffer，而是读 DISPCNT/BGxCNT 寄存器，
> 走 tile+tilemap+调色板 / 直色位图 两条真实 2D 路径。为可测试性拆成两块：
> `render.h/.c`（纯逻辑，只依赖 bus，无 SDL）+ `ppu.h/.c`（SDL 纹理上传，复用 render）。

### 9.1 — 概念短文

- 新增 `docs/10-bg-tile-palette.md`：图层合成（BG/OBJ）、8×8 tile、tilemap（屏幕位置→tile 号）、
  调色板查表（索引→RGB555）、位图模式 vs tile 模式、关键寄存器地址（DISPCNT/BGxCNT/调色板 RAM）。

### 9.2 — 显示寄存器 + 调色板/OAM/VRAM 窗口

- 新建 `src/io/disp.h/.c`（io 功能文件）：`disp_t` 持主/副各一套 DISPCNT、4 个 BGxCNT、4 组滚动
  （HOFS/VOFS）；按字节读写 32 位 DISPCNT 与 16 位 BGxCNT/滚动（小端）。
- bus 侧新增内存区间：调色板 RAM `0x05000000`（2KB）、OAM `0x07000000`（2KB，主/副各 1KB）、
  三个 VRAM 固定窗口（副 BG `0x06200000`/主 OBJ `0x06400000`/副 OBJ `0x06600000`，各 128KB，
  对应 libnds vramDefault 的 bank C/B/D）。
- 验收：`test_disp_regs`——主副 DISPCNT/BGxCNT/滚动读写、调色板 RAM、VRAM 窗口映射，11 项全过。

### 9.3 — 直色位图模式

- `render_bitmap()`：BG mode 3-5 下，BG2/BG3 若为「扩展位图+直色」（BGxCNT bit7=1 且 bit2=1），
  把 VRAM 里 256×256 的 16bpp 位图左上 256×192 画到屏；位图基址复用 BGxCNT 屏幕基址字段 bit8-12
  （单位 16KB，真机 bitmap 语义）。
- 验收：`test_bitmap_render`——mode 5 写红/蓝两像素，渲染后 RGB888 正确、未写区黑。

### 9.4 — Mode 0 tile 图层

- `tile_pixel()`：取 tile 某像素调色板索引——4bpp 用 4 位面（8 行×4 字节，bit 位序 0=最左像素），
  8bpp 直接 1 字节 1 索引。
- `draw_bg()`：逐屏像素经 tilemap 找 tile → 取索引 → 查调色板（16 色按 tilemap 条目 pal_slot、
  256 色单一调色板）；索引 0=透明（露出已画更低层/背景色）；支持 hflip/vflip。
- `render_tiled()`：按优先级从低到高画各 BG（同优先级 BG0 最高）；最小实现 screen size=0、暂不支持滚动。
- 验收：`test_tile_render`——4bpp 像素(0,0)=索引1 显示红、其余透明露出绿背景；切 256 色同理。

### 9.5 — Engine B 对称

- `render_engine(bus, fb, is_sub)` 统一主/副：副引擎用 `IO_DISPCNT_SUB`/`IO_BGCNT_SUB_BASE`、
  副调色板 `0x05000400`、副 BG 图形窗口 `0x06200000`；字符/屏幕基址仅主引擎有效（副引擎为 0）。
- 验收：`test_engine_b`——副引擎直色位图 + tile 渲染各验一次。

### 9.6 — OBJ 最小（一个 sprite）

- `render_obj()`：遍历 128 条 OAM（每 8 字节一档，属性 0/1/2 = Y/X/尺寸/tile/调色板），
  按尺寸表（size×shape）算宽高，1D tile 映射（从左到右、从上到下线性），取 tile 像素查 OBJ
  调色板（`0x05000200` 主 / `0x05000600` 副）；透明索引 0 不画。最小实现：无旋转/缩放/翻转/半透明，
  OBJ 一律画在 BG 之上。
- **踩坑（测试）**：OAM 默认全 0 = 「8×8 方块在 (0,0)」的可见 sprite（真机行为），测试需先对
  128 条 OAM 统一置 bit9 禁用，再单独配置 entry 0。
- 验收：`test_obj_render`——16 色 OBJ 在 (20,10) 显示红、透明/屏外像素露背景；切 256 色显示绿。

### 9.7 — 自造数据 2D 场景 + main 演示迁移

- `tests/test_nds.c` 新增 `test_2d_scene`：顶屏 8bpp tile+tilemap+调色板 拼「红 tile0+绿 tile1+
  透明底」+ 叠一个 OBJ 蓝方块；底屏副引擎 8bpp tile 填纯青。整条链路只经寄存器，无线性 FB 写入。
- `main.c` 删除旧 `fill_test_pattern`/`selftest_4_cpu_draw`，改 `setup_2d_demo`：顶屏 8bpp 棋盘格
  （4 色）+ OBJ 白方块，底屏副引擎 8bpp 竖条纹（青/品红）。
- `ppu.h` 删除旧 framebuffer 常量（`PPU_SCREEN_W/H`、`PPU_VRAM_TOP/BOTTOM_OFFSET`）。
- 验收：148 项检查 0 失败（ctest 通过）。

### 19.4 — 3D 图层合成进 2D 顶屏

- `render.c` 新增 `render_3d(bus, fb)`：`DISP3DCNT` 使能位（bit13）置位时，把 3D 帧缓冲覆盖到主引擎（顶屏）。
  0 像素视作「无几何」背景保留 2D 输出，非 0 像素用其 RGB555 颜色覆盖（最小实现：不分优先级、无 alpha 混合）。
- `render_frame` 末尾调用（仅顶屏；副引擎无 3D）。`render.c` 引入 `io/io.h` + `gx/gx.h` 以读 `bus->io->gx`。
- 验收：`test_gx_layer`——3D 未使能时像素露 2D 黑背景、使能后三角形红覆盖到顶屏。
