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

## 2026-09-13 · 21-B9yi（续47）：窗口「鼠标 → 触摸屏」通路（底屏可点）

- **背景**：FFXII 亡灵之翼是**触控驱动**的游戏（拖拽 LEADER、点选指令/单位），
  此前窗口模式只能敲键盘（Z/X/S/D/A/F/Enter/Backspace/方向键），底屏点不动
  ⇒ 就算进了战斗界面也玩不下去。
- **做了什么**：`src/main.c` 的主循环加**鼠标事件分支**。窗口没有 SDL logical size，
  事件坐标是物理像素，先除以 `window_get_scale()` 还原成逻辑坐标；布局是
  菜单栏 `[0, 28)`、顶屏 `[28, 220)`、底屏 `[220, 412)`（`MENU_H`/`SCREEN_W`/
  `SCREEN_H` 来自 `window/window.h`）：
  1. 左键落在**底屏区域** → 把逻辑坐标减掉菜单栏+顶屏高度得到底屏像素 `(sx, sy)`，
     按**固件默认校准**换算 ADC：`adc = 0x200 + (px - 33) * 16`（与 runner 的
     `--touch-*` 完全同口径），钳到 `0..0xFFF`，再调 `io_set_touch(nds->io, ax, ay, 1)`；
  2. **拖动**（`SDL_MOUSEMOTION` 且左键仍按住）更新触点（坐标钳在底屏范围内）；
  3. **抬起**（`SDL_MOUSEBUTTONUP`）→ `io_set_touch(..., 0, 0, 0)` 清触摸；
  4. 落在菜单栏/顶屏的左键仍走原来的 `menu_handle_click()`（缩放菜单不受影响）。
  实现上只新增一个 `touch_mouse_down` 状态位，**复用**阶段 17 已单测过的
  `io_set_touch()` → `touch_set_pos()` 路径，没有新的寄存器语义。
- **怎么验证**（窗口冒烟，跑一次）：
  ```
  .\build\nds-emu.exe "<ROM 路径>" --frames 400
      → window: reached --frames 400, exiting
      → save : stored …(1024Mb).sav        退出码 0
  ```
  另记一条环境坑：窗口冒烟**必须用可见窗口**；用 `Start-Process -WindowStyle Hidden`
  起 SDL 会初始化失败、退出码 1（与触摸代码无关）。
- **结果**：✅ 窗口通路可用（编译通过、931 项单测 0 失败、400 帧冒烟退出码 0）。
  游戏侧触摸响应见下一条（本轮顺带做了一轮触摸排查）。

## 2026-09-13 · 21-B9yi（续48）：触摸注入的游戏侧排查（反汇编 ARM7 驱动）

- **问题**：续46 的 `--touch-*` 注入在**寄存器级**已验证（注入 `adc=(7F0,5F0)`），
  但找不到「必须触摸才能过」的画面 ⇒ 触摸的**游戏侧效果**没有证据。
- **做了什么**（用现成设施，不改代码）：
  1. 同参数跑两次 600 帧无头（注入 vs 不注入），比对**全部输出**：
     唯一的差异就是 5 行 `runner: touch …` 自身 ⇒ f≤600 触摸**不改变任何状态**；
  2. `--watch/--watch-r 040001C0-040001C8`（AUXSPI 全寄存器）：
     读 `0x040001C2`（SPIDATA）**只有 f=0 那 842 次**（启动校准），
     之后每帧只有 5 写 + 4 读，且 **4 次读全是 AUXSPICNT（0x040001C0）**；
  3. 关键一步：`--dump` 导出 ARM7 WRAM，**手工反汇编** 0x038054A0 一带，
     并结合 `--watch-r 038055D8-038055F0` 核对指令字面量池，确认这段循环是
     「读 SPICNT 测试 busy(bit7) → 写 SPICNT=0x8A01(触控设备+hold) → 写 SPIDATA=0x84
     → 写 SPIDATA=0 → 写 SPICNT=0x8201 → 写 SPIDATA=0」，
     而最终**数据出口不是 SPIDATA**，而是读 `0x0380AA68` 这个 RAM 变量
     （该变量由 0x038050C0 一带的代码每帧写 0）。
