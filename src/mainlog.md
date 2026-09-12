# mainlog

> 覆盖：工程骨架、SDL2 引入、窗口与主循环，即 `src/main.c` 与根 `CMakeLists.txt` 相关部分。
> 按时间从旧到新记录。

## 2026-08-08 · 阶段 0.1 工具链

- **做了什么**：本机无编译器/CMake。winget 安装 WinLibs（gcc 16.1.0 + cmake 4.4.1 + ninja，POSIX/UCRT），自动写入用户 PATH。
- **怎么验证**：`gcc --version`、`cmake --version` 输出版本号。
- **结果**：✅ 通过。注意：已运行的程序需刷新 PATH 或重启才能识别新命令。

## 2026-08-08 · 阶段 0.2 工程骨架

- **做了什么**：根 `CMakeLists.txt`（C11，`add_executable(nds-emu src/main.c)`）+ `src/main.c` 打印 `nds-emu`；`cmake -S . -B build` + `cmake --build build` 产出 `build/nds-emu.exe`。
- **怎么验证**：运行 `.\build\nds-emu.exe` 输出 `nds-emu`；清空 PATH 仍可运行（无动态依赖）。
- **结果**：✅ 通过。

## 2026-08-08 · 阶段 0.3 引入 SDL2（FetchContent 本地 URL）

- **做了什么**：
  - 原计划 FetchContent 从 GitHub 拉源码 → 直连 GitHub 不通（`git ls-remote` 无响应）。
  - 改为经代理 `ghfast.top` 下载 SDL 2.26.3 官方源码包至 `tools/sdl2/`，`FetchContent_Declare` 用本地 `URL` + `URL_HASH`（SHA256 校验），构建完全离线。
  - 链接 `SDL2-static` 与 `SDL2main`；`main.c` 用标准签名 `int main(int, char**)` 并打印 SDL 版本。
- **踩坑**：
  1. SDL 2.26.3 的 CMake 脚本声明兼容 <3.5，被 CMake 4.4 拒绝 → 设 `CMAKE_POLICY_VERSION_MINIMUM=3.5`。
  2. Windows 下 `SDL_main.h` 把 `main` 宏替换为 `SDL_main`，要求标准签名并链接 `SDL2main`。
- **怎么验证**：运行输出 `nds-emu (SDL 2.26.3)`。
- **结果**：✅ 通过。

## 2026-08-08 · 分支工作流与开发日志体系

- **做了什么**（无代码，纯工程管理）：
  - git 分支：`main`（受保护，仅 PR 合并）+ `develop`（默认分支，日常开发）；bug 修复从 develop 拉 `debug/*`。
  - 新增本模块日志体系：根 `DEVLOG.md` 索引 + `devlog/<模块>.md` 详情。
- **怎么验证**：`git branch -a`、`gh repo view --json defaultBranchRef`。
- **结果**：✅ 通过。

## 2026-08-08 · 拆分 window 与 menu 模块

- **做了什么**：
  - `main.c`（325 行）拆成三部分：`src/window/window.h/.c`（窗口/渲染器/缩放/退出）、`src/menu/menu.h/.c`（字体/文本/菜单 UI/语言），`main.c` 只留主循环编排与游戏区占位。
  - `CMakeLists.txt` 加入两个新源文件，并加 `target_include_directories(nds-emu PRIVATE src)` 统一 include 前缀。
  - 依赖方向单向：menu 的缩放变更经返回值通知 main → `window_set_scale`，window/menu 互不直接调用。
- **怎么验证**：构建通过；运行后缩放、语言、点击行为与拆分前一致。
- **结果**：✅ 通过。

## 2026-08-08 · 阶段 0.6 顶/底屏两种纯色

- **做了什么**：
  - `window.h` 新增单屏尺寸常量 `SCREEN_W 256` / `SCREEN_H 192`，`GAME_W`/`GAME_H` 改为引用（`GAME_H = SCREEN_H * 2`）。
  - `main.c` 游戏区占位从一块纯黑拆成两块纯色：顶屏蓝 `(24,90,200)`、底屏绿 `(24,160,60)`，各 `SCREEN_W × SCREEN_H`，随 scale 缩放。
