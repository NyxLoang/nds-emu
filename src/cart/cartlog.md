# cartlog

> 覆盖：卡带模块 `cart`，即 `src/cart/cart.h` / `src/cart/cart.c`（读 `.nds` 文件、解析 ROM 头并拷入内存）、
> `src/cart/key1.*`（KEY1 安全区解密，阶段 14）、`src/cart/cartbus.*`（卡带总线 ROMCTRL/命令/数据端口，阶段 15）、
> `src/cart/save.*`（存档芯片 SPI 状态机 + .sav 持久化，阶段 16）。
> 按时间从旧到新记录。

## 2026-08-08 · 阶段 1.2 cart 读整个文件到缓冲区

- **做了什么**：
  - 新建 `src/cart/cart.h` / `src/cart/cart.c`：`cart_t`（`data` + `size`），`cart_load` 用 `fopen/fseek/ftell/fread` 读整份文件到堆缓冲，失败返回 NULL 并写错误信息；`cart_free` 释放。
  - `main.c` 接受 `argv[1]` 作为文件路径，启动时 `cart_load` 并打印 `cart: loaded <path> (<size> bytes)`；无参运行不装载。
  - `CMakeLists.txt` 加入 `src/cart/cart.c`。
- **怎么验证**：`cmake --build build` 通过；`.\build\nds-emu.exe tools\sdl2\SDL2-2.26.3.tar.gz` 打印文件大小（8464990 字节，与实际一致）。
- **结果**：✅ 通过。

## 2026-08-08 · 阶段 1.3 按偏移解析 ARM9 头字段并打印

- **做了什么**：
  - `cart.h` 新增 `cart_header_t`（`arm9_offset/entry/ram/size`）与 `cart_parse_header` 接口。
  - `cart.c` 实现小端 `read_le32`（`b0 | b1<<8 | b2<<16 | b3<<24`），按 `0x020/0x024/0x028/0x02C` 解析四字段；文件不足 0x30 字节返回 -1。
  - `main.c` 在装载后解析并打印 `arm9 offset/entry/ram/size` 四个十六进制值。
- **怎么验证**：构建通过；用 SDL2 包运行打印四个十六进制（非真 ROM，为数据字节，仅验证机制）。
- **结果**：✅ 通过。

## 2026-08-08 · 修复：支持中文/Unicode ROM 路径

- **做了什么**：
  - 根因：Windows 下 `main` 的窄字符 `argv` 受控制台代码页影响，中文路径变成乱码，`fopen` 打不开。
  - `cart.c` 重构：抽出 `cart_load_fp` 共用读取逻辑；新增 `cart_load_w`（`_wfopen` 宽字符打开，仅 `_WIN32`）。
  - `cart.h` 增加 `cart_load_w` 声明。
  - `main.c` 用 `CommandLineToArgvW` + `GetCommandLineW` 拿宽字符 `argv[1]`（保留普通 `main` 入口兼容 SDL2main），调用 `cart_load_w`，用完 `LocalFree`；`printf` 用 `%ls` 打印宽路径。
- **怎么验证**：用中文名真实 ROM（`tools\Z 最终幻想12…(1024Mb).nds`，112MB）运行，成功装载并打印：
  `cart: loaded … / arm9 offset=00004000 entry=02000800 ram=02000000 size=00078038`（ram=02000000 正是 Main RAM 基址，字段合理）。
- **结果**：✅ 通过。

## 2026-08-08 · 阶段 1.4 拆 arm9/arm7 功能文件 + 解析 ARM7 头

- **做了什么**：
  - 按模块结构规则拆分：新增 `arm9.h/.c`（ARM9 解析迁入）、`arm7.h/.c`（新增 ARM7 解析 `0x030/034/038/03C`），小端读取各文件内 `static` 自持。
  - `cart.h/.c` 精简为接口文件：`cart_header_t = { arm9_header_t arm9; arm7_header_t arm7; }`，`cart_parse_header` 编排调用两个子解析；外部只依赖 `cart.h`。
  - `main.c` 打印 ARM9 + ARM7 两组字段；`CMakeLists.txt` 加 `arm9.c`、`arm7.c`。
- **怎么验证**：构建通过；真实 ROM 运行打印两组字段：
  `arm9 offset=00004000 entry=02000800 ram=02000000 size=00078038`
  `arm7 offset=0007C200 entry=02380000 ram=02380000 size=000286B0`（ram=02380000 是 ARM7 Main RAM 基址，合理）。
