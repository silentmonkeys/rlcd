// I2C 主机总线 + SHTC3 驱动。移植自 02_ESP-IDF/05_I2C_SHTC3，
// 用 ESP-IDF v6 的 i2c_master 新 API。
//
// 布线（见 main/user_config.h）：
//     SDA = GPIO13, SCL = GPIO14, 内部上拉打开
// SHTC3 地址 0x70，400 kHz。采样流程：Wakeup → MEAS_T_RH_POLLING → 读 6 字节。

#include "i2c_bsp.h"

#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/i2c_master.h>
#include <esp_log.h>

static const char *TAG = "i2c_bsp";

// -------- I2C bus --------
static i2c_master_bus_handle_t s_bus = NULL;

esp_err_t I2cBus_Init(int scl_pin, int sda_pin, int i2c_port)
{
    if (s_bus) return ESP_OK;

    i2c_master_bus_config_t cfg = {};
    cfg.clk_source                   = I2C_CLK_SRC_DEFAULT;
    cfg.i2c_port                     = i2c_port;
    cfg.scl_io_num                   = (gpio_num_t) scl_pin;
    cfg.sda_io_num                   = (gpio_num_t) sda_pin;
    cfg.glitch_ignore_cnt            = 7;
    cfg.flags.enable_internal_pullup = true;

    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus fail: %d", err);
        s_bus = NULL;
    }
    return err;
}

// -------- SHTC3 --------
#define SHTC3_ADDR              0x70
#define SHTC3_CMD_READ_ID       0xEFC8
#define SHTC3_CMD_SOFT_RESET    0x805D
#define SHTC3_CMD_SLEEP         0xB098
#define SHTC3_CMD_WAKEUP        0x3517
#define SHTC3_CMD_MEAS_T_RH     0x7866   // T first, clock stretching disabled
#define SHTC3_CRC_POLY          0x131
#define SHTC3_TEMP_OFFSET       4        // 板级自热补偿（沿用 factory 例程）

static i2c_master_dev_handle_t s_shtc3 = NULL;

static const TickType_t kBusTimeout  = pdMS_TO_TICKS(1000);
static const TickType_t kXferTimeout = pdMS_TO_TICKS(1000);

static esp_err_t shtc3_send_cmd(uint16_t cmd)
{
    uint8_t buf[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xff) };
    i2c_master_bus_wait_all_done(s_bus, kBusTimeout);
    return i2c_master_transmit(s_shtc3, buf, 2, kXferTimeout);
}

static esp_err_t shtc3_write_read(uint16_t cmd, uint8_t *rx, size_t rx_len)
{
    uint8_t tx[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xff) };
    i2c_master_bus_wait_all_done(s_bus, kBusTimeout);
    return i2c_master_transmit_receive(s_shtc3, tx, 2, rx, rx_len, kXferTimeout);
}

static bool shtc3_check_crc(const uint8_t *data, uint8_t n, uint8_t checksum)
{
    uint8_t crc = 0xFF;
    for (uint8_t i = 0; i < n; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ SHTC3_CRC_POLY) : (uint8_t)(crc << 1);
        }
    }
    return crc == checksum;
}

static esp_err_t shtc3_wakeup(void)
{
    esp_err_t err = shtc3_send_cmd(SHTC3_CMD_WAKEUP);
    vTaskDelay(pdMS_TO_TICKS(1));   // datasheet: 240us；给到 1ms 更稳
    return err;
}

static esp_err_t shtc3_sleep(void)
{
    return shtc3_send_cmd(SHTC3_CMD_SLEEP);
}

esp_err_t Shtc3_Init(void)
{
    if (!s_bus) {
        ESP_LOGE(TAG, "I2C bus not init");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_shtc3) return ESP_OK;

    i2c_device_config_t dev = {};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address  = SHTC3_ADDR;
    dev.scl_speed_hz    = 400000;
    esp_err_t err = i2c_master_bus_add_device(s_bus, &dev, &s_shtc3);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add SHTC3 dev fail: %d", err);
        s_shtc3 = NULL;
        return err;
    }

    shtc3_wakeup();
    shtc3_send_cmd(SHTC3_CMD_SOFT_RESET);
    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t rx[3] = {0};
    err = shtc3_write_read(SHTC3_CMD_READ_ID, rx, 3);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SHTC3 read ID xfer fail: %d", err);
        return err;
    }
    if (!shtc3_check_crc(rx, 2, rx[2])) {
        ESP_LOGE(TAG, "SHTC3 read ID CRC fail");
        return ESP_ERR_INVALID_CRC;
    }
    uint16_t id = ((uint16_t)rx[0] << 8) | rx[1];
    ESP_LOGI(TAG, "SHTC3 ID = 0x%04x", id);
    shtc3_sleep();
    return ESP_OK;
}

esp_err_t Shtc3_ReadTempHumi(float *temp_c, float *humi_pct)
{
    if (!s_shtc3) return ESP_ERR_INVALID_STATE;

    esp_err_t err = shtc3_wakeup();
    if (err != ESP_OK) return err;

    // 触发采样，需要 ~12ms
    err = shtc3_send_cmd(SHTC3_CMD_MEAS_T_RH);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SHTC3 meas cmd fail: %d", err);
        shtc3_sleep();
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t rx[6] = {0};
    i2c_master_bus_wait_all_done(s_bus, kBusTimeout);
    err = i2c_master_receive(s_shtc3, rx, 6, kXferTimeout);
    shtc3_sleep();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SHTC3 read data fail: %d", err);
        return err;
    }
    if (!shtc3_check_crc(rx, 2, rx[2]) || !shtc3_check_crc(&rx[3], 2, rx[5])) {
        ESP_LOGW(TAG, "SHTC3 data CRC fail");
        return ESP_ERR_INVALID_CRC;
    }
    uint16_t raw_t = ((uint16_t)rx[0] << 8) | rx[1];
    uint16_t raw_h = ((uint16_t)rx[3] << 8) | rx[4];

    // T = -45 + 175 * raw / 2^16  -   板级自热补偿
    if (temp_c)   *temp_c   = 175.0f * (float)raw_t / 65536.0f - 45.0f - (float)SHTC3_TEMP_OFFSET;
    // RH = raw / 2^16 * 100
    if (humi_pct) *humi_pct = 100.0f * (float)raw_h / 65536.0f;
    return ESP_OK;
}
