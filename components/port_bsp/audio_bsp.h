// audio_bsp —— 板载音频底座（ES8311 DAC 出声 + ES7210 双麦阵列收音）
//
// 引脚见 main/user_config.h（来自 Document/rlcd 引脚表）：I2S 时钟共享，
// 数据线 DOUT→ES8311（播放）/ ASDOUT→ESP32（收音）；I2C 控制复用 SHTC3
// 所在总线（i2c_bsp）。PA 使能脚（GPIO46）由本模块自管，只在通话期间打开。
//
// 职责边界：本模块只提供「16kHz/16bit/单声道 PCM 的阻塞读写」这一层薄底座。
// opus 编解码 + WebSocket 音频流 + 按键说话逻辑都在 net_xiaozhi.c；它通过
// NetBsp_XiaozhiSetAudioOps() 拿到本模块的函数指针（反向注册，避免
// net_bsp → port_bsp 的组件依赖环，方向与 button_bsp → net_bsp 一致）。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// 初始化 codec + I2S。I2C 总线必须已由 I2cBus_Init() 建好。
// 自动探测 ES8311/ES7210 的 I2C 地址（ strap 未知，逐个 probe）。
// 失败（板上无音频芯片/总线异常）时返回错误且 AudioBsp_Ready()==false，
// xiaozhi 自动回落到纯文本对话。
esp_err_t AudioBsp_Init(void);

// true = 底座可用（codec 探测成功）
bool AudioBsp_Ready(void);

// 功放使能（高电平开）。仅在一轮语音会话期间打开，避免上电噗声。
void AudioBsp_PaEnable(bool on);

// 打开一条全双工通话通路（I2S tx/rx enable + codec open + PA on）
esp_err_t AudioBsp_TalkStart(void);

// 关闭通话通路（codec close + I2S disable + PA off）
void AudioBsp_TalkStop(void);

// 阻塞写 PCM 到扬声器。pcm/samples：16kHz 16bit 单声道。返回写入的采样数（<0 失败）。
int AudioBsp_SpeakerWrite(const int16_t *pcm, int samples);

// 阻塞读一帧麦克风 PCM（16kHz 16bit 单声道，取 ES7210 MIC1）。
// 返回读到的采样数（<0 失败）。
int AudioBsp_MicRead(int16_t *pcm, int samples);

#ifdef __cplusplus
}
#endif
