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
