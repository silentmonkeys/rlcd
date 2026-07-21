// 电池电压 ADC 采样。移植自 02_ESP-IDF/03_ADC_Test。
//
// 通道 ADC1_CH3（GPIO4）、12dB 衰减（量程 ~0..3.1V）、12bit。板上电池经分压
// 接入，采到的电压需 ×3 还原实际电池电压。用 curve-fitting 校准提升精度。

#include "adc_bsp.h"

#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_log.h>

static const char *TAG = "adc_bsp";

#define BAT_ADC_UNIT      ADC_UNIT_1
#define BAT_ADC_CHANNEL   ADC_CHANNEL_3     // GPIO4
#define BAT_ADC_ATTEN     ADC_ATTEN_DB_12
#define BAT_ADC_BITWIDTH  ADC_BITWIDTH_12
#define BAT_DIVIDER       3.0f              // 板上分压比（参考例程 vol = tage * 3）

static adc_oneshot_unit_handle_t s_adc = NULL;
static adc_cali_handle_t         s_cali = NULL;
static bool                      s_inited = false;

esp_err_t Adc_PortInit(void)
{
    if (s_inited) return ESP_OK;

    adc_oneshot_unit_init_cfg_t unit_cfg = {};
    unit_cfg.unit_id = BAT_ADC_UNIT;
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit fail: %d", err);
        return err;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {};
    chan_cfg.bitwidth = BAT_ADC_BITWIDTH;
    chan_cfg.atten    = BAT_ADC_ATTEN;
    err = adc_oneshot_config_channel(s_adc, BAT_ADC_CHANNEL, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_config_channel fail: %d", err);
        return err;
    }

    // 曲线拟合校准（S3 支持）；失败则退化为无校准（原始 raw 估算）
    adc_cali_curve_fitting_config_t cali_cfg = {};
    cali_cfg.unit_id  = BAT_ADC_UNIT;
    cali_cfg.chan     = BAT_ADC_CHANNEL;
    cali_cfg.atten    = BAT_ADC_ATTEN;
    cali_cfg.bitwidth = BAT_ADC_BITWIDTH;
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "curve-fitting 校准不可用，电压读数精度下降");
        s_cali = NULL;
    }

    s_inited = true;
    ESP_LOGI(TAG, "ADC 电池采样就绪 (unit1 ch3/GPIO4, 12dB, x%.0f 分压)", BAT_DIVIDER);
    return ESP_OK;
}

float Adc_GetBatteryVoltage(void)
{
    if (!s_inited) return 0.0f;

    int raw = 0;
    if (adc_oneshot_read(s_adc, BAT_ADC_CHANNEL, &raw) != ESP_OK) return 0.0f;

    int mv = 0;
    if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
        return 0.001f * (float)mv * BAT_DIVIDER;
    }
    // 无校准兜底：12bit / 12dB 满量程约 3.1V
    return (raw / 4095.0f) * 3.1f * BAT_DIVIDER;
}

uint8_t Adc_GetBatteryLevel(void)
{
    float vol = Adc_GetBatteryVoltage();
    if (vol <= 3.0f)  return 0;
    if (vol >= 4.12f) return 100;
    return (uint8_t)(((vol - 3.0f) / 1.12f) * 100.0f);
}
