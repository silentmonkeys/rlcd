// audio_bsp 实现 —— 见 audio_bsp.h 头注释。
//
// 技术路线：esp_codec_dev 统一管理 codec 控制面（I2C 寄存器）+ 数据面（I2S
// std 全双工）。ES8311 只做 DAC、ES7210 只做 ADC，两个 codec 共享同一组
// I2S 时钟（S3 是 master），数据线各走各的。

#include "audio_bsp.h"
#include "user_config.h"
#include "i2c_bsp.h"

#include <string.h>
#include <driver/i2s_std.h>
#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_check.h>

#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>

static const char *TAG = "audio_bsp";

// 采样规格：xiaozhi 协议的语音流就是 16k/16bit/mono/60ms
#define AUDIO_SAMPLE_RATE  16000
#define AUDIO_BITS         16
#define AUDIO_CHANNELS     1

// 播放音量（0~100）与麦克风增益（dB）
#define AUDIO_OUT_VOL      78
#define AUDIO_IN_GAIN_DB   30.0f

static bool s_ready = false;
static i2s_chan_handle_t s_tx = NULL, s_rx = NULL;
static esp_codec_dev_handle_t s_out = NULL;   // ES8311 DAC
static esp_codec_dev_handle_t s_in = NULL;    // ES7210 ADC
static const audio_codec_data_if_t *s_i2s_data = NULL;
static const audio_codec_ctrl_if_t *s_i2c_es8311 = NULL;
static const audio_codec_ctrl_if_t *s_i2c_es7210 = NULL;

// 在一条已初始化的 I2C 总线上探测 7bit 地址；命中返回 true 并填 *out
static bool probe_i2c(i2c_master_bus_handle_t bus, const uint8_t *cands, int n, uint8_t *out)
{
    for (int i = 0; i < n; i++) {
        if (i2c_master_probe(bus, cands[i], 100) == ESP_OK) {
            *out = cands[i];
            return true;
        }
    }
    return false;
}

// I2S std 全双工：一个控制器上建 TX+RX 通道，时钟配置必须一致
static esp_err_t i2s_duplex_init(void)
{
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;          // 欠载时自动送静音，避免杂音
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, &s_rx),
                        TAG, "i2s_new_channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = AUDIO_I2S_MCLK_PIN,
            .bclk = AUDIO_I2S_BCLK_PIN,
            .ws   = AUDIO_I2S_LRCK_PIN,
            .dout = AUDIO_I2S_DOUT_PIN,
            .din  = AUDIO_I2S_DIN_PIN,
        },
    };
    // 单声道时只占左槽：TX 送左槽给 ES8311，RX 收左槽（ES7210 MIC1）。
    // 要换双麦混音时把 RX 的 slot_mask 改 BOTH 并在这里做下混。
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg),
                        TAG, "init std tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg),
                        TAG, "init std rx");
    return ESP_OK;
}