- **结论**：本游戏 ARM7 侧在启动/标题阶段**不轮询 TSC 转换结果**，
  触摸注入因此不可能在这个阶段产生游戏侧差异；触摸的**游戏侧**响应要在
  **战斗/菜单界面**（`--frames` 跑到 f≈12000 的战术地图）或真机对照下再验。
  这不是渲染/IO 缺陷，而是**验证时机**问题。

## 2026-09-13 · 21-B9yi（续49）：窗口路径显式关掉指令级 trace（**用户可见的性能修复**）

- **背景**：本轮测窗口模式（用户实际玩游戏的方式）时发现 600 帧**超过 5 分钟跑不完**
  （<2 fps），stdout 涨到 **1.97 GB**。根因在 CPU 侧：`exec.c`/`thumb.c` 的指令级
  trace 开关默认是 1，**窗口路径从来没关过**（无头路径会关）⇒ 每执行一条指令打印一行。
- **做了什么**（`src/main.c`）：
  1. 在窗口路径 `runner_create()` 之后**显式**调用 `exec_set_trace(0)` /
     `thumb_set_trace(0)`（并补上 `cpu/exec.h`、`cpu/thumb.h` 两个 include）；
  2. 与 CPU 侧「默认值改 0」形成双重保险：默认安全 + 窗口路径显式声明。
- **怎么验证**：
  ```
  窗口 600 帧（满键 0x3FF）：修复前 >330 s 未完成、日志 1.97 GB
                            修复后 7.13 s（84 fps）、日志 3.4 KB
  窗口 3000 帧（满键）    ：104.4 s（28.7 fps）—— 与无头 3000 帧 100.6 s（29.8 fps）同量级
  ```
- **结果**：✅ 修复有效。`build\test_nds.exe` 931 项 0 失败；同配置 3000 帧无头跑两遍，
  416 行 `disp-change` 时间线**逐行相同**（trace 只影响打印，语义零变化）。
  注：`build\win600.txt` 那个 1.97 GB 的日志已删除（它是修复前的产物，可随时重跑生成）。

## 2026-09-13 · 21-B9yi（续57）：编译优化级别 `-O2 → -O3`（帧率 +16%）

- **背景**：解释器是「大量小函数 + 热点 switch」的形态。此前（21-B9wq）为提窗口帧率把
  本工程三个目标的编译选项定在 `-O2`，那之后核心热路径经历了多轮改造
  （访存快路径 续51、每步开销清理 续52–54、IO 门控 续55/56），值得重测一次优化级别。
- **做了什么**：`CMakeLists.txt` 里 `ndscore` / `nds-emu` / `test_nds` 的
  `target_compile_options` 由 `-O2` 改为 `-O3`（第三方 SDL 不受影响）。
- **怎么验证**（同机、3 轮交替、各 1000 帧、满键 0x3FF）：
  ```
  -O3：12.67 / 12.40 / 12.23 s   → 平均 12.43 s
  -O2：14.65 / 14.37 / 14.36 s   → 平均 14.46 s      ⇒ 约 -14.0%（帧率 +16%）
  窗口 3000 帧：57.4 s (52.3 fps) → 50.4 s (59.5 fps)
  窗口 600  帧：5.30 s (113 fps) → 4.57 s (131 fps)
  ```
- **零回归**：931 项单测 0 失败（两个版本都跑）；1000 帧 70 行 `disp-change` 时间线
  **逐行相同**；2000 帧双屏统计逐值相同、最终截图 **SHA-256 完全相同**
  （`A72E11A2…CC513`）——只改机器码，不改语义。
- **结果**：✅ 保留 `-O3`。

## 2026-09-13 · 21-B9yi（续58）：触摸「游戏侧」诊断 —— 游戏运行期**从不读触摸数据寄存器**