- **结果**：✅ 通过。

## 2026-08-08 · 阶段 1.5 最小假 .nds

- **做了什么**：新建 `homebrew/make_fake_rom.py`（Python 生成小端假 ROM，头字段可核对 + ARM9 死循环 `0xEAFFFFFE` / ARM7 `BX lr` 占位），生成 `homebrew/mini.nds`（20992 字节）。
- **怎么验证**：`nds-emu homebrew\mini.nds` 打印头字段与构造值完全一致。
- **结果**：✅ 通过。

## 2026-08-08 · 阶段 1.6 ARM9 镜像拷入 RAM 缓冲区

- **做了什么**：`main.c` 声明 `static unsigned char arm9_ram[4MB]`（过渡，阶段 2 换 bus），装载后 `memcpy` 从 `cart->data + arm9.offset` 拷 `arm9.size` 字节，并打印首 4 字节对照。
- **怎么验证**：假 ROM 拷贝后首字节 `FE FF FF EA`（即写入的死循环小端），与文件一致；真实 ROM 拷 491576 字节，首字节 `FF DE FF E7`。
- **结果**：✅ 通过。

## 2026-08-08 · 阶段 1.7 CLI 装载摘要

- **做了什么**：`main.c` 装载块整理为 `=== NDS cartridge ===` 结构：file 大小 + arm9/arm7 头字段 + image 拷贝行（首字节对照）。一条命令展示 1.3–1.6 全部内容。
- **怎么验证**：假 ROM 与真实 ROM 运行均输出完整摘要。
- **结果**：✅ 通过。

## 2026-08-15 · 阶段 14.1 安全区加密短文

- **做了什么**：新增 `docs/15-secure-area.md`：NDS 商业卡带安全区加密（KEY1 = 定制 Blowfish + 固定密钥表 + 三级子密钥派生 + `"encryObj"` 魔数校验）。纠正路线图「ARM9/ARM7 全加密」的说法：只有 ARM9 镜像前 0x800 字节安全区加密。
- **怎么验证**：能复述安全区位置、密钥表结构、level2/level3 派生差异与解密流程。
- **结果**：✅ 完成。

## 2026-08-15 · 阶段 14.2/14.3 KEY1 实现 + 安全区解密

- **做了什么**：
  - 新建 `src/cart/key1.h/.c` 功能文件：Blowfish 块加解密（`key1_encrypt64/decrypt64`，16 轮 + F 函数查 4 个 S 盒）、
    三级密钥调度（`key1_init_keycode` + `key1_apply_keycode`，keycode 就地加密 + P 数组 `^ bswap32(keycode[i%2])` + 整表重滚）、
    安全区就地解密/加密（`key1_decrypt/encrypt_secure_area`，level2 剥头 8 字节外层 + level3 解全 0x800 字节）。
  - 密钥表常量 `src/cart/key1_table.inc`：0x1048 字节（P 数组 18 字 + S 盒 4×256 字），
    由 `tools/gen_key1_table.py` 从 ndstool `encryption.cpp` 自动生成（= NDS ARM7 BIOS `0x30..0x1077`），
    首字节 `99 D5 20 5F` 与 melonDS/nds-bootstrap 一致。
  - `cart.h/.c` 增接口 `cart_decrypt_secure_area(cart)`：从头读 gamecode(`0x00C`)/ARM9 offset(`0x020`)，
    解密到临时缓冲、魔数校验通过才写回，避免误伤 homebrew。
  - `CMakeLists.txt` 加 `key1.c`。
- **怎么验证**：`ctest` 全绿；`test_nds.exe` 新增 24 项检查（块往返×三级、安全区往返、密钥表累加和 0x803BA、
  bswap32、自制加密 ROM 装载）全过。
- **结果**：✅ 通过。

## 2026-08-15 · 阶段 14.4 装载时解密安全区

- **做了什么**：`main.c` 在解析头后、拷贝 ARM9 镜像前调用 `cart_decrypt_secure_area(cart)`，
  就地解密 ROM 缓冲里的安全区，使拷进 Main RAM 的镜像已是明文；打印 `secure: KEY1 area decrypted` 或 `not encrypted`。
- **怎么验证**：`test_secure_area_load` 走完整流程（造加密 ROM → 解密 → 拷 RAM → 断言魔数 + entry PC）通过；`ctest` 全绿。
- **结果**：✅ 通过。