esp_err_t AudioBsp_Init(void)
{
    if (s_ready) return ESP_OK;

    i2c_master_bus_handle_t bus = I2cBus_GetHandle();
    if (!bus) {
        ESP_LOGE(TAG, "I2C 总线未初始化（AudioBsp_Init 必须在 I2cBus_Init 之后）");
        return ESP_ERR_INVALID_STATE;
    }

    // codec I2C 地址 strap 未知 → 逐个 probe（cfg.addr 要 8bit 形式，驱动里 >>1）
    uint8_t a8311 = 0, a7210 = 0;
    const uint8_t C8311[] = { 0x18, 0x19 };                  // ES8311: CE=0/1
    const uint8_t C7210[] = { 0x40, 0x41, 0x42, 0x43,        // ES7210: AD1AD0
                              0x20, 0x21, 0x22, 0x23 };
    if (!probe_i2c(bus, C8311, sizeof(C8311), &a8311)) {
        ESP_LOGW(TAG, "未探测到 ES8311 —— 语音播放不可用，回落纯文本");
        return ESP_ERR_NOT_FOUND;
    }
    if (!probe_i2c(bus, C7210, sizeof(C7210), &a7210)) {
        ESP_LOGW(TAG, "未探测到 ES7210 —— 收音不可用，回落纯文本");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "ES8311@0x%02x ES7210@0x%02x（7bit）", a8311, a7210);

    // 控制面（I2C）×2 + 数据面（I2S）×1
    audio_codec_i2c_cfg_t i2c_cfg_8311 = {
        .bus_handle = bus, .addr = (uint8_t)(a8311 << 1) };
    s_i2c_es8311 = audio_codec_new_i2c_ctrl(&i2c_cfg_8311);
    audio_codec_i2c_cfg_t i2c_cfg_7210 = {
        .bus_handle = bus, .addr = (uint8_t)(a7210 << 1) };
    s_i2c_es7210 = audio_codec_new_i2c_ctrl(&i2c_cfg_7210);
    if (!s_i2c_es8311 || !s_i2c_es7210) {
        ESP_LOGE(TAG, "i2c ctrl new fail");
        return ESP_FAIL;
    }

    ESP_RETURN_ON_ERROR(i2s_duplex_init(), TAG, "i2s duplex");
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0, .tx_handle = s_tx, .rx_handle = s_rx };
    s_i2s_data = audio_codec_new_i2s_data(&i2s_cfg);
    if (!s_i2s_data) {
        ESP_LOGE(TAG, "i2s data if new fail");
        return ESP_FAIL;
    }

    // ES8311：从机 + 外部 MCLK，只做 DAC；PA 由本模块自管（pa_pin=-1）
    es8311_codec_cfg_t es8311_cfg = {};
    es8311_cfg.ctrl_if      = s_i2c_es8311;
    es8311_cfg.codec_mode   = ESP_CODEC_DEV_WORK_MODE_DAC;
    es8311_cfg.pa_pin       = -1;
    es8311_cfg.use_mclk     = true;
    es8311_cfg.master_mode  = false;
    const audio_codec_if_t *es8311_if = es8311_codec_new(&es8311_cfg);

    // ES7210：从机，收 MIC1
    es7210_codec_cfg_t es7210_cfg = {};
    es7210_cfg.ctrl_if      = s_i2c_es7210;
    es7210_cfg.master_mode  = false;
    es7210_cfg.mic_selected = ES7210_SEL_MIC1;
    const audio_codec_if_t *es7210_if = es7210_codec_new(&es7210_cfg);
    if (!es8311_if || !es7210_if) {
        ESP_LOGE(TAG, "codec if new fail");
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t dev_cfg = {};
    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_OUT;
    dev_cfg.codec_if = es8311_if;
    dev_cfg.data_if  = s_i2s_data;
    s_out = esp_codec_dev_new(&dev_cfg);
    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
    dev_cfg.codec_if = es7210_if;
    s_in = esp_codec_dev_new(&dev_cfg);
    if (!s_out || !s_in) {
        ESP_LOGE(TAG, "codec dev new fail");
        return ESP_FAIL;
    }

    // PA 脚：推挽输出，默认关
    gpio_config_t pa = {};
    pa.pin_bit_mask = 1ULL << AUDIO_PA_PIN;
    pa.mode         = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&pa));
    gpio_set_level(AUDIO_PA_PIN, AUDIO_PA_ACTIVE ? 0 : 1);

    s_ready = true;
    ESP_LOGI(TAG, "音频底座就绪（16k/16bit/mono，vol=%d in=%.0fdB）",
             AUDIO_OUT_VOL, AUDIO_IN_GAIN_DB);
    return ESP_OK;
}

bool AudioBsp_Ready(void) { return s_ready; }

void AudioBsp_PaEnable(bool on)
{
    gpio_set_level(AUDIO_PA_PIN, on ? AUDIO_PA_ACTIVE : !AUDIO_PA_ACTIVE);
}

esp_err_t AudioBsp_TalkStart(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = AUDIO_BITS,
        .channel         = AUDIO_CHANNELS,
        .sample_rate     = AUDIO_SAMPLE_RATE,
    };
    int rc = esp_codec_dev_open(s_out, &fs);
    if (rc != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec out open fail: %d", rc);
        return ESP_FAIL;
    }
    rc = esp_codec_dev_open(s_in, &fs);
    if (rc != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec in open fail: %d", rc);
        esp_codec_dev_close(s_out);
        return ESP_FAIL;
    }
    esp_codec_dev_set_out_vol(s_out, AUDIO_OUT_VOL);
    esp_codec_dev_set_in_gain(s_in, AUDIO_IN_GAIN_DB);
    AudioBsp_PaEnable(true);
    return ESP_OK;
}

void AudioBsp_TalkStop(void)
{
    if (!s_ready) return;
    AudioBsp_PaEnable(false);
    esp_codec_dev_close(s_in);
    esp_codec_dev_close(s_out);
}

int AudioBsp_SpeakerWrite(const int16_t *pcm, int samples)
{
    if (!s_ready || !pcm || samples <= 0) return -1;
    int rc = esp_codec_dev_write(s_out, (void *)pcm, samples * 2);
    return (rc == ESP_CODEC_DEV_OK) ? samples : -1;
}

int AudioBsp_MicRead(int16_t *pcm, int samples)
{
    if (!s_ready || !pcm || samples <= 0) return -1;
    int rc = esp_codec_dev_read(s_in, pcm, samples * 2);
    return (rc == ESP_CODEC_DEV_OK) ? samples : -1;
}
