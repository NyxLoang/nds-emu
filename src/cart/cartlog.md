# cartlog

> 覆盖：卡带装载模块 `cart`，即 `src/cart/cart.h` / `src/cart/cart.c`。负责读 `.nds` 文件、后续解析 ROM 头并拷入内存。
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

