// 电池电压 ADC 采样。移植自 02_ESP-IDF/03_ADC_Test。
//
// 通道 ADC1_CH3（GPIO4）、12dB 衰减（量程 ~0..3.1V）、12bit。板上电池经分压
// 接入，采到的电压需 ×3 还原实际电池电压。用 curve-fitting 校准提升精度。
// 单次读数噪声在分压还原后可达 ±20mV，因此读电压时取 8 次均值（见下方注释）。

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

// 均值滤波：ESP32-S3 ADC 单次读数噪声可达 ±20mV（分压还原后 ×3 更明显），
// 而充电趋势判据阈值只有 20mV —— 不滤波会导致电量百分比跳变、充电态误报。
// 取 8 次（2^3，右移即可）算术平均：8 次 oneshot ≈ 0.2ms，5s 一次的调用频率
// 完全不敏感。任何一次读失败就整体放弃（返回 0 = "不可用"），不用半套数据。
#define BAT_SAMPLE_N      8
#define BAT_SAMPLE_SHIFT  3

float Adc_GetBatteryVoltage(void)
{
    if (!s_inited) return 0.0f;

    int sum = 0;
    for (int i = 0; i < BAT_SAMPLE_N; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, BAT_ADC_CHANNEL, &raw) != ESP_OK) return 0.0f;
        sum += raw;
    }
    int raw_avg = sum >> BAT_SAMPLE_SHIFT;

    int mv = 0;
    if (s_cali && adc_cali_raw_to_voltage(s_cali, raw_avg, &mv) == ESP_OK) {
        return 0.001f * (float)mv * BAT_DIVIDER;
    }
    // 无校准兜底：12bit / 12dB 满量程约 3.1V
    return (raw_avg / 4095.0f) * 3.1f * BAT_DIVIDER;
}

uint8_t Adc_LevelFromVoltage(float vol)
{
    if (vol <= 3.0f)  return 0;
    if (vol >= 4.12f) return 100;
    return (uint8_t)(((vol - 3.0f) / 1.12f) * 100.0f);
}

uint8_t Adc_GetBatteryLevel(void)
{
    return Adc_LevelFromVoltage(Adc_GetBatteryVoltage());
}