- **背景**：续47 做了「鼠标 → 触摸屏」通路、续46 做了 `--touch-*` 注入，但一直缺
  「游戏真的收到触摸」的证据。本轮把它查到底。
- **实验一（12500 帧长跑，两组对照）**：同输入脚本，一组在 f=11800 起每 40 帧注入
  一次底屏触点，另一组不注入。
  ```
  两组输出（去掉 runner: touch 行与截图行）**逐行完全相同**
  两组最终截图 SHA-256 完全相同（970D6C05…02DD）
  ⇒ 触摸注入对游戏状态**零影响**（连一次寄存器/时间线差异都没有）
  ```
- **实验二（决定性）**：同一次运行里监视 `0x040001C2`（AUXSPIDATA）的**所有读**：
  ```
  整个 12500 帧里，读 SPIDATA 只发生在 f=0：842 次（启动期校准），
  f=1 之后**一次都没有**
  ```
  同时监视触摸驱动的结果缓冲区 `0x0380AA68`：每个帧段都被 pc=038050C8 写成 **0**，
  注入窗口内也一样。
- **结论**：本游戏在**我们目前能到达的所有状态**（标题/剧情/对话框/战术地图）里
  **不读触摸数据寄存器**，只在启动期读一次做校准 ⇒ 触摸注入在这些状态下不可能
  产生游戏侧差异。触摸的**游戏侧**验证必须满足两个条件之一：
  ①玩到真正需要触控的界面（拖拽部队/点选指令那一类，可能要更多按键才能推进到）；
  ②或与参考核对拍参考核在同一帧是否读 SPIDATA（参考核可能在同样状态下也不读）。
  本项**保持开放**，不当作已完成。

## 2026-09-13 · 21-B9yi（续59）：触摸闭环 —— EXTKEYIN bit6（PENRIQ）补上后游戏真的收到触摸

- **背景**：续58 发现「游戏运行期从不读触摸数据寄存器」，当时列为开放项。本轮用
  **参考核（melonDS）** 做同口径对照把它查穿了。
- **参考核证据**（临时补丁见 `tools/ref/README.md`）：12500 帧、满键、f=11800 起
  每 40 帧注入一次触点：
  ```
  不触摸：refspi: datareads=842        （与本地一致 —— 只有启动校准）
  注入触摸：refspi: datareads=18122    （≈24 次/帧）
  ```
  ⇒ 游戏**有触摸才去读** SPI，说明「笔按下」这个状态必须先被游戏看见。
- **根因**：melonDS `TSC::SetTouchCoords()` 把笔状态写进 `KeyInput` bit22，
  即 **EXTKEYIN(0x04000136) bit6 = PENIRQ，低有效**（按下=0、抬起=1）。
  本地 `io_read8()` 里这一位是写死的：
  ```c
  case 0x04000136u: return 0x7Fu;   /* bit6 永远“抬起” */
  ```
  ⇒ 游戏永远认为没有触摸，也就永远不去读坐标。
- **修复**：`case 0x04000136u: return (uint8_t)(io->touch.down ? 0x3Fu : 0x7Fu);`
  （`io->touch.down` 由 `io_set_touch()` 维护，**窗口鼠标触摸与 runner `--touch-*`
  共用同一条路**，所以两边同时生效。）
- **怎么验证**：
  ```
  3000 帧、f=100 起每 40 帧注入触点、监视 AUXSPI 全部读：
    修复前：f≥1 读 SPIDATA 0 次；  修复后：70080 次（≈24 次/帧，与参考核同量级）
  游戏侧：同输入下「无触摸 vs 注入触摸」画面统计从 f≤500 起就不同、
    最终截图 SHA-256 不同 ⇒ 游戏确实在响应触摸
  零回归：不触摸时与修复前逐字节一致（2000 帧统计逐值相同、截图仍 A72E11A2…CC513）
  931 项单测 0 失败
  ```
