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
