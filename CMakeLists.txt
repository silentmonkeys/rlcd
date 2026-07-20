cmake_minimum_required(VERSION 3.16)

# 根据当前配置区分：构建 ESP-IDF 固件 or 桌面模拟器。
# 桌面模拟器请进 simulator/ 目录 cmake -B build && cmake --build build 单独构建。
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rlcd_home)
