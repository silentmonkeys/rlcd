/*
 * sim_console —— 模拟器调试控制台后端
 *
 * 提供一个线程安全的命令队列 + TCP socket server（默认 127.0.0.1:9000），
 * 让外部调试客户端（rlcd_debug）通过文本协议远程读写 ui_model、切页、
 * 标注日历等。主循环在每帧 drain 队列 → 所有 LVGL 操作都在主线程执行，
 * 彻底消除 stdin_cmd_thread 直接调 LVGL 导致的多线程崩溃。
 *
 * 同时保留 stdin 控制台（stdin_cmd_thread 现在只把命令塞进队列，不再碰 LVGL）。
 *
 * 协议（文本行，\n 分隔）：
 *   set <field> <value>       写 ui_model 字段，并设 override 位
 *   get <field>               读字段当前值
 *   page N                    切页
 *   next / prev               下一页 / 上一页
 *   mark MM-DD / unmark       日历标注
 *   marks CSV / events SPEC / labels SPEC   日历批量
 *   clear                     清除所有 override（恢复 sim_tick_data 自动填充）
 *   ping                      回 pong（检测连接）
 *   help                      列出可用字段
 *   任意未知 → 回 "error: unknown command"
 *
 * 响应："ok <msg>" / "error: <reason>" / "val <json>"
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

// sim_tick_data 用这些位判断"该字段是否被外部设置过，不应覆盖"。
// 与 sim_console.c 内部的 s_override 位图对应。
#define OVR_INDOOR_TEMP   (1u << 0)
#define OVR_INDOOR_HUMI   (1u << 1)
#define OVR_OUTDOOR_TEMP  (1u << 2)
#define OVR_WEATHER_CODE  (1u << 3)
#define OVR_WEATHER_TEXT  (1u << 4)
#define OVR_CITY          (1u << 5)
#define OVR_WIFI_CONN     (1u << 6)
#define OVR_WIFI_RSSI     (1u << 7)
#define OVR_BAT_PCT       (1u << 8)
#define OVR_BAT_CHG       (1u << 9)
#define OVR_FEELS_LIKE    (1u << 10)
#define OVR_OUTDOOR_HUMI  (1u << 11)
#define OVR_WIND_SPD      (1u << 12)
#define OVR_WIND_DIR      (1u << 13)
#define OVR_CLOUD         (1u << 14)
#define OVR_PRESSURE      (1u << 15)
#define OVR_VISIBILITY    (1u << 16)
#define OVR_UV            (1u << 17)
#define OVR_TEMP_MIN      (1u << 18)
#define OVR_TEMP_MAX      (1u << 19)
#define OVR_SUNRISE       (1u << 20)
#define OVR_SUNSET        (1u << 21)

// 启动 socket server 线程。port=0 时用默认端口 9000。
// 失败只 fprintf(stderr)，不退出 —— 模拟器还能用 stdin 调试。
bool sim_console_init(int port);

// 关闭 server（程序退出时调，也可不调 —— OS 会回收）
void sim_console_shutdown(void);

// 主循环每帧调一次：从队列取出命令并在主线程执行。
// 返回本帧处理的命令数（0 表示队列为空）。
int sim_console_drain(void);

// 把一条命令塞进队列（stdin 线程用）。返回是否成功。
// 调用方须保证 cmd 是合法以 \0 结尾的字符串（会被内部 strdup）。
bool sim_console_submit(const char *cmd);

// 主线程直接设字段 + 覆盖 override 位（启动时 --temp/--humi 用，此时队列还没 drain）。
// 只支持 float 字段；返回是否匹配到字段。
bool sim_console_set_field_override(const char *field, float val);

#ifdef __cplusplus
}
#endif
