// ui_bot —— 机器人页：播放 bloub 预渲染 1-bit 动画
//
// bloub（/home/chen/demo/bloub，MIT）的引擎是纯函数，动画在构建期由
// tools/gen_bloub_frames.py 采样成 BLO1 帧序列（partitions/fonts/bloub_seq.bin），
// 本页只做播放：按 fps 定时从 fonts 分区读一帧 1-bit 位图，塞进 I1 调色板
// lv_image_dsc 刷新（与 ui_weather_icon.c 同一套加载/调色板模式）。
//
// 文件格式（BLO1，帧定长无索引）：
//   header 16B: 'BLO1' | ver(u16) | w | h | fps | count | reserved
//   data:     count 帧，每帧 ceil(w/8)*h 字节，MSB 先行，1=墨(黑) 0=底(白)
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

// 创建机器人页并返回其 screen 对象（未激活）
lv_obj_t *ui_bot_create(void);

// 数据 → UI 同步（外部持锁调用；本页无模型字段，只刷状态栏）
void ui_bot_apply_locked(void);

#ifdef __cplusplus
}
#endif