- **怎么验证**：`cmake --build build` 通过；运行 `.\build\nds-emu.exe` 窗口菜单栏下方上下两屏颜色不同（上蓝下绿）。
- **结果**：✅ 编译通过（待用户运行确认）。

## 2026-08-09 · 阶段 6 中断/定时器/按键接入

- **做了什么**：
  - 主循环每帧跑完固定步数后 `io_set_vblank(nds->io)`：指令计数近似产生 VBlank（IF bit3）。
  - 最小 IRQ 检测：`io_irq_pending()` 首次为真时打印一次 `irq: IRQ pending (VBlank ...)`（不进异常向量）。
  - SDL 键映射：事件循环捕获 `SDL_KEYDOWN/UP`，`z/x/s/d/a/f/回车/退格/方向键` → NDS 键位，维护 pressed 掩码后 `io_set_keyinput`（组合根在 main.c，window/menu 不依赖机器）。
- **怎么验证**：`cmake --build build` + `ctest` 70 项检查 0 失败；运行 `nds-emu.exe` 看控制台 VBlank/IRQ 打印，按方向键/ABXY 验证按键映射。
- **结果**：✅ 编译与自动化测试通过（窗口按键行为待用户运行确认）。

## 2026-08-09 · 阶段 8 装载 ARM7 镜像 + 双核交错调度

- **做了什么**：
  - 装载 ARM7 镜像：与 ARM9 装载对称，把 ROM 头 ARM7 镜像逐字节经 `bus_write8` 写入
    ARM7 WRAM（`0x03800000`），`bus_read32` 读回验证，`cpu_reset(nds->cpu7, arm7.entry)` 指到入口。
  - 交错调度：主循环每帧 `steps_per_frame` 步按 `i%3==2` 跑 ARM7、其余跑 ARM9（约 2:1），
    每 60 周期打印一次双核 PC/cycles。
- **配套**：`make_fake_rom.py` 的 ARM7 镜像由 `BX lr` 占位改为 `B self` 死循环，让 ARM7 停在自己
  WRAM 里空转（与 ARM9 对称），避免跳到地址 0 空转。
- **怎么验证**：`ctest` 117 项检查 0 失败；运行 `nds-emu.exe ..\homebrew\mini.nds` 看到
  `image : loaded 128 bytes into ARM7 WRAM @ 03800000`、`cpu7 : reset PC=03800000`，主循环打印
  `ARM9 PC=02000800 | ARM7 PC=03800000`（两核各自死循环）。
- **结果**：✅ 编译与自动化测试通过。

## 2026-08-15 · 阶段 14.4 装载时解密安全区

- **做了什么**：`main.c` 在解析 ROM 头后、拷贝 ARM9 镜像前调用 `cart_decrypt_secure_area(cart)`，
  就地解密 ROM 缓冲里 ARM9 镜像开头的 0x800 字节安全区（KEY1），使后续 `bus_write8` 拷进 Main RAM 的
  已是明文；打印 `secure: KEY1 area decrypted (encryObj OK)` 或 `secure: not encrypted (homebrew) or decrypt failed`。
- **怎么验证**：`test_secure_area_load` 走完整装载流程通过；`ctest` 360 项检查 0 失败。
- **结果**：✅ 通过。

## 2026-08-15 · 阶段 15.4 装载后挂载卡带总线

- **做了什么**：`main.c` 装载 ROM 并解析头、解密安全区后，调用 `io_attach_cart(nds->io, cart->data, cart->size)`
  把 ROM 缓冲借给卡带总线，让游戏运行时能经 ROMCTRL/CARD_DATA 按需读卡带数据（真游戏流式读，不靠一次性装载）。
- **怎么验证**：`test_card_program`（CPU 程序设 DMA + 激活卡带命令读 ROM）通过；`ctest` 393 项检查 0 失败。
- **结果**：✅ 通过。

