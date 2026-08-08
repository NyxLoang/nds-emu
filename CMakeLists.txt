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

add_executable(nds-emu
    src/main.c
    src/nds/nds.c
    src/bus/bus.c
    src/cart/cart.c
    src/cart/arm9.c
    src/cart/arm7.c
    src/window/window.c
    src/menu/menu.c
)

target_include_directories(nds-emu PRIVATE src)

target_link_libraries(nds-emu PRIVATE SDL2-static SDL2main SDL2_ttf)
