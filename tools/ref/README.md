# 参考核（melonDS）对照工具

本项目用一个**独立构建的 melonDS 参考核**做行为对照的「oracle」：
本地模拟器的某个阶段到底是「跟真机一致」还是「自己跑偏了」，用它来判定。

## 为什么放在仓库里

21-B9yi(续27) 曾经把参考核源码 + 我们打的诊断补丁 + 参考帧缓存全部放在
`%TEMP%\melonds-ref`，结果被 Windows 的临时文件清理清掉，逐像素对照一度不可用。
因此**补丁版 harness 必须进版本库**（本目录），重建时从 melonDS 官方仓库重取源码，
再把 `ref_harness.cpp` 覆盖进 `src/`。

## 重建步骤

```powershell
# 1. 取源码（本仓库记录用的 commit：906e9eb）
git clone https://github.com/melonDS-emu/melonDS "$env:TEMP\melonds-ref"

# 2. 生成构建（关掉 Qt/OpenGL/JIT，留 GDB stub，release 级别即可）
cmake -G Ninja -S "$env:TEMP\melonds-ref" -B "$env:TEMP\melonds-ref\build-core" `
      -DBUILD_QT_SDL=OFF -DENABLE_GDBSTUB=ON -DENABLE_OGLRENDERER=OFF `
      -DENABLE_JIT=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo

# 3. 把本目录的 harness 放进 src/，并在 src/CMakeLists.txt 里加一个 refhead 目标
#    （add_executable(refhead ref_harness.cpp) + 链接 core/teakra）
Copy-Item tools\ref\ref_harness.cpp "$env:TEMP\melonds-ref\src\ref_harness.cpp" -Force

# 4. 构建
ninja -C "$env:TEMP\melonds-ref\build-core" refhead
```

> 本机工具链：`%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin`
> （cmake / ninja / gcc / g++ 都在里面）。

## harness 支持的环境变量

| 变量 | 作用 |
|------|------|
| `REF_FRAMES=N` | 跑多少帧（默认 6000） |
| `REF_STATS_EVERY=N` | 每 N 帧打印双屏统计（非黑像素数 + 均值 RGB），与本地 `--stats-every` 同口径 |
| `REF_KEY_FRAME/REF_KEY_MASK/REF_KEY_PERIOD` | 按键注入脚本，与本地 `--key-frame/--key-mask/--key-period` 一致（`(f-KEY_FRAME)%PERIOD<12` 为按下） |
| `REF_SHOT_EVERY=N` | 每 N 帧把两块 256×192 帧缓冲写成 `%TEMP%\ref_fb_<frame>.bin` |
| `REF_STOP_IDLE=1` | 恢复旧行为：命中游戏空闲任务（0x02009570-0x02009590）就提前退出 |
| `REF_GXHIST=1` | 进程结束前打印 GX 命令码直方图（`refgxhist:` 行），与本地 `NDS_GXHIST` 对照 |
| `REF_POLYDBG=1` | 帧号 ≥3800 后打印前 6 个顶点的**裁剪空间坐标**与矩阵（对应本地 `NDS_POLYDBG=1`） |
| `REF_GXDBG_FRAME=N` / `REF_MAT_FRAME=N` | 只 dump 第 N 帧窗口的**矩阵类命令序列** / 打印该帧的 `proj/pos/tex` 关键元素（对应本地 `NDS_GXDBG_FRAME` / `NDS_MAT_FRAME`） |

## melonDS 侧需要的补丁

`melonds-gpu3d.patch` 是对 melonDS `src/GPU3D.cpp` 的全部改动（`REF_GXHIST` /
`REF_POLYDBG` / `REF_GXDBG_FRAME` / `REF_MAT_FRAME` 四个诊断设施），重建时：

```powershell
git -C "$env:TEMP\melonds-ref" apply <仓库路径>\tools\ref\melonds-gpu3d.patch
```

## 把参考帧转成图片

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\ref\fb2bmp.ps1 `
           -In "$env:TEMP\ref_fb_4000.bin" -Out build\ref_4000.bmp
```

（顶屏在上、底屏在下，和本地 `--shot-every` 出的 BMP 版式一致，可直接目视并排比较。
脚本保持纯 ASCII —— Windows PowerShell 5.1 按 ANSI 读 `.ps1`，中文注释会解析失败。）

## 已知口径差异（对照时必须记住）

* `GPU.GetFramebuffers()` 返回的是 **32 位/像素**、且是**加 MASTER_BRIGHT 之前**的
  合成结果；本地 `render_frame()` 是最终的 24 位画面。做 RGB 均值/逐像素对照时，
  本地要配 `NDS_NOMB=1` 才与之同口径。
* `906e9eb` 这一版 master 与「历史那份参考核」从 f=40 起就不一样（见
  `docs/21-rom-bringup.md` 续29），所以它只能做**结构性/早期帧**对照，
  不能当作后期帧的逐像素判定依据。