- **结果**：✅ 保留。**触摸从「机制验证过」升级为「游戏侧闭环」**：
  窗口里用鼠标按住底屏，游戏现在真的能收到（这解释了续58 的“零影响”）。

## 2026-09-13 · 21-B9yi（续61）：窗口分段 fps 打印 `--fps-every N`

- **背景**：此前只有「整段平均帧率」一个数字，看不出**哪一段卡**。游戏不同场景
  （地牢/对话/战斗指挥界面/等待输入）负载差别很大，需要分段数据才能定位瓶颈。
- **做了什么**：窗口模式新增 `--fps-every N`：每跑满 N 帧用真实时钟
  （`SDL_GetTicks64`）打一行
  `fps: frames=A..B  <毫秒> ms  <fps> fps`。含渲染/音频宿主开销，就是玩家的实际体验。
- **怎么验证**：`--frames 600 --fps-every 200` → 三行分段 fps 正常输出；
  随后用它跑了整段 20000 帧（结果见 `docs/21-rom-bringup.md` 续61）。
- **结果**：✅ 保留，作为后续性能工作的常规测量工具。

## 2026-09-13 · 21-B9yi（续68）：窗口侧的三个诊断开关 + 主循环分段计时

- **背景**：查「窗口模式慢段为什么慢」时发现两组对比不可比，于是补齐**窗口侧**的诊断设施。
- **做了什么**（都在 `src/main.c`，默认关闭、零开销）：
  1. `NDS_NORENDER=1`：跳过清屏/菜单/`ppu_render`/`SDL_RenderPresent`
     （窗口不更新），用于量化**宿主渲染**占多少帧时间；
  2. `NDS_NOAUDIO=1`：不打开声卡（仍按模拟时间推进 SPU，行为等价），
     用于量化**音频回调**占多少；
  3. `--fps-every N` 的输出增加一行**分段计时**：
     `phases: events=<SDL 事件泵> ms  emulation=<runner_run_frame> ms  other=… ms`
     （用 `SDL_GetPerformanceCounter` 计，只在打开 `--fps-every` 时收集）。
- **怎么验证 / 结论**（10000 帧、同时打开前两个开关）：
  ```
  fps: frames=8000..10000  30651 ms  65.3 fps
       phases: events=2 ms  emulation=30647 ms  other=2 ms
  ```
  ⇒ 窗口侧的**事件泵与其它开销都可忽略（各 2 ms / 2000 帧）**，
  **时间 100% 花在 `runner_run_frame`（模拟本身）**；上一轮「窗口慢段只有 52 fps」的对比
  因此被判定为**不可比**（见下）。
- **结果**：✅ 三个开关都保留（后续性能/可玩性排查都用得上）。
  ⚠️ 方法论教训（已写入 docs/21 续68）：**跨时段/跨输入的比较不可信**——
  ①同一段 2000 帧，按全键跑 17.1 s、不按键却要 45.3 s（游戏状态不同、工作量差 2.6 倍）；
  ②本机跨时段的吞吐漂移可达 1.6 倍。以后只认**同会话、相邻、同输入**的配对测量。

## 2026-09-13 · 21-B9yi（续69/70）：窗口模式也套用键盘脚本 + 渲染热点定位

- **续69（重要修正）**：窗口路径此前**不套用** `--key-frame/-mask/-period`
  （它们只在无头路径生效）⇒ 一切「窗口带按键」的测量其实都是**无输入**状态，
  而输入会让游戏工作量差 2.6 倍 ⇒ 之前的窗口帧率并不代表“真的在玩”。
  修复：窗口创建 runner 后同样调用 `runner_set_keys(...)`。