## 2026-08-15 · 阶段 15.1 卡带命令协议短文

- **做了什么**：新增 `docs/16-cartridge-protocol.md`：NDS 卡带总线寄存器（ROMCTRL/AUXSPICNT/CARD_COMMAND/CARD_DATA）、
  ROMCTRL bit23 DRQ / bit31 激活、命令 `B7`/`B8`（GetData 读 ROM）、DMA 卡带触发源（start mode=5）。说明真实游戏运行时
  按需流式读卡带（不靠一次性装载）。
- **怎么验证**：能复述寄存器地址、命令格式（大端）、读 ROM 的典型流程。
- **结果**：✅ 完成。

## 2026-08-15 · 阶段 15.2/15.4 卡带总线 cartbus + 接线

- **做了什么**：
  - 新建 `src/cart/cartbus.h/.c` 功能文件：`cartbus_t`（AUXSPICNT/AUXSPIDATA/ROMCTRL/cmd[8]/传输游标 + 借用 ROM 指针）、
    `cartbus_attach`（只借指针不拷贝）、`cartbus_is_addr`（`0x040001A0..AF`）、按字节读写控制寄存器、
    `cartbus_read32`（CARD_DATA 读 4 字节小端并自动 +4，越界返 0xFFFFFFFF）。
  - 命令解码：ROMCTRL 写 bit31（字节 3 的 bit7）锁存 `cmd[8]` 并激活；`B7`/`B8` 解析大端地址，块大小按 ROMCTRL bit24-26
    （0=默认 0x200、7=4 字节、其余 0x100<<(n-1)），置 DRQ（bit23）。瞬时传输模型：数据立即就绪。
  - `bus.c/h`：加 `BUS_CARD_DATA`（`0x04100010`），`bus_read32/write32` 整体转发到 `io_card_data_read32/write32`。
  - `io.c/h`：加 `cartbus` 字段 + `io_attach_cart`/`io_card_data_read32`；`io_read8/write8` 分发卡带寄存器；
    卡带刚激活时 `irq_set_card`（两核 IF bit19）+ `io_card_dma_check`（触发卡带 DMA）。
  - `main.c`：装载后 `io_attach_cart(nds->io, cart->data, cart->size)`，让游戏运行时能读卡带。
  - `CMakeLists.txt` 加 `cartbus.c`。
- **怎么验证**：`ctest` 全绿；新增 `test_cartbus_read`（命令读 + 越界返 0xFFFFFFFF）、`test_card_dma`（卡带 DMA 8 字搬 RAM
  + DRQ + 使能自清 + IF bit19）全过。
- **结果**：✅ 通过。

## 2026-08-15 · 阶段 15.3 DMA 补全：4 通道 + 地址控制 + 触发源

- **做了什么**：`src/io/dma.h/.c` 从单通道扩为 `dma_t`（4 通道 `ch[IO_DMA_COUNT]`）；新增地址控制
  （源/目的 增/减/固定，`DMA_CNT_SRC_DEC`/`DMA_CNT_DST_DEC` 等）、`DMA_CNT_REPEAT`、`DMA_CNT_IRQ`（暂不接线）、
  `DMA_CNT_MODE_SHIFT` 与 `DMA_START_VBLANK`/`DMA_START_CARD`；新增 `dma_fire(dma, bus, start_mode)` 供延迟触发源使用。
  `io_set_vblank` 现在会 `dma_fire(VBlank)`；`io_card_dma_check` 在卡带就绪时 `dma_fire(Card)`。
- **怎么验证**：新增 `test_dma_channels`（多通道独立 + 源递减 + VBlank 触发）全过。
- **结果**：✅ 通过。

## 2026-08-15 · 阶段 15.5 综合：CPU 程序 DMA 读卡带

- **做了什么**：`test_card_program` 用真实 ARM 指令设 DMA0（源=CARD_DATA 固定、目的=RAM、32 位、卡带触发、4 字）、
  预写 `B7` 命令、CPU `STR` 激活 ROMCTRL → 卡带就绪 → DMA 搬 4 字到 RAM，断言 PC 停机 + 数据一致 + 使能自清。
- **怎么验证**：`test_nds.exe` 全量 393 项检查 0 失败；`ctest` 100% 通过。
- **结果**：✅ 通过。

