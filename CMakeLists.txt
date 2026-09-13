cmake_minimum_required(VERSION 3.16)
project(nds-emu C)

# SDL 2.26.3 的 CMake 脚本仍声明支持 CMake < 3.5，CMake 4.x 默认拒绝；
# 显式允许用 3.5 策略继续配置（仅影响第三方依赖，不影响本项目）
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(SDL2
    URL ${CMAKE_CURRENT_SOURCE_DIR}/tools/sdl2/SDL2-2.26.3.tar.gz
    URL_HASH SHA256=AF0FF86E4A268BC12C915A0EA19EA1A16419AB1426D92BAD5F4E26EB31583967
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
# SDL 同时会建 main 重定义等机制；我们只要静态库本体
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL2)

FetchContent_Declare(SDL2_ttf
    URL ${CMAKE_CURRENT_SOURCE_DIR}/tools/sdl2_ttf/SDL2_ttf-2.24.0.tar.gz
    URL_HASH SHA256=0B2BF1E7B6568ADBDBC9BB924643F79D9DEDAFE061FA1ED687D1D9AC4E453BFD
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
# 关闭 SDL_ttf 的可选依赖与共享库；用内置 freetype（release 包已含 vendored 源码）
# SDL2_ttf 2.24 以 BUILD_SHARED_LIBS 决定库形态，须先强制为 OFF 以匹配静态 SDL2
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
set(TTF_SDL2_TEST OFF CACHE BOOL "" FORCE)
set(SDL2TTF_VENDORED ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL2_ttf)

# 核心库：不含任何 SDL 依赖，供模拟器可执行程序与单元测试共用。
# 测试程序只链 ndscore，不开窗口即可验证 CPU/内存总线/指令语义。
add_library(ndscore STATIC
    src/nds/nds.c
    src/bus/bus.c
    src/cpu/cpu.c
    src/cpu/exec.c
    src/cpu/thumb.c
    src/cpu/arm9.c
    src/cpu/arm7.c
    src/bios/bios.c
    src/bios/bios_arith.c
    src/bios/bios_mem.c
    src/bios/bios_decompress.c
    src/bios/bios_wait.c
    src/bios/bios_crc16.c
    src/bios/bios_snd.c
    src/bios/bios7_low.c
    src/bios/bios7_image.c
    src/bios/bios9_image.c
    src/io/io.c
    src/io/irq.c
    src/io/timer.c
    src/io/key.c
    src/io/dma.c
    src/io/fifo.c
    src/io/disp.c
    src/io/touch.c
    src/io/rtc.c
    src/io/memctl.c
    src/io/power.c
    src/io/math.c
    src/snd/snd.c
    src/gx/gx.c
    src/ppu/render.c
    src/timing/timing.c
            src/cart/cart.c
            src/cart/cartbus.c
            src/cart/save.c
            src/cart/arm9.c
            src/cart/arm7.c
    src/cart/key1.c
    src/state/state.c
)
# PUBLIC：include 路径随链接传递，nds-emu 与 test_nds 都能 #include "nds/..." 等
target_include_directories(ndscore PUBLIC src)

# 模拟器本体：核心库 + SDL 显示/菜单/音频宿主/演示/诊断部分
add_executable(nds-emu
    src/main.c
    src/audio/audio.c
    src/demo/demo.c
    src/runner/runner.c
    src/ppu/ppu.c
    src/window/window.c
    src/menu/menu.c
)
target_link_libraries(nds-emu PRIVATE ndscore SDL2-static SDL2main SDL2_ttf)

# 单元测试：统一测试入口（阶段 5.1）。
# 只链核心库、不碰 SDL，可直接运行也可被 ctest 收集。
enable_testing()
add_executable(test_nds tests/test_nds.c)
target_link_libraries(test_nds PRIVATE ndscore)
add_test(NAME nds_test COMMAND test_nds)

# 21-B9wq：本工程源码用 -O2（第三方 SDL 保持各自默认），提高窗口模式帧率。
# 21-B9yi(续57)：解释器是「大量小函数 + 热点 switch」的形态，试 -O3
# （更多内联/向量化机会）。是否保留以同机相邻配对 A/B 为准。
target_compile_options(ndscore PRIVATE -O3)
target_compile_options(nds-emu PRIVATE -O3)
target_compile_options(test_nds PRIVATE -O3)

# 21-B9yi(续54)：**细粒度阶段剖析**编译开关。
# `NDS_PROF=1` 的取指/译码/IO 分桶要在热路径上每步插 rdtsc，这些分支**默认构建里
# 也要花钱**（实测会吃掉微优化的收益：续53 的清理本来是正的，但被这几条分支抵掉）。
# 因此默认关闭；要剖析时单独配置一个构建目录：
#   cmake -S . -B build-prof2 -DNDS_PROF=ON && cmake --build build-prof2
option(NDS_PROF "启用细粒度阶段剖析（每步 rdtsc，开发期诊断用）" OFF)
if(NDS_PROF)
    target_compile_definitions(ndscore PRIVATE NDS_PROF_BUILD)
endif()

# 21-B9yi(续41)：**LTO**。解释器每执行一条指令都要跨 TU 调用一批小函数
# （`timer_advance` / `bus_read*` / `gx_advance` / `cartbus_advance` …），
# 关掉 LTO 时这些调用无法内联；打开后编译器可以跨文件内联/常量传播。
# 只作用于本工程目标，第三方 SDL 保持各自默认。
include(CheckIPOSupported)
# 21-B9yi(续62)：必须显式指定 `LANGUAGES C`。本工程 `project()` 只声明了 C，
# 而 `check_ipo_supported()` 默认按 CXX 探测 ⇒ 一直报「CMake doesn't support IPO
# for current CXX compiler」，LTO **从来没真正生效过**（build.ninja 里只有 -std=c11 -O3）。
check_ipo_supported(RESULT NDS_IPO_OK OUTPUT NDS_IPO_MSG LANGUAGES C)
if(NDS_IPO_OK)
    set_property(TARGET ndscore nds-emu test_nds PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
else()
    message(STATUS "IPO/LTO 不可用：${NDS_IPO_MSG}")
endif()
