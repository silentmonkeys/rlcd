// SD 卡 BSP 实现 —— SDMMC 1-line（CLK=38 CMD=21 D0=39）
//
// 引脚来源：02_ESP-IDF/06_SD_Card 参考项目的默认值（与本板 TF 槽走线一致）。
// 挂载后 UI 层通过 SdcardBsp_QueryUsage() 拉 f_getfree 读用量；
// 未插卡时 esp_vfs_fat_sdmmc_mount 返回错误，本函数返回非 ESP_OK 让上层跳过。

#include "sdcard_bsp.h"

#include <string.h>
#include <sys/stat.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <driver/sdmmc_host.h>
#include <sdmmc_cmd.h>

static const char *TAG = "sdcard_bsp";

// SDMMC 1-line 引脚（与 02_ESP-IDF/06_SD_Card 保持一致）
#define SD_PIN_CLK  38
#define SD_PIN_CMD  21
#define SD_PIN_D0   39

static sdmmc_card_t *s_card = NULL;
static bool          s_mounted = false;

esp_err_t SdcardBsp_Init(void)
{
    if (s_mounted) return ESP_OK;

    // 参数与 02_ESP-IDF/06_SD_Card 参考项目保持一致，避免"参考能读、这边不能读"
    // 的差异（allocation_unit_size = 48KB、不改 host 频率、走 SDMMC_HOST_DEFAULT
    // 的默认 probing/HS 自适应）
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files              = 5;
    mount_config.allocation_unit_size   = 16 * 1024 * 3;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    // 注意：不再手动调 host.max_freq_khz——SDMMC_HOST_DEFAULT() 会走
    // 默认 probing/HS 自适应；某些卡在人工强设 HS 时反而握手失败。

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.clk   = (gpio_num_t)SD_PIN_CLK;
    slot_config.cmd   = (gpio_num_t)SD_PIN_CMD;
    slot_config.d0    = (gpio_num_t)SD_PIN_D0;
    // 1-line 时 D1..D3 保持默认（未接），CD/WD 不用

    ESP_LOGI(TAG, "mounting SDMMC 1-line: clk=%d cmd=%d d0=%d width=%d",
             SD_PIN_CLK, SD_PIN_CMD, SD_PIN_D0, (int)slot_config.width);

    // 最多重试 3 次 —— 某些卡上电后首帧握手偶尔失败，稍等再来一次能成功
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        err = esp_vfs_fat_sdmmc_mount(SDCARD_MOUNT_POINT, &host, &slot_config,
                                      &mount_config, &s_card);
        if (err == ESP_OK) break;
        ESP_LOGW(TAG, "mount attempt %d failed: %s (0x%x)",
                 attempt + 1, esp_err_to_name(err), (unsigned)err);
        s_card = NULL;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD 挂载失败: %s —— 请检查:"
                 " 1) 卡是否插到位; 2) 是否 FAT32 格式; 3) 引脚接线 clk=%d cmd=%d d0=%d",
                 esp_err_to_name(err), SD_PIN_CLK, SD_PIN_CMD, SD_PIN_D0);
        s_card = NULL;
        s_mounted = false;
        return err;
    }
    s_mounted = true;
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

bool SdcardBsp_IsMounted(void) { return s_mounted; }

bool SdcardBsp_QueryUsage(uint32_t *total_mb, uint32_t *used_mb)
{
    if (!s_mounted || !s_card) return false;

    // 卡 CSD 上报的总容量（挂载时即确定，绝对可用）作为兜底。
    uint64_t card_total_bytes = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;

    // ESP-IDF v6.0.1 的 POSIX statvfs 是 ENOSYS 桩函数，恒失败；
    // 官方封装 esp_vfs_fat_info() 内部按挂载点解析盘符再调 FATFS f_getfree，
    // 才能真正拿到剩余空间。
    uint64_t total_bytes = 0, free_bytes = 0;
    esp_err_t err = esp_vfs_fat_info(SDCARD_MOUNT_POINT, &total_bytes, &free_bytes);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_vfs_fat_info failed: %s, using card capacity only",
                 esp_err_to_name(err));
        if (total_mb) *total_mb = (uint32_t)(card_total_bytes / (1024 * 1024));
        if (used_mb)  *used_mb  = 0;
        return true;
    }
    // f_getfree 拿不到总容量（=0）时回落到卡上报的容量
    if (total_bytes == 0) total_bytes = card_total_bytes;
    uint64_t used_bytes = (total_bytes > free_bytes) ? (total_bytes - free_bytes) : 0;
    if (total_mb) *total_mb = (uint32_t)(total_bytes / (1024 * 1024));
    if (used_mb)  *used_mb  = (uint32_t)(used_bytes  / (1024 * 1024));
    ESP_LOGD(TAG, "usage: total=%u MB used=%u MB",
             (unsigned)(total_bytes / (1024 * 1024)),
             (unsigned)(used_bytes  / (1024 * 1024)));
    return true;
}

// 主动卸载 —— 拔卡探活失败时调用；不打印错误（正常路径）
static void sdcard_unmount(void)
{
    if (!s_mounted) return;
    esp_vfs_fat_sdcard_unmount(SDCARD_MOUNT_POINT, s_card);
    s_card = NULL;
    s_mounted = false;
    ESP_LOGW(TAG, "SD 卡已卸载");
}

bool SdcardBsp_ProbeAndRemount(void)
{
    if (s_mounted && s_card) {
        // 探活：用一次轻量 CMD13（GO_IDLE + status），走 sdmmc_get_status。
        // 卡被拔出后此函数会返回非 ESP_OK；随后同步 unmount 掉。
        if (sdmmc_get_status(s_card) != ESP_OK) {
            ESP_LOGW(TAG, "sdmmc_get_status fail → 卸载 SD");
            sdcard_unmount();
        } else {
            return true;
        }
    }
    // 未挂载 → 试挂一次（用户可能刚插卡）
    // Init 内部本身也会重试 3 次，不必外层再套；这里失败就静默返回 false
    (void)SdcardBsp_Init();
    return s_mounted;
}