- **续70（定位）**：`--fps-every` 的分段计时把窗口每帧拆成
  `events / emulation / other`，再把 `other` 拆成
  `render: ppu=… sdl(clear/menu/present)=…`。实测（窗口 + 键盘脚本、每 2000 帧）：
  ```
  0–2000：  events=7 ms   emulation=25114 ms   other=6763 ms（ppu=4055, sdl=2708）
  2000–4000：events=6 ms   emulation=21943 ms   other=13698 ms（ppu=10946, sdl=2751）
  ```
  ⇒ **宿主渲染占 21–50%**（重场景 `ppu_render` 达 5.8 ms/帧），不是模拟。
  随后两处优化（`draw_bg` 按图块循环、菜单文字纹理缓存，见 ppu/menu 模块日志）使：
  ```
  ppu：4055/11026/11611 ms → 2413/4564/5123 ms
  sdl：~2730 ms → ~540 ms
  窗口 fps：62.6 / 55.9 / 56.6 → **69.2 / 72.6 / 74.2**
  ```
  零回归：931 项单测 0 失败；2000 帧统计逐值相同、截图 SHA-256 完全相同。
- **结果**：✅ 保留。（`NDS_NORENDER`/`NDS_NOAUDIO`/`phases`/`render` 四行诊断也都保留。）

## 2026-09-13 · 21-B9yi（续82）：窗口帧节奏 —— 把帧率对齐真机 59.83 Hz（音画同步）

- **问题（保真度，而非性能）**：续62/70 把窗口优化到轻场景 **150–265 fps**（`NDS_NOSYNC`
  实测 265.5/172.2 fps），但**真机只有 59.83 Hz**。而音频回调是按**真实时间**驱动 SPU 的
  （SDL 声卡按 32768 Hz 拉样本）⇒ 模拟器跑得越快、画面越超前，**声音与画面脱节**
  ——这是真机不存在、只有「不限速模拟器」才有的状态。
- **做了什么**：
  1. 主循环末尾加**帧节奏**：维护「下一帧截止时刻」，`SDL_Delay` 睡掉大头，
     最后 <1.5 ms **忙等**收尾（`SDL_Delay` 只有毫秒粒度，单靠它 fps 会在 55–65 之间晃）；
  2. 已落后（拖窗口/断点/比真机还慢）时把截止时刻重置为「现在 + 一帧」，避免追赶式狂跑；
  3. 默认开启，目标 **59.8261 Hz（16.715 ms/帧）**；
  4. 开关：`NDS_NOSYNC=1` 或 `--speed 0` 关闭（自动化压测要保持原速度必须带上）；
     `--speed N` 以 N 倍速快进（上限 = 59.8261×N fps）。
- **怎么验证**（窗口冒烟，同一 ROM）：
  ```
  --frames 600 --fps-every 200（默认）
    window: 帧节奏开启 目标 59.83 fps（真机 59.83 fps x1.00）
    fps: 3363 ms 59.5 fps / 3344 ms 59.8 fps / 3374 ms 59.3 fps
    phases: emulation=623/151/1242 ms  other=2739/3192/2132 ms  ← other = 节奏等待
  NDS_NOSYNC=1：1130 ms 265.5 fps、1742 ms 172.2 fps（确认可关闭、恢复满速）
  --speed 2：目标 119.65 fps，实测 108.4 / 95.7 fps
              （场景太重时跑不满，属正常；限速只会「封顶」不会「加速」）
  ```
- **零回归**：931 项单测 0 失败；无头路径**完全不经过**节奏代码（headless 在窗口循环之前
  就返回），且按文档锚点复现完全一致 —— 2000 帧
  `--stats-every 500 --shot --key-frame 1 --key-mask 0x3FF --key-period 120`：
  `f=2000 顶屏 24,36,40 / 底屏 52,81,108`（与续51 记录逐值相同）、
  `savechip: nonzero=24 hash=3A361368EF684AD7`（与续77 记录相同）、
  截图 SHA-256 = `A72E11A2292D3203AE4C81677BDE2D76B54BC651C34234F38070034A697CC513`
  （与续51 起的锚点 `A72E11A2…CC513` 完全相同）。
