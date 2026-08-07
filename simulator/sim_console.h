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
 *   dump                      一次性返回全部字段（每行一条 val）
 *   ovr                       返回当前被 override 的字段名（空格分隔）
 *   page N                    切页
 *   next / prev               下一页 / 上一页
 *   mark MM-DD / unmark       日历标注
 *   marks CSV / events SPEC / labels SPEC   日历批量
 *   clear                     清除所有 override（恢复 sim_tick_data 自动填充）
 *   ping                      回 pong（检测连接）
 *   help                      列出可用字段
 *   任意未知 → 回 "error unknown command"
 *
 * 响应："ok <msg>" / "error <reason>" / "val <field> <value>"
 *
 * 帧终止：每条命令的响应之后**再发一个空行**（"\n"）作为帧结束标记。
 * 这样客户端读到空行即可确定一帧读完，无需靠 recv 超时判断结束
 * （dump 这类多行响应也因此可以被正确切分）。逐行读的旧客户端
 * （tools/rlcd_debug）不受影响 —— 空行只是它下一次读到的空串。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

// sim_tick_data 用这些位判断"该字段是否被外部设置过，不应覆盖"。
// 与 sim_console.c 内部的 s_override 位图对应。
//
// 位数已超过 32 → 掩码类型是 uint64_t，宏必须用 1ull。
// 加字段：这里加一位 + 在 sim_console.c 的 FIELDS[] 表加一行，共两处。
#define OVR_INDOOR_TEMP   (1ull << 0)
#define OVR_INDOOR_HUMI   (1ull << 1)
#define OVR_OUTDOOR_TEMP  (1ull << 2)
#define OVR_WEATHER_CODE  (1ull << 3)
#define OVR_WEATHER_TEXT  (1ull << 4)
#define OVR_CITY          (1ull << 5)
#define OVR_WIFI_CONN     (1ull << 6)
#define OVR_WIFI_RSSI     (1ull << 7)
#define OVR_BAT_PCT       (1ull << 8)
#define OVR_BAT_CHG       (1ull << 9)
#define OVR_FEELS_LIKE    (1ull << 10)
#define OVR_OUTDOOR_HUMI  (1ull << 11)
#define OVR_WIND_SPD      (1ull << 12)
#define OVR_WIND_DIR      (1ull << 13)
#define OVR_CLOUD         (1ull << 14)
#define OVR_PRESSURE      (1ull << 15)
#define OVR_VISIBILITY    (1ull << 16)
#define OVR_UV            (1ull << 17)
#define OVR_TEMP_MIN      (1ull << 18)
#define OVR_TEMP_MAX      (1ull << 19)
#define OVR_SUNRISE       (1ull << 20)
#define OVR_SUNSET        (1ull << 21)

// ---- 以下为后续补齐的字段（时间 / 设备页 / SD / Flash / 配网页）----
#define OVR_TIME          (1ull << 22)  // hour+minute+year+month+day+weekday 共用一位
#define OVR_WEATHER_UPD   (1ull << 23)
#define OVR_IP            (1ull << 24)
#define OVR_MAC           (1ull << 25)
#define OVR_SSID          (1ull << 26)
#define OVR_FREE_HEAP     (1ull << 27)
#define OVR_FLASH_SIZE    (1ull << 28)
#define OVR_UPTIME        (1ull << 29)
#define OVR_CHIP_MODEL    (1ull << 30)
#define OVR_CPU_CORES     (1ull << 31)
#define OVR_IDF_VER       (1ull << 32)
#define OVR_APP_VER       (1ull << 33)
#define OVR_AP_SSID       (1ull << 34)
#define OVR_AP_IP         (1ull << 35)
#define OVR_AP_ACTIVE     (1ull << 36)
#define OVR_SETUP_DISMISS (1ull << 37)
#define OVR_SD_MOUNTED    (1ull << 38)
#define OVR_SD_TOTAL      (1ull << 39)
#define OVR_SD_USED       (1ull << 40)
#define OVR_FLASH_USED    (1ull << 41)
#define OVR_FLASH_FREE    (1ull << 42)


// 启动 socket server 线程。port=0 时用默认端口 9000。
// 失败只 fprintf(stderr)，不退出 —— 模拟器还能用 stdin 调试。
bool sim_console_init(int port);

// sim_tick_data 查询"哪些字段被外部设置过，不该被自动填充覆盖"。
// 返回 OVR_* 位或。（原先只在 .c 里定义、靠隐式声明被 main.c 调用，
// 新编译器会报错 —— 补上正式声明。）
uint64_t sim_console_override_mask(void);

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