## 2026-08-15 · 阶段 16.1 存档类型短文

- **做了什么**：新增 `docs/17-save-memory.md`：NDS 存档芯片类型（EEPROM 512B/8K/64K/128K、Flash 256K-8M、FRAM 32K）、
  辅助 SPI 总线寄存器（AUXSPICNT `0x040001A0` / AUXSPIDATA `0x040001A2`）、AUXSPICNT 位定义（bit6 保持片选 / bit13 SPI 模式 /
  bit15 使能）、EEPROM/Flash 命令集与地址宽度、典型读写流程与 .sav 持久化要点。
- **怎么验证**：能复述寄存器地址、片选判定、命令流程与地址宽度差异。
- **结果**：✅ 完成。

## 2026-08-15 · 阶段 16.2 存档芯片 SPI 状态机（save）

- **做了什么**：新建 `src/cart/save.h/.c` 功能文件：
  - `save_t`：存档缓冲 + 芯片参数（大小/地址字节数/是否 Flash/写页/JEDEC ID）+ SPI 三态状态机（IDLE/收地址/读/写）。
  - `save_configure` 按类型分配缓冲并填 `0xFF`（擦除态）；`save_transfer(out)->in` 做一次 SPI 字节传输。
  - 命令：EEPROM/FRAM 的 WREN(06)/WRDI(04)/RDSR(05)/WRSR(01)/READ(03)/WRITE(02)；Flash 的 RDID(9F)/FAST READ(0B)/
    PP/PW(02/0A)/SE(0D8)/PE(0DB)/CE(C7/60)/DP(B9)/RDP(AB)。
  - 地址宽度按类型（512B 的 A8 来自 opcode bit3、8K/FRAM 2 字节、64K/128K/Flash 3 字节）；
    Flash 写用「与」语义（只能 1→0）、擦除填 `0xFF`；EEPROM/Flash 写按页回绕。
  - `cartbus` 接上 AUXSPI：写 AUXSPIDATA 低字节触发 `save_transfer`（仅当 AUXSPICNT bit13+bit15 选中存档芯片），
    bit6（保持片选）=0 时传完自动 `save_reset_cmd`；AUXSPICNT 撤片选也复位命令。
  - `io.c/h`：`io_attach_save`（配置类型）、`io_get_save`（取芯片指针供持久化）；`io_destroy` 释放存档缓冲。
  - `CMakeLists.txt` 加 `save.c`。
- **怎么验证**：`test_save_eeprom`（WREN/RDSR/WRITE/READ 往返）、`test_save_flash`（RDID/PE 擦除/PP 写/AND 语义）、
  `test_save_spi_regs`（经 AUXSPICNT/AUXSPIDATA 总线读写）全过。
- **结果**：✅ 通过。

## 2026-08-15 · 阶段 16.3 存档持久化（.sav）

- **做了什么**：`save_load_file/save_save_file`（及 `_w` 宽字符版）用 `fopen/_wfopen` 读写整块存档；
  装载时先填 `0xFF` 再读（文件不足保持擦除态）、文件不存在视为全新存档。
  `main.c` 加 `make_save_path_w`（ROM 路径换 `.sav` 后缀）；启动装载 ROM 后 `io_attach_save(EEPROM_8K)` +
  `save_load_file_w` 读回进度；退出前 `save_save_file_w` 写回（在 `nds_destroy` 释放缓冲之前）。
- **怎么验证**：`test_save_persist`（写 .sav → 破坏内存 → 重载 → 读回一致）全过；`ctest` 100% 通过。
- **结果**：✅ 通过。

## 2026-08-15 · 阶段 16.4 综合：CPU 程序读写存档

- **做了什么**：`test_save_program` 用真实 ARM 指令（STRB/LDRB）经 AUXSPICNT/AUXSPIDATA 走完整流程：
  选片 → WREN → WRITE(0x20)=0xAB → 撤片选 → 选片 → READ(0x20) → LDRB 收数据到 r3 → 停机；
  断言 PC 停机、r3=0xAB、芯片缓冲 `data[0x20]=0xAB`。
- **怎么验证**：`test_nds.exe` 全量 423 项检查 0 失败；`ctest` 100% 通过。
- **结果**：✅ 通过。

## 2026-09-06 · 21-B9k — ROMCTRL bit31 块传输忙位

