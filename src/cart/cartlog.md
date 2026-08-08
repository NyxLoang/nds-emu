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
