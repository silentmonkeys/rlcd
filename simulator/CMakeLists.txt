# 桌面 LVGL 模拟器 —— 用 SDL2 显示 400x300 单色屏效果
#
# 依赖：
#   sudo apt install libsdl2-dev cmake build-essential
#
# 构建：
#   cd simulator && cmake -B build && cmake --build build -j
#   ./build/rlcd_sim
#
# 目的：把 components/ui 里的界面在桌面上跑起来，看得到真机上的
#       “单色屏 + 400x300”效果——包括每一个像素的黑白判定。
cmake_minimum_required(VERSION 3.16)
project(rlcd_sim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(SDL2 REQUIRED)

# --- 抓取 lvgl 源码 ------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    lvgl
    GIT_REPOSITORY https://github.com/lvgl/lvgl.git
    GIT_TAG        v9.2.2
)
set(LV_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE STRING "" FORCE)
set(LV_CONF_INCLUDE_SIMPLE ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(lvgl)

# --- UI 组件（从主项目共享） --------------------------------------
set(UI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../components/ui")

add_executable(rlcd_sim
    main.c
    sim_console.c
    ${UI_DIR}/src/ui_common.c
    ${UI_DIR}/src/ui_home.c
    ${UI_DIR}/src/ui_weather.c
    ${UI_DIR}/src/ui_calendar.c
    ${UI_DIR}/src/ui_device.c
    ${UI_DIR}/src/ui_setup.c
    ${UI_DIR}/src/ui_pages.c
    ${UI_DIR}/src/lcd_clock.c
    ${UI_DIR}/src/ui_font.c
    ${UI_DIR}/src/ui_weather_icon.c
)

# 字库目录（partitions/fonts/*.bin）路径传给模拟器，运行时 lv_binfont_create 读它
target_compile_definitions(rlcd_sim PRIVATE
    RLCD_FONTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../partitions/fonts")

target_include_directories(rlcd_sim PRIVATE
    ${UI_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${SDL2_INCLUDE_DIRS}
)

target_link_libraries(rlcd_sim PRIVATE
    lvgl
    ${SDL2_LIBRARIES}
    m
    pthread
)

# 单色 flush 需要用到 <math.h> —— 已通过 m 链接