- FFXII 的卡带读取用 CPU 循环轮询 ROMCTRL：bit23(DRQ)=1 时从 CARD_DATA 读字，
  bit31(Block Start/Status)=1 期间持续读；旧实现激活命令后 bit31 永远保持写值 1，
  即使块读完也不回落，游戏在该循环空转。
- `cartbus.c` 修正为瞬时传输语义：命令激活 → DRQ=1 且 bit31=1（忙）；CARD_DATA
  每读一个字前进地址，块内最后一个字读完时 DRQ 与 bit31 同时清 0；无命令/越界
  读也清忙位。块大小由 ROMCTRL bit24-26 决定（含 4 字节块）。
- 验证：15.2 用例补「4 字节块读完 busy/DRQ 回落」断言；全量 **700 项检查 0
  失败**。真 ROM 该读块循环结束，ARM9 继续推进。

## 2026-09-06 · 21-B9w — 卡带芯片 ID（B8 命令）+ 容量 ID 口径
- 对照参考快照发现两处卡带 ID 口径错误：
  1. `main.c` 直接把 ROM 头 0x0C 的游戏代码 ASCII 当成卡带 ID 写入
     0x027FF800/0x027FFC00 系统表；melonDS `NDSCart::ParseROM` 实际用
     “补成 2 的幂后的 ROM 大小”推导芯片 ID，FFXII 这颗是 0x7FC2。
  2. `cartbus` 把 B7/B8 都当 ROM 读；melonDS 只有 B7 读 ROM，B8 直接返回
     ChipID（`CartCommon::ROMCommandReceive`）。0x02011840 发 B8 读芯片 ID，
     原来读回“AXFJ”让 0x02011FE8 走了错误的 service14 分支。
- 修复：`cartbus_attach` 按补幂容量算 `chip_id`；`cartbus_activate/read32`
  支持 B8 单字返回芯片 ID；`main.c` direct-boot 表复用同一公式。
- 验证：15.2 用例补 B8 芯片 ID 断言（0x400 字节 ROM → 0x100C2）；真 ROM
  跑过 0x0200AB70/0x02011D84 的 service11 文件读取路径，ARM9 开始加载
  Worldmap/Menu 等 bmd 资源（此前只发 service14 后即停）。

## 2026-09-06 · 21-B9z — B7 地址掩码/低地址重定向 + 尾部零填充
- 对照 melonDS `CartCommon`：B7 的地址要先 `& ROMMask`（补幂后容量-1），
  且请求 <0x8000 时重定向到 `0x8000 + (addr & 0x1FF)`；旧 cartbus 直接按
  命令地址读原始缓冲区。
- `PadToPowerOf2` 尾部是 0 填充；本地越界读此前返回 0xFF，改为 0x00。
- 15.2/15.4/15.5 假 ROM 放大到 0x10000，改用 0x8100 起始地址贴近真实命令。

## 2026-09-06 · 21-B9zc — B7/B8 周期就绪时序

- 激活命令后不再瞬时 DRQ：按 melonDS 公式计算首字延迟并逐步推进。
- 读完一个字后清 DRQ 并排下一次就绪；0x200 块边界补 gap 延迟。
- DMA 从 CARD_DATA 取数时也先推进卡带时钟到就绪。

## 2026-09-06 · 21-B9zo — ROMCTRL 块长按 melonDS 0x100<<n 修正

- 旧实现按 `0x100 << (blk-1)` 解块长（1=0x100、2=0x200 …）；melonDS
  `NDSCart::WriteROMCnt` 实际是 `datasize = 0x100 << datasize`
  （1=0x200、2=0x400 … 6=0x4000；7=4；0=None）。
- FFXII 的 B7 读命令 ROMCTRL=0xA1416657（块字段=1）被本地解成 0x100：
  读循环每块只写 64 字就绪，而目标指针仍前进 0x200，主存缓冲每隔 0x100 缺
  一段；修正后 0x0217F000-0x02190000 缓冲与参考 frame30 **逐字节一致**。
- 15.2/15.4/15.5 用例把 B7 块字段从 0 改为 1（0 在 melonDS 语义里无数据）；
  全量 **751 项检查 0 失败**。真 ROM 读块不再半块推进，ARM9 越过原 4.01M
  空闲点继续读卡带（仍会在 service11 三次后回空闲，下一步继续追唤醒/续读）。

## 2026-09-12 · 21-B9yi — ROMCTRL 真机语义 + 卡带侧预取 FIFO（START 之后卡死修复）

