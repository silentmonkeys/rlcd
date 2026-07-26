# rlcd_debug —— RLCD 模拟器独立调试客户端
#
# 纯 POSIX / Win32，不依赖 SDL/LVGL，单文件即可编译：
#   gcc -O2 -o rlcd_debug rlcd_debug.c        # Linux/macOS
#   cl  /O2  rlcd_debug.c ws2_32.lib           # MSVC
#
# 也可通过 cmake 构建：
#   cmake -B build && cmake --build build
cmake_minimum_required(VERSION 3.16)
project(rlcd_debug C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(rlcd_debug rlcd_debug.c)

if(WIN32)
    target_link_libraries(rlcd_debug PRIVATE ws2_32)
endif()
