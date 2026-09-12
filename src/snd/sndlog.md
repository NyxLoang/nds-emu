# snd 模块日志

> 覆盖：NDS 音频（16 通道 + 混音器）。`src/snd/snd.h/.c` 纯核心实现（不依赖 SDL），
> 按地址提供 16 通道寄存器（`SOUNDxCNT/SAD/TMR/PNT/LEN`）与主控（`SOUNDCNT/SOUNDBIAS`），
> `snd_render` 合成 32768Hz 立体声样本，供 SDL 回调（`main.c`）或测试直接断言。
> 寄存器地址落在 IO 区（`0x04000400..0x04000505`），由 `io_read8/io_write8` 转发到本模块。

## 18.1 — 短文 + 头文件

- **做了什么**：新增 `docs/19-audio.md`：16 硬件通道（PCM8/PCM16/IMA-ADPCM/PSG）、
  各通道寄存器布局、`SOUNDCNT/SOUNDBIAS` 主控、混音流程与采样率计算。
  新增 `snd.h`：通道/主控常量、`SOUNDxCNT` 位定义（音量/除法器/声相/占空比/循环/格式/启动）、
  `snd_channel_t`（寄存器 + 运行时游标 `pos` + ADPCM 解码器状态）、`snd_t`（16 通道 + 主控），
  声明 `snd_is_addr/snd_read8/snd_write8/snd_render`。

## 18.2 — 寄存器读写 + io 接线

- **做了什么**：
  - `snd.c` 实现 `snd_is_addr`（`0x04000400..0x04000505`）、按字节读写的 `snd_read8/snd_write8`
    （通道区按 `0x10` 步进取通道、通道内按偏移取 CNT/SAD/TMR/PNT/LEN；主控 SOUNDCNT/SOUNDBIAS 各 2 字节）。
  - `snd_write8` 写 CNT 时检测 bit31 沿：0→1 复位通道运行时状态（游标/ADPCM 解码器），1→0 即停止。
  - `io.h/io.c`：`io_t` 增 `snd_t snd` 字段；`io_read8/io_write8` 在 `snd_is_addr` 命中时转发到 `snd_read8/snd_write8`。
  - `CMakeLists.txt` 的 `ndscore` 加入 `snd.c`。
- **怎么验证**：`test_snd_regs` 写读回 SOUNDCNT/SOUNDBIAS + 通道 0/1 全套寄存器 + 越界读 0。

## 18.3 — 混音器

- **做了什么**：`snd_render(snd, bus, out_l, out_r, n)` 每采样遍历 16 通道，按格式解码：
  - **PCM8**：`(int8)byte << 2`；**PCM16**：`(int16)half >> 6`（都折到 10 位有符号域 ±0x200）。
  - **IMA-ADPCM**：首读头字（bit0-6 7 位有符号初始采样 `<<9`、bit8-15 步进索引），之后每个 4 字节数据字
    拆 8 个 4-bit nibble 依 IMA 步进表（`7..31`）与索引表解码；追赶游标保证按需解码。
  - **PSG**：方波（通道 8-13，8 步 × 占空比 duty+1）与白噪声（通道 14-15，确定性伪随机）满幅 ±0x200。
  - 音量（mul/127 ÷ 除法器 1/2/4/16）→ 声相（pan 0..127 分左右）→ 主音量（SOUNDCNT bit0-6）→
    加偏置（SOUNDBIAS bit0-9）并夹到 10 位 → 输出 16 位。
  - 游标定点 16.16 推进（`inc = master_clock<<16 / tmr / mix_rate`，tmr=0 视作 65536）；单发/手动到总长停 bit31，
    循环回绕（简化：回绕从头重放，PNT 循环点暂不实现）。
- **踩坑修复**：ADPCM 头初始采样符号扩展原为 `if (init>=0x4000) init-=0x8000`，对 7 位符号位（bit6）错位；
  改为 `if (init&0x8000) init-=0x10000`；游标越界比较用 `uint64_t` 防大长度 `<<16` 溢出。
- **怎么验证**：`test_snd_mix`（PCM8/PCM16/ADPCM/PSG 样本值 + 静音 + 主音量 0 + 单发停止）全过。

## 18.4 — SDL 音频回调（main 接线）

- **做了什么**：`main.c` 增加 `audio_callback`（`snd_render` 合成 → 交叠成 L/R 交替 int16 写进 SDL 流）、
  `audio_init`（`SDL_OpenAudioDevice` 32768Hz / S16SYS / 2ch，`SDL_PauseAudioDevice(0)` 开播）、`audio_shutdown`；
  经全局 `g_audio_nds` 让回调线程读到 `nds->io->snd` 与 `nds->bus`。`setup_audio_demo` 生成 64 采样 PCM8 方波
  （写入 Main RAM 空闲区）并配置通道 0 循环播放（tmr=2048 → 约 256Hz），验证「寄存器→合成→声卡」全链路。
- **简化**：回调线程与主线程访问 snd/bus 未加锁（单机演示可接受，真实并发保护后续补）。
- **怎么验证**：构建 + `ctest` 通过；运行 `nds-emu.exe` 可听到约 256Hz 提示音。

## 2026-09-12 · 21-B9wu — 无头模式按帧推进 SPU（游戏轮询播放状态不再死等）

- **证据**：FFXII 开场后 ARM7 的热点代码是 `0x037FEFD0-0x037FEFE8`
  （`ldrb r0,[0x04000403+idx*16]` → 取 SOUNDxCNT 高字节的通道状态位并轮询）。
  窗口模式下 SDL 音频回调会按实时速率调用 `snd_render` 推进通道游标；
  **无头模式没有回调**，通道永远停在 `start` 位，游戏就在这个轮询里出不来。
- **修复**：`snd_advance(snd,bus,samples)` 按“经过的样本数”渲染并丢弃结果，
  只推进通道游标/结束标志；`snd_set_host_render_active()` 让 audio.c 在打开
  SDL 设备时置位，runner 每帧在**没有宿主回调**时按 `tm.now/1024` 补推
  （NDS 32768Hz = 33.51MHz ÷ 1024）。
- 新增 `[case 21-B9wu]`：PCM8 单发通道 → `snd_advance(2048)` 后 start 位清零。

## 2026-09-12 · 21-B9xi — SOUNDBIAS 复位值与 10 位写语义

- **证据**：参考核 IO 快照（帧 1500/1900/2200）里 `0x04000504` 都读回 `0200`，
  本地是 `0000`；melonDS 侧该寄存器是 10 位、`Bias = val & 0x3FF`，
  混音公式 `(bias<<6)-0x8000` 在 `bias=0x200` 时正好抵消为 0。
- **修复**：新增 `snd_reset()`（通道清零 + `soundbias=0x200`，真机上电值，
  `io_create` 调用）；低字节写不再把高 2 位混进低字节（旧实现 `val & 0x03`
  会丢掉 bit2-7），高字节只保留 bit0-1，读回统一按 10 位掩码。
- **效果**：`--dump` 的 io7 快照 `SOUNDBIAS=0200` 与参考核一致。