**现象**：按键离开标题后（第 1900 帧起）两核都停在 IRQ 模式、IPC FIFO 积压 16 条，
ARM9 卡在 `0x02011C54`——那是游戏「写卡带命令前先等 ROMCTRL 空闲」的轮询循环
（`ldr r2,[0x040001A4]; ands r2,r2,#0x80000000; bne .-8`）。此时 ROMCTRL
`A1C16657`：bit31（忙）与 bit23（DRQ，数据就绪）都是 1，但游戏不再读数据端口。

**三个口径与参考核不一致（melonDS 对照）**：

1. **bit31/bit29/bit23 对软件只读**：melonDS 写掩码 `0xFF7F7FFF`，
   且 `(ROMCnt & (~mask | 0x20800000))` 强制这三位保持旧值。旧实现把写入值
   原样落进寄存器，于是「写 1 就是忙」。
2. **只有 bit31 由 0 变 1 才启动新传输**（`xferstart = (val & ~ROMCnt) & (1<<31)`），
   且要求 `AUXSPICNT` 使能（bit15=1）且处于 ROM 模式（bit13=0）。旧实现
   「写高字节带 bit7 就重启」，会把上一笔还没读完的传输覆盖掉——游戏永远
   等不到 bit31 清零。
3. **卡带侧预取 FIFO（2 字）**：melonDS 用 `ROMTransfer_ReceiveData` 事件把数据
   **按周期取进 2 字 FIFO 并置 DRQ**，CPU/DMA 只是从 FIFO 取走；传输在
   「取数位置到达块尾 **且** FIFO 被读空」时才结束（`ROMDataCount == 0`）。
   旧实现是「CPU 读一次才取一次、读不满整块就不结束」。

**实现**（`cartbus.c/.h`、`io.c`）：

- ROMCTRL 写入改成「只读位保持 + 0→1 才启动 + AUXSPICNT 门控」；
- 新增 `data[2]` 预取 FIFO（`data_head/tail/count/late`）与 `xfer_len`：
  `cartbus_advance` 到点就把字取进 FIFO（首次延迟 `xfercycle*(cmddelay+4)`、
  之后每字 `xfercycle*(4[+gap2])`，与 melonDS 相同），`cartbus_read32` 从 FIFO
  取字（空时返回残留字，而不是旧实现的 0xFFFFFFFF-且不消费）；
- 传输结束（FIFO 读空且到块尾）清 bit31/bit23，并在 `AUXSPICNT` bit14 使能时
  由 `io_advance_cart` 挂两核卡带完成中断（`end_irq`）。

**验证（与参考核逐事件对照）**：

- 卡带命令流：前 **5388 次传输启动的命令/地址与参考核逐条一致**；
- 读数据流：本地前 **500,905 次 CARD_DATA 读取与参考核逐值一致**；
- 每块读满 128 字（`pos` 走到 512）后传输正确结束（对照参考核的
  `128 reads, last pos=512`）。

**仍未解决**：第 1900 帧附近本地仍会在该轮询循环里停住（参考核此时在正常
跑游戏逻辑）。已知线索：本地该段比参考核慢 ~8~13 帧后出现「连续两个 B8
芯片 ID 传输」的异常序列，随后某次 512 字节传输只读了 127 字就被放弃。
下一步从「ARM9 IRQ 是否在卡带读循环中途插入、打断现场与参考核是否一致」查起。

**21-B9yi（续）— 定位到「第一次分歧」的精确帧**：给两边的传输启动都加上
发起者 PC 后对照（`NDS_CARTLOG=1` 的 `WRA7` 行 / 参考核 `REF_CART_LOG`）：

- 参考核在第 1370-1400 帧的传输启动只有两个来源：块读函数 `pc=0201191C
  lr=0201190C` 与芯片 ID 助手 `pc=0201187C lr=02011854`，且每块恰好一次 B8；
- 本地在**第 1887 帧**出现「连续两次 B8」（芯片 ID 校验失败重试），
  紧接着**第 1888 帧**出现唯一一次来自 IRQ 上下文（`sp=027E39F8`）的
  `pc=02011C38 lr=02011C24`（游戏把 `[0x027FFE64]` 里存的 ROMCTRL 值写回），
  之后游戏就在该函数的「等 ROMCTRL 空闲」轮询里停住。