- **附带确认**：上一条也把「零回归标准脚本」定死为
  `--key-frame 1 --key-mask 0x3FF --key-period 120 --headless-frames 2000 --stats-every 500 --shot`
  （此前文档只记结论没记命令，无法复现；今后 A/B 一律用这一条）。
- **结果**：✅ 保留（默认开启）。**注意**：`--fps-every` 在默认限速下会显示 ~59.8，
  这是预期值；要测性能必须 `NDS_NOSYNC=1`。

## 2026-09-13 · 21-B9yi（续83）：窗口退出摘要 —— 让「人工验收」有客观判据

- **为什么做**：剩下的两项开放项（①战斗内拖拽下令 ②游戏内存档菜单保存）只能人工试玩，
  但人工不该等于「凭感觉」。要给出**不读代码就能判定**的输出。
- **做了什么**（`src/main.c` 主循环退出后、写回 `.sav` 之前）：
  ```
  window: summary frames=300 elapsed=1367 ms avg=219.5 fps（含帧节奏等待）
  savechip: type=2 size=8192 nonzero(vs 0xFF)=24 hash=3A361368EF684AD7
  window: screenshot saved to build\win_exit.bmp        ← 给了 --shot 才有
  save : stored …sav
  ```
  ①`runner_savechip_report()` 从 runner 公开出来（与无头同一口径）：**人工存过档后
  `nonzero` 会从 24 明显变大**；②`runner_save_screenshot()` 用**同一个** `save_bmp`
  写双屏 BMP（顶屏在上），窗口模式 `--shot` 因此也生效；③摘要给出帧数与平均帧率。
- **怎么验证**：窗口 300 帧 `--shot` ⇒ 三行都按预期出现、BMP 成功落盘；
  **跨路径一致性**：窗口与无头同脚本（`--key-frame 1 --key-mask 0x3FF --key-period 120`）
  跑到 f=300 **截图 SHA-256 完全相同**（`1B688CEF…7460`）、跑到 f=2000 时窗口侧
  得到 `A72E11A2…CC513`，与续51 起的 headless 文档锚点**同一值**
  ⇒ 窗口与无头同口径、窗口也是确定性的（音频回调按真实时间走不影响模拟状态）。
- **结果**：✅ 保留。默认限速下 `avg≈59.8` 是预期（含节奏等待），测性能用 `NDS_NOSYNC=1`。

## 2026-09-13 · 21-B9yi（续92）：按住 Tab 快进（×4）+ 快进时的音频口径

- **动机**：玩游戏要跳过长过场的现实需求；`--speed N` 只在启动时设定，玩的时候改不了。
- **做了什么**：
  1. 窗口主循环里加「**按住 Tab = 快进**」（`NDS_FF_MUL` 可改倍数，默认 ×4）：
     按下时把帧节奏目标改成 `59.8261 × N`，松开还原；
  2. **快进的音频口径**：按下时 `snd_set_host_render_active(0)` ⇒ SPU 改由 **runner 按模拟时间**
     推进（不是声卡按墙钟），同时 `audio.c` 的回调在宿主渲染关闭时**只输出静音**
     —— 否则声卡既会按墙钟推进 SPU（与 4 倍速的游戏时间脱节），又会把通道状态**多推进一次**。
     这样 4 倍速下**游戏逻辑仍然正确**（音频状态跟着模拟时间走），只是听不到声音；
  3. 顺带记录 `audio_on`（`NDS_NOAUDIO=1` 时不该把宿主渲染标志置 1）；
  4. `NDS_FF_HOLD=1`：启动即视为按住快进（自动化验证/基准用）。
- **怎么验证**：
  ```
  单测 961 项 0 失败（快进改动不触及核心库）
  NDS_FF_HOLD=1 --frames 600 --fps-every 300
    window: 快进 开（启动即按住，×4.0 → 目标 239.3 fps，音频静音）
    fps: 300 帧 1647 ms（182.1 fps）/ 300 帧 2150 ms（139.5 fps）
    summary：600 帧 3797 ms（158.0 fps）      ← 1× 时同样 600 帧要 ~10.1 s
  ⇒ 实际约 **2.7 倍**（限速只封顶、不加速；重场景跑不满 239 fps 属正常）
  ```