## 2026-08-15 · 阶段 16.3/16.4 存档装载与持久化

- **做了什么**：`main.c` 加 `make_save_path_w`（由 ROM 路径派生 `<rom>.sav`）；装载 ROM 后
  `io_attach_save(nds->io, SAVE_EEPROM_8K)` 配置默认存档芯片（EEPROM 8K，最常用；类型自动检测后续补），
  再 `save_load_file_w` 读回进度并打印 `save: loaded <path> (<size> bytes)`；退出循环后、`nds_destroy` 前
  `save_save_file_w` 写回 `.sav` 并打印 `save: stored <path>`。宽字符路径与 ROM 装载一致（支持中文）。
- **怎么验证**：`test_save_persist`（.sav 写读回）与 `test_save_program`（CPU 程序读写存档）通过；`ctest` 100% 通过。
- **结果**：✅ 通过。

## 2026-08-15 · 阶段 18.4 音频 SDL 回调接线

- **做了什么**：`main.c` 增加 `audio_callback`（`snd_render` 合成 32768Hz 立体声 → 交叠成 L/R int16 写 SDL 流）、
  `audio_init`（`SDL_OpenAudioDevice` S16SYS/2ch + `SDL_PauseAudioDevice(0)` 开播，全局 `g_audio_nds` 让回调读
  `nds->io->snd` 与 `nds->bus`）、`audio_shutdown`；`setup_audio_demo` 生成 64 采样 PCM8 方波写入 Main RAM 空闲区并
  配置通道 0 循环播放（tmr=2048 → 约 256Hz），在 `setup_2d_demo` 后调用；退出前 `audio_shutdown`。
- **简化**：回调线程与主线程访问 snd/bus 未加锁（单机演示可接受，后续补并发保护）。
- **怎么验证**：构建 + `ctest` 467 项检查 0 失败；运行 `nds-emu.exe` 可听到约 256Hz 提示音。
- **结果**：✅ 通过。

## 2026-09-05 · 工程：程序入口自动切 UTF-8 控制台

- **做了什么**：`main()` 开头、任何输出之前，在 Windows 分支调用 `SetConsoleOutputCP(CP_UTF8)`，
  把控制台输出代码页从默认 936(GBK) 切到 65001(UTF-8)，与源码/日志统一为 UTF-8 的字节输出对齐，
  修复 cmd/Windows Terminal 直接运行时中文日志乱码（无需每次手动 `chcp 65001`）。
- **怎么验证**：`cmake --build build --parallel` 编译通过；`test_nds.exe` 528 项检查 0 失败。
- **结果**：✅ 编译与自动化测试通过（终端中文显示待用户按验收步骤确认）。

## 2026-09-06 · 21-B9n — headless 双屏 BMP 截图诊断（`--screenshot`）

- bring-up 阶段需要“不开窗口也能看到 FFXII 画面进展”：`--headless N
  --screenshot` 在跑完后调用纯渲染器 `render_frame`，把顶屏/底屏各 256×192
  纵向拼成 24 位 BMP 存为 `headless_shot.bmp`（不依赖 SDL 纹理）。
- `runner.c` 增加 BMP 编码（BGR、行自下而上、4 字节对齐）；`main.c` 解析
  `--screenshot` 开关。无参数时行为与原来完全一致。

## 2026-09-05 · 21-B8 — 直接启动 0x027FFxxx 卡带信息表

- `main.c` 装载 ROM 后、写镜像前，按 melonDS `SetupDirectBoot` 口径把卡带头信息写进
  ARM9 主存系统表：0x027FFE00 起 0x170 字节完整 ROM 头；0x027FF800/0x027FFC00 两组
  「卡带 ID + 头 CRC + 安全区 CRC + 0x5835 签名 + 0xFFFF/0x0001」表。
- FFXII 的启动/overlay 代码会读 0x027FFE60、0x027FFC00 等位置；缺表会走不到正常
  初始化流程。headless 日志打印一行 `boot : direct-boot tables @ 027FFxxx written`。
