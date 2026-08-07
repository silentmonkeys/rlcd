// ui_font —— 从字库文件加载 3 份 LVGL binfont。
//
// 真机：字库在 fonts 分区（SPIFFS "/spiffs"），main.cpp 先 UiFont_MountFs()
//       挂载，再 UiFont_Load() 建 3 个 font。
// 模拟器：字库直接是 partitions/fonts/*.bin（工作目录下的普通文件），
//       simulator/main.c 调 UiFont_LoadFromDir() 指定目录加载。
//
// 加字流程：改 tools/gen_font.sh → 重跑 → idf.py flash-fonts（只烧 1MB 分区，
// 不动 4MB app）。运行时 lv_binfont_create() 打开文件流式读入 font 结构。

#include "ui_font.h"

#include <string.h>
#include <stdio.h>

#include "lvgl.h"

#ifdef ESP_PLATFORM
#include <esp_log.h>
#include <esp_spiffs.h>
static const char *TAG = "ui_font";
#define FONT_LOGE(...)  ESP_LOGE(TAG, __VA_ARGS__)
#define FONT_LOGI(...)  ESP_LOGI(TAG, __VA_ARGS__)
#else
#include <stdio.h>
#define FONT_LOGE(fmt, ...)  fprintf(stderr, "[ui_font] " fmt "\n", ##__VA_ARGS__)
#define FONT_LOGI(fmt, ...)  fprintf(stderr, "[ui_font] " fmt "\n", ##__VA_ARGS__)
#endif

// LVGL FS 驱动盘符 —— 真机 CONFIG_LV_FS_POSIX_LETTER=65('A')，
// 模拟器 lv_conf.h LV_FS_POSIX_LETTER='A'。两边都是 'A'。
#define FS_LETTER          "A"
#define FONTS_MOUNT_POINT  "/spiffs"

static lv_font_t *s_cjk    = NULL;
static lv_font_t *s_big    = NULL;
static lv_font_t *s_mid    = NULL;
static lv_font_t *s_mood   = NULL;
static bool       s_ready  = false;

// 兜底：LVGL 内置默认字体 —— 加载失败时 UI 至少还能画 ASCII/数字。
static const lv_font_t *fallback_font(void)
{
    return &lv_font_montserrat_14;
}

// 从 "A:<dir>/<name>" 加载一个 binfont
static lv_font_t *load_bin(const char *dir, const char *name)
{
    char lv_path[128];
    snprintf(lv_path, sizeof(lv_path), FS_LETTER ":%s/%s", dir, name);
    lv_font_t *f = lv_binfont_create(lv_path);
    if (!f) FONT_LOGE("lv_binfont_create('%s') failed", lv_path);
    else    FONT_LOGI("font loaded: %s", lv_path);
    return f;
}

// 公共加载核心：从 dir 目录加载 3 份字库
static bool load_all_from(const char *dir)
{
    if (s_ready) return true;
    s_cjk  = load_bin(dir, "ui_font_cjk_16.bin");
    s_big  = load_bin(dir, "ui_font_digit_big.bin");
    s_mid  = load_bin(dir, "ui_font_digit_mid.bin");
    s_mood = load_bin(dir, "ui_font_mood_16.bin");
    if (!s_cjk || !s_big || !s_mid) {
        FONT_LOGE("至少一个字库加载失败");
        return false;
    }
    s_ready = true;
    return true;
}

#ifdef ESP_PLATFORM
esp_err_t UiFont_MountFs(void)
{
    esp_vfs_spiffs_conf_t conf = {};
    conf.base_path              = FONTS_MOUNT_POINT;
    conf.partition_label        = "fonts";
    conf.max_files              = 4;
    conf.format_if_mount_failed = false;
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        FONT_LOGE("fonts SPIFFS 挂载失败: %s", esp_err_to_name(err));
        return err;
    }
    size_t total = 0, used = 0;
    esp_spiffs_info(conf.partition_label, &total, &used);
    FONT_LOGI("fonts SPIFFS mounted at %s (total=%u used=%u)",
              FONTS_MOUNT_POINT, (unsigned)total, (unsigned)used);
    return ESP_OK;
}

esp_err_t UiFont_Load(void)
{
    return load_all_from(FONTS_MOUNT_POINT) ? ESP_OK : ESP_FAIL;
}
#else
// 模拟器：直接从磁盘目录加载
bool UiFont_LoadFromDir(const char *dir)
{
    return load_all_from(dir);
}
#endif

const lv_font_t *ui_font_cjk_16(void)    { return s_cjk ? s_cjk : fallback_font(); }
const lv_font_t *ui_font_digit_big(void) { return s_big ? s_big : fallback_font(); }
const lv_font_t *ui_font_digit_mid(void) { return s_mid ? s_mid : fallback_font(); }
const lv_font_t *ui_font_mood_16(void)   { return s_mood ? s_mood : fallback_font(); }
