// ui_weather_icon.c —— 按 weather_code 从 fonts 分区加载 1-bit 天气图标。
//
// 字库文件（tools/gen_weather_icons.py 生成）：weather_icons.bin
//   header:  'WETH' | version(u16) | count(u16) | reserved(u16)      = 10 bytes
//   index:    count 个 { code(u16) | offset(u32) | size(u16) }         stride 8
//   data:     各图标 { w(u16) h(u16) bits[] } 依次拼接，bits 为 1-bit MSB-first
//
// 运行时打开文件一次，二分查索引找到 offset/size，seek 读数据进静态缓冲，
// 拼出 lv_image_dsc_t（LV_COLOR_FORMAT_I1），调色板 index0=白 index1=黑。

#include "ui_weather_icon.h"

#include <string.h>
#include <stdio.h>

#include "lvgl.h"

// 40×40 I1：调色板 8 字节 + 5×40 = 200 字节像素；留到 64×64 余量
#define MAX_ICON_BUF 512

// 字库文件名（与 ui_font_*.bin 同级，命名对齐 ui_font_<name>_<size>.bin）
#define WICON_FILE "ui_font_weather_40.bin"

// LVGL 盘符（与 ui_font.c 的 FS_LETTER 一致）
#define WICON_LETTER "A"

// 图标根目录（不含盘符）：真机 = fonts 分区挂载点 "/spiffs"；
// 模拟器 = partitions/fonts（RLCD_FONTS_DIR）。
#ifdef ESP_PLATFORM
static const char * base_dir(void) { return "/spiffs"; }
#else
static const char * base_dir(void) { return RLCD_FONTS_DIR; }
#endif

static lv_color32_t s_palette[2];
static uint8_t       s_buf[MAX_ICON_BUF];   // 布局：palette(2) + bits
static lv_image_dsc_t s_dsc;

// 缓存文件句柄 + 索引，避免每次加载都重新 open / 扫索引
static lv_fs_file_t s_file;
static bool         s_file_ok = false;
static uint16_t     s_count   = 0;

// 索引项解析后的缓存（code → offset/size），按需从文件读取一次
#define MAX_INDEX 128
static struct { uint16_t code; uint32_t offset; uint16_t size; }
    s_index[MAX_INDEX];

static bool load_index(void)
{
    if (s_file_ok) return true;

    char path[128];
    snprintf(path, sizeof(path), WICON_LETTER ":%s/" WICON_FILE, base_dir());

    if (lv_fs_open(&s_file, path, LV_FS_MODE_RD) != LV_FS_RES_OK) {
        s_file_ok = false;
        return false;
    }

    // 读 header：magic(4) + version(2) + count(2) + reserved(2) = 10 bytes
    uint8_t hdr[10];
    uint32_t got = 0;
    if (lv_fs_read(&s_file, hdr, 10, &got) != LV_FS_RES_OK || got != 10) goto fail;
    if (memcmp(hdr, "WETH", 4) != 0) goto fail;

    s_count = (uint16_t)(hdr[6] | (hdr[7] << 8));
    if (s_count > MAX_INDEX) s_count = MAX_INDEX;

    // 读整个索引：每项 <HIH = 8 bytes
    uint8_t idx_buf[MAX_INDEX * 8];
    uint32_t idx_bytes = (uint32_t)s_count * 8;
    if (lv_fs_read(&s_file, idx_buf, idx_bytes, &got) != LV_FS_RES_OK || got != idx_bytes) goto fail;

    for (uint16_t i = 0; i < s_count; i++) {
        uint8_t *p = idx_buf + i * 8;
        s_index[i].code   = (uint16_t)(p[0] | (p[1] << 8));
        s_index[i].offset = (uint32_t)(p[2] | (p[3] << 8) | (p[4] << 16) | (p[5] << 24));
        s_index[i].size   = (uint16_t)(p[6] | (p[7] << 8));
    }

    s_file_ok = true;
    return true;

fail:
    lv_fs_close(&s_file);
    s_file_ok = false;
    return false;
}

// 二分查索引，返回 size（0 表示未找到）。offset 写入 *out_offset。
static uint16_t find_icon(int code, uint32_t *out_offset)
{
    if (!load_index()) return 0;

    int lo = 0, hi = s_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint16_t c = s_index[mid].code;
        if (c == (uint16_t)code) {
            *out_offset = s_index[mid].offset;
            return s_index[mid].size;
        }
        if (c < (uint16_t)code) lo = mid + 1;
        else                     hi = mid - 1;
    }
    return 0;
}

bool ui_weather_icon_load(lv_obj_t *img, int code)
{
    uint32_t offset = 0;
    uint16_t size = find_icon(code, &offset);

    if (size == 0) {
        // 找不到指定代码 → 用 999（未知）兜底
        if (code != 999) return ui_weather_icon_load(img, 999);
        return false;
    }

    // 数据布局：w(2) + h(2) + bits(size-4)
    if (size < 4 || 8 + size - 4 > sizeof(s_buf)) {
        if (code != 999) return ui_weather_icon_load(img, 999);
        return false;
    }

    uint32_t got = 0;
    if (lv_fs_seek(&s_file, offset, LV_FS_SEEK_SET) != LV_FS_RES_OK) goto fail_read;
    uint8_t wh[4];
    if (lv_fs_read(&s_file, wh, 4, &got) != LV_FS_RES_OK || got != 4) goto fail_read;

    uint32_t w = wh[0] | (wh[1] << 8);
    uint32_t h = wh[2] | (wh[3] << 8);
    uint32_t bits_size = size - 4;
    uint32_t stride = (w + 7) / 8;

    uint8_t *bits = s_buf + 8;
    if (lv_fs_read(&s_file, bits, bits_size, &got) != LV_FS_RES_OK || got != bits_size) goto fail_read;

    // 调色板：0 = 白底，1 = 黑线
    s_palette[0] = (lv_color32_t){.red = 0xFF, .green = 0xFF, .blue = 0xFF, .alpha = 0xFF};
    s_palette[1] = (lv_color32_t){.red = 0x00, .green = 0x00, .blue = 0x00, .alpha = 0xFF};
    memcpy(s_buf, s_palette, 8);

    s_dsc.header.magic      = 0x19;
    s_dsc.header.cf         = LV_COLOR_FORMAT_I1;
    s_dsc.header.flags      = 0;
    s_dsc.header.w          = w;
    s_dsc.header.h          = h;
    s_dsc.header.stride     = stride;
    s_dsc.header.reserved_2 = 0;
    s_dsc.data_size         = 8 + bits_size;
    s_dsc.data              = s_buf;
    s_dsc.reserved          = NULL;

    lv_img_set_src(img, &s_dsc);
    return true;

fail_read:
    if (code != 999) return ui_weather_icon_load(img, 999);
    return false;
}