- **验证**：594 项单测 0 失败；FFXII headless 越过旧信箱轮询后能读到卡带信息并继续
  boot 初始化（下一卡点见 docs/21-rom-bringup.md 21-B8）。

## 2026-09-06 · 21-B9w — direct-boot 表的卡带 ID 按补幂容量推导
- 旧实现把 ROM 头 0x0C 的游戏代码 ASCII 写进 0x027FF800/0x027FFC00，
  参考快照是 melonDS 按“补成 2 的幂的 ROM 大小”算出的芯片 ID（FFXII=0x7FC2）。
  修正为与 `cartbus` 同一公式：`0xC2 | ((size>>20)-1)<<8`（1MB..128MB）。
- FFXII 0x02011FE8 会把 CARD_DATA 读回的芯片 ID 与 0x027FFC00 比较；
  旧表导致它走到 service14 错误分支，修正后进入 service11 文件读取流程。

## 2026-09-06 · 21-B9wn — 无头脚本按键与 IRQ 计数诊断

- `main.c`/`runner.c` 增加 `--key-frame N`、`--key-mask M`、`--key-period P`
  三个可选参数：从第 N 帧起注入按键，保持 8 个调度迭代后释放；period 非 0
  时每 P 帧再按一次（`0x8` = START）。事件驱动帧数跳跃时也能被游戏读到。
- 事件 headless 结束摘要补两核 `irq_hle.log_count`：4 亿步/frame684 时
  ARM9≈3680 次、ARM7≈3514 次中断，VBlank 没有整帧缺失。
- 真 ROM 验证：标题段之后周期性按 START 能推进场景，输入链路在真实 ROM
  上可用；开场字幕比参考慢属周期成本模型问题，后续单独校准。

## 2026-09-06 · 21-B9wq — 帧驱动 runner 接入窗口模式 + -O2

- 新增持久化 `runner_t`（`runner_create/run_frame/run_to_frame/set_keys`）：
  把事件驱动 headless 的扫描线/VBlank 事件、周期成本、Halt/WFI 唤醒和
  定时器补偿封装成可复用调度器；`--headless-frames N` 与窗口模式共用。
- `main.c` 窗口循环不再“每帧固定 8 条指令 + 手动 io_set_vblank”，改为
  `runner_run_frame()` 每帧推进一个真实 VBlank 周期；输入/渲染路径不变。
- CMake 给本工程三个目标加 `-O2`（第三方 SDL 不动）：300 帧 headless 从
  约 5.5s 降到约 2.9s；1700 帧（到标题）约 93s，等价帧吞吐约 18fps
  （早期帧更轻），窗口模式已达到可交互速度量级。
- `--headless-frames 1700 --screenshot` 与旧 `--headless-cycles` 结果一致：
  frame1700、ARM9 PC=02085604、ARM7=0x1158、VRAM 非零 389114。

## 2026-09-12 · 21-B9wr — 脚本按键按帧保持 + headless-frames 进度行

- 脚本按键的“保持”单位从调度迭代改为**帧**：旧实现 `key_hold=8` 数的是
  调度循环次数，事件驱动里一次迭代可能跨很多帧，游戏（按帧轮询 KEYINPUT）
  有时读不到按下状态；现在记 `key_release_frame = 当前帧 + 8`，按帧释放，
  并在按下时打印一行 `runner: key mask=.... at frame=` 便于验收对照。
- `--headless-frames` 每 100 帧打印一行进度（帧号 + 两核 PC + DISPCNT），
  长跑（数千帧、数分钟）时能看到卡在哪一段，不必等结束摘要。

## 2026-09-12 · 21-B9ws — 直接启动的寄存器/栈口径（melonDS SetupDirectBoot）