⇒ 分歧链是「芯片 ID 校验失败 → 重试 → 在仍有未读完的块传输时发起新命令 →
轮询 bit31 永远等不到」。参考核同段从未出现这条路径。下一步对照
「ARM9 VBlank/IRQ 的插入相位」（本地在卡死时停在 IRQ 模式 `cpsr=A00000D2`，
参考核同段始终在 System 模式 `cpsr=...1F`）。

**同轮附带的时钟口径修正**：卡带时钟此前固定「每条指令 1 个周期」，现改为
按**上一条指令实际消耗的周期数**推进（`io_advance_cart(io, is_arm7, cycles)`）；
实测读卡带进度不变（说明该阶段瓶颈是 CPU，不是卡带节奏），但口径与
melonDS 的系统时钟一致，保留。

### 21-B9yi（续12）— 逐事件对齐：卡带块的完整循环 + 节奏回到参考核

用两侧的**卡带寄存器写日志**（本地 `NDS_CARTIO`、参考核 `REF_IOLOG`）把每个
512 字节块的寄存器写序列逐条对齐后，卡带侧的机制终于完全清楚（两侧**逐事件相同**）：

```
[任务]  ITCM 0x01FF82C0 武装 DMA3：SAD=04100010、DAD=buf、CNT=AF000001
        AF00 = 使能 + 模式 5（DS 卡带）+ 32 位 + 源固定 + 重复位 bit25；CNT_L=1
[IRQ ]  0x02011B14 卡带完成处理器（每条 512 字节块完成触发一次）：
          0x02011B24 → 0x020096CC 停 DMA（CNT_H 先 0x8500 清模式位、再 0x0500 清使能）
          缓冲指针 +0x200、剩余长度 −0x200
          剩余 != 0 → 0x02011BE0 → 0x02011BF4 发下一条 B7 命令
                        （0x02011C4C 先等 ROMCTRL bit31 落 0，再写 0xA1=0xC0 与 8 字节命令）
          剩余 == 0 → 关 bit19 → 0x02011840 读芯片 ID（B8）
[卡带]  每 20 周期取 1 字进 2 字 FIFO → 拉 DRQ → 每个 DRQ 一次 CheckDMA → DMA 搬 1 字
[结束]  最后一个字被读走 → ROMEndTransfer → bit31 落 0 + IF bit19 → 下一轮
```

⇒ **`CNT_L=1` + 重复位（`AF000001`）是真机用法**：整段 512 字节靠「每字一次
`CheckDMA`」搬完；不是「一次 `Start()` 搬 `count` 个字」。本地旧实现按后者建模，
再叠加「瞬间推进卡带时钟」的 hack，于是同一笔传输只用参考核 1/4 的帧数跑完
（205 块/帧 vs 参考 51 块/帧），游戏侧「哪一块由谁搬」的计数随之错位。

**改动后的对照**：

| 观测量 | 改前 | 改后 | 参考核 |
|---|---|---|---|
| 每帧启动的 512 字节块数（f=1889/1890） | 206 / 205 | **52 / 52** | 50 / 51 |
| 前 931 条命令（帧 1886-1903） | 一致 | **逐条相同** | — |
| ARM7 `if7`（1880-1912） | `00080000`（bit19 恒挂） | `00000000` | `00000000` |
| 1904 帧之后（带按键时序） | 停在 `0x02011C50`（空闲任务侧） | **仍停在 `0x02011C50`**，但已在芯片 ID 读取路径（IRQ 模式、`lr=0x02011854`），IPC FIFO 积压 16 条 | 整块搬完 → 芯片 ID 读完 → 继续执行 |

**残留差异（下一步，机理已定位）**：参考核的块循环是「武装 → 发命令 →
**DMA 按 DRQ 把整块搬完** → 处理器停 DMA → 读芯片 ID」（带时间戳实证：
发命令 t=2132531477 → DMA 起搬 t=2132547836 → 停 DMA t=2132553118）；本地是
「武装 → 发命令 → **立刻**停 DMA」，于是该块剩 2 字留在 FIFO
（`rem=504/pos=8/fifo=2/late=1`）、ROMCTRL bit31 恒为 1，芯片 ID 读取
（`0x02011840` → `0x02011C4C` 先等 bit31 落 0）永远等不到。
⇒ 要修的是 **ARM9 任务/IRQ 的插入相位**（谁在何时停 DMA、为何本地提前一步），
不是卡带/DMA 的触发模型。