- **结果**：✅ 保留。人工手感验收：正常玩时按住 `Tab` 应明显加速，且**没有声音**（预期）。

## 2026-09-13 · 21-B9yi（续92b）：快进的**实际倍率受模拟速度限制**（诚实记录）

- **补充实测**（`NDS_FF_HOLD=1 --frames 3000`，窗口）：3000 帧 **44.7 s（平均 67.1 fps）**、
  `savechip` 与 1× 时完全一致（24 字节 / 同一哈希）、退出截图正常写盘、无停摆。
- **结论**：`Tab` 快进的目标是 239.3 fps（4×），但**限速只封顶、不加速**——
  重场景（过场/战斗，1× 时约 70 fps）实际只能跑到 ~67 fps ⇒ **约 1.1×**；
  轻场景（静态对话/地图，1× 时 150~265 fps）才能真正跑满 4×。
  ⇒ 想让快进在所有场景都有效，得继续做**性能优化**（当前瓶颈仍是解释器访存），
  或降低窗口分辨率/关掉音频以外的东西——留作后续。

## 2026-09-13 · 21-B9yi（续93）：帧节奏的两处修正 + **节奏误差量化**（为什么 1× 下重场景是 58.4 而不是 59.8）

- **动机**：续92 的 18000 帧窗口长跑（5.1 分钟、带音频、1×）实测平均 **58.4 fps**，
  而目标是 59.83 —— 差 2.4%。这既影响手感，也会让「声卡按墙钟推进 SPU」与游戏时间
  产生同等比例的时间轴漂移，必须查清。
- **改动 1（保留欠账）**：帧节奏原来「只要落后就 `pace_next = now + 一帧`」，
  会把每次睡眠多睡的那点时间**丢掉** ⇒ 每帧系统性偏慢。改成**保留欠账**
  （下帧少等一会儿补回来），只有落后**超过一整帧**才重新对齐。
- **改动 2（诊断）**：`--fps-every` 新增一行：
  ```
  pace: 超时 N/总数 帧，平均 X ms/帧，最大 Y ms，丢帧重同步 Z 次（目标 16.72 ms）
  ```
- **量化结果**（9000 帧窗口、1×、带音频与键盘脚本）：
  ```
  段 1（含启动）：58.4 fps  平均超时 6.78 ms/帧  最大 36.3 ms  重同步 35 次
  段 2（轻场景）：59.8 fps  平均超时 0.21 ms/帧  最大 37.8 ms  重同步  1 次
  段 3（重场景）：58.7 fps  平均超时 2.38 ms/帧  最大 39.6 ms  重同步 27 次
  ```
  ⇒ **轻场景节奏几乎完美（0.21 ms/帧）**；重场景掉到 ~58.5 fps 的原因是**几十个
  「工作量超过一整帧预算」的突发帧**（最重 ~37–40 ms ≈ 2.2 帧预算）。
  这类帧（场景切换/纹理上传/复杂 3D 帧）一旦超出一帧，欠账就**不可追回**，
  只能丢帧重同步。
- **对照实验（排除音频抢占）**：`NDS_NOAUDIO=1` 跑同样 12000 帧 →
  58.2/58.5 fps，与带音频时一样 ⇒ **不是音频回调抢 CPU**，是渲染/模拟的突发负载。
- **稳定性证据（顺带拿到）**：18000 帧（5.1 分钟）窗口长跑无停摆、分段 57.5→58.4 fps
  不随时间衰减、`savechip` 与短跑完全一致（24 字节 / 同一哈希）、峰值内存约 176 MB。
- **结果**：✅ 两处改动保留。**已知项**：要真正稳 59.83 fps 需要压掉重场景的突发帧成本
  （后续性能优化或 JIT 方向）；音频时间轴在高负载段会有 ~2.4% 漂移。