- 装载完成后除 `cpu_reset(入口)`，再调 `cpu_direct_boot()`：
  ARM9 `r12/r14=入口、sp=0x03002F7C、sp_irq=0x03003F80、sp_svc=0x03003FC0`；
  ARM7 `r12/r14=入口、sp=0x0380FD80、sp_irq=0x0380FF80、sp_svc=0x0380FFC0`。
  参考核在跳卡带入口前就是这么设的；本地旧实现 sp=0，BIOS 低地址路径一旦
  真的压 SVC/IRQ 帧就会写到栈外。

## 2026-09-12 · 21-B9wu — `--dump` 现场导出 + 逐帧诊断行扩充

- `--dump <前缀>`：headless 结束后导出 `mainram / arm7wram / sharedwram /
  vram / itcm / dtcm` 六份原始镜像，以及两核视角的 IO 寄存器快照
  `_io9.bin / _io7.bin`（0x04000000-0x04000FFF，各 4KB），便于和参考 harness 的
  `ref_fNNN_*.bin` 逐字节 diff（本步就是靠它确认 ITCM 完全一致、DTCM 只差栈帧）。
- `--headless-frames` 每 100 帧的进度行补 `if9/if7`、IPC `cnt=..`、FIFO 深度、
  GX 命令/三角形计数；结束摘要补 `gx3d:` 一行（3D 帧缓冲非零像素、GXSTAT、
  DISP3DCNT、命令统计）。排查「画面不动」时不必再临时加打印。
- `--shot <路径>`：把结束截图写到指定文件（默认仍是 `headless_shot.bmp`），
  便于并行跑多个帧号做像素级对照（如 1500/1700/1900/2100/2300 同时跑，
  再和已验收的标题图逐像素比较）。
- 进度行再补 `sp9/lr9`：卡住时用 `lr9` 就能看出 ARM9 处在哪个等待例程
  （例如 `lr9=0200761C` = `0x0200760C` 的“关中断+无限 WFI”深睡例程）。
- VBlank 事件改到 **第 192 扫描线**（`runner_ev_line` 里 `vcount == 192`），
  帧边界改调 `io_frame_boundary()`（VCOUNT 归零 + 清 VBlank 标志）；
  旧的 `--headless` 步进路径同样处理。

### 21-B9yi（续42）— 窗口模式冒烟：`--frames N` + 音频子系统初始化 + 存档路径打印

**背景**：用户实际是在**窗口模式**下玩，但此前没有一条自动化路径能验证「窗口模式
能不能正常起、出图、放声音、写存档」。本轮补上并发现两处真实缺陷。

**改动**：

1. **`--frames N`**：窗口模式跑满 N 帧后自动退出并打印一行（冒烟测试/自动化用，
   不必人工关窗口）。
2. **音频子系统从未初始化**（真实缺陷）：`window_init()` 只 `SDL_Init(SDL_INIT_VIDEO)`，
   `audio_init()` 直接 `SDL_OpenAudioDevice()` ⇒ 实测报
   `audio: SDL_OpenAudioDevice failed: Audio subsystem is not initialized`
   ⇒ **窗口模式全程没有声音**。改为在 `audio_init()` 里按需
   `SDL_InitSubSystem(SDL_INIT_AUDIO)`（已初始化则跳过），关机时只退出自己初始化的那个。
3. **存档路径打印截断**（真实缺陷，观感问题）：`printf("%ls")` 在已切 UTF-8 的控制台上
   遇到中文 ROM 名会在第一个中文处截断，看起来像「路径被吃掉」（实际 .sav 写对了）。
   改成先转 UTF-8 再 `%s` 打印。

**怎么验证**（窗口模式冒烟，各跑一次）：

```
--frames 400： save : loaded tools\Z 最终幻想12 …(1024Mb).sav (8192 bytes)
               audio: device opened 32768 Hz, 2 ch, format=32784      ← 修复前这里是失败
               window: reached --frames 400, exiting
               save : stored tools\Z 最终幻想12 …(1024Mb).sav        ← 修复前打印被截断
退出码 0；tools/ 下的 .sav 用**完整文件名**写回（实测时间戳更新）
```
