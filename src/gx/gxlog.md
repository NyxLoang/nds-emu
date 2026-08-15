# gx 模块日志

> 覆盖：NDS 3D 几何引擎（Geometry Engine）。`src/gx/gx.h/.c` 纯核心实现（不依赖 SDL）。
> 吃「命令流」（矩阵/顶点/多边形属性），做矩阵变换 + 顶点变换（pos×proj → 透视除 → 视口映射），
> 输出屏幕空间三角形，由软件光栅化写入 3D 帧缓冲（256×192 RGB555），经 `DISP3DCNT` 使能位合成进 2D 顶屏。
> 寄存器：DISP3DCNT(0x04000060)、GXFIFO(0x04000400)、命令端口(0x04000440+cmd*4)、GXSTAT(0x04000600)。
> 注意：`0x04000400..0x040005FF` 与 ARM7 音频寄存器重叠，按访问者 CPU 分流（ARM9→几何，ARM7→音频），
> 由 `bus_write32`（ARM9 整字转发）+ `io_read8/io_write8`（DISP3DCNT/GXSTAT 按字节、gx 侧）路由。

## 19.1 — 短文 + 头文件

- **做了什么**：新增 `docs/20-3d.md`：几何引擎（GE）/渲染引擎（RE）分工、GXFIFO 40 位命令条目
  （8 位命令 + 32 位参数）、三种命令提交方式（unpacked / packed / 命令端口）、矩阵（1.19.12）与
  顶点（1.3.12）定点格式、图元类型、最小软件管线实现指南。
  新增 `gx.h`：寄存器/命令码/图元/定点常量、`gx_vertex_t`（屏幕坐标 + RGB555）、
  `gx_t`（DISP3DCNT/GXSTAT、pos/proj/tex 矩阵 + 栈、当前顶点属性、视口、顶点列表、3D 帧缓冲），
  声明 `gx_is_addr/gx_read8/gx_write8/gx_write32/gx_reset/gx_framebuffer/gx_transform_vertex/gx_raster_tri`。

## 19.2 — 寄存器 + 几何命令 FIFO + io/bus 接线

- **做了什么**：
  - `gx.c` 实现字节级 `gx_is_addr/gx_read8/gx_write8`（DISP3DCNT 4 字节 + GXSTAT/RAM_COUNT，
    GXSTAT 复位置 FIFO 空位 bit26，RAM_COUNT 简化读 0）。
  - 命令 FIFO 解码状态机：`gx_enqueue`（按命令码查参数表入队，0 参数命令立即执行）、
    `gx_feed_param`（喂参数给队头，满则执行）、`gx_flush`（弹队头执行）。
    `gx_write32`：`0x04000400..0x040043F` 为 GXFIFO 命令字——「无待填参数」时按低 4 字节拆成
    ≤4 条命令（0=NOP），「有待填参数」时该字是参数；`0x04000440..0x040005FF` 为命令端口——
    地址低字节编码命令码，写入值即该命令的唯一参数。
  - `io.h/io.c`：`io_t` 增 `gx_t gx`；`io_create` 里 `gx_reset`；`io_read8/io_write8` 增
    `gx_is_addr && !is_arm7` 路由（DISP3DCNT/GXSTAT），`snd_is_addr` 收紧为 `&& is_arm7`；
    新增 `io_gx_write32`。
  - `bus.c`：`bus_write32` 增几何区（`0x04000400..0x040005FF`）ARM9 整字转发到 `io_gx_write32`
    （拆字节会破坏 40 位命令语义）。
- **怎么验证**：`test_gx_fifo` 经 `bus_write32` 发完整命令流（矩阵→颜色→视口→BEGIN/VTX/END）出图；
  `test_snd_regs/mix/program` 切到 `active_is_arm7=1`（音频由 ARM7 控制）后仍全过。

## 19.3 — 几何命令：矩阵 / 顶点变换

- **做了什么**：`gx_exec` 按命令码分发：
  - **矩阵**：`MTX_MODE`(投影/位置/纹理选当前矩阵)、`MTX_IDENTITY`、`MTX_LOAD_4x4/4x3`、
    `MTX_MULT_4x4/4x3/3x3`、`MTX_SCALE`、`MTX_TRANS`、`MTX_PUSH/POP/STORE/RESTORE`（栈 32 层）。
    矩阵行主序存 `int64_t[16]`（1.19.12），装载按列主序，乘法 `r[i][j]=Σa[i][k]b[k][j] >>12`。
  - **顶点**：`COLOR/NORMAL/TEXCOORD` 记当前属性；`VTX_16/VTX_10/VTX_XY/XZ/YZ/VTX_DIFF` 提交顶点；
    `BEGIN_VTXS`（图元类型低 2 位）/`END_VTXS`（弹图元光栅化）；`VIEWPORT`（x1/y1/x2/y2 各 8 位）；
    `SWAP_BUFFERS` 简化 no-op（即时光栅化）。
  - `gx_transform_vertex`：clip=pos×proj → 齐次乘（w=0x1000）→ 右移 12 得 NDC →
    透视除（nw≠0，否则当正交）→ 视口映射（NDC(-1..1)→(vx1..vx2, vy1..vy2)，y 翻转 +1=顶）。
- **怎么验证**：`test_gx_transform`（单位矩阵 + 全屏视口下 (-1,-1)/(1,-1)/(-1,1)/(0,0) → (0,191)/(255,191)/(0,0)/(127,95)）。

## 19.4 — 软件光栅化 + 3D 图层合成

- **做了什么**：
  - `gx_raster_tri`：求包围盒夹到屏幕，逐像素用三条边函数同号（含 0）判定在三角形内，平色写入帧缓冲。
  - `render.c` 新增 `render_3d`：`DISP3DCNT` 使能位（bit13）置位时，把 3D 帧缓冲覆盖到主引擎（顶屏）；
    0 像素视作「无几何」背景保留 2D 输出，非 0 像素用 RGB555 颜色覆盖（最小实现：不分优先级/无 alpha 混合）。
    `render_frame` 末尾调用（仅顶屏，副引擎无 3D）。
- **怎么验证**：`test_gx_raster`（三角 (10,10)-(100,10)-(10,100) 内 (50,50)=红、外=0）；
  `test_gx_layer`（3D 未使能时像素露 2D 黑背景、使能后三角形红覆盖到顶屏）。

## 19.5 — 综合测试

- **做了什么**：`tests/test_nds.c` 增 `test_gx_transform`（8 项）、`test_gx_raster`（3 项）、
  `test_gx_fifo`（3 项）、`test_gx_layer`（2 项）；`test_snd_regs/mix/program` 改 ARM7 视角。
  累计 483 项检查 0 失败，`ctest` 100% 通过。
- **简化说明**：命令同步执行（不模拟 4 拍 FIFO 延迟）；平色着色（无光照/纹理采样/alpha 混合）；
  `SWAP_BUFFERS`/`RAM_COUNT`/`POLYGON_ATTR`/`NORMAL` 为占位或忽略。
