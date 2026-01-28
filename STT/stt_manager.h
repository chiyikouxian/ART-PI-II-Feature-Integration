/*
 * stt_manager.h - STT管理器：VAD录音 + 识别 + OLED显示
 */
#ifndef __STT_MANAGER_H__
#define __STT_MANAGER_H__

#include <rtthread.h>
#include "stt_baidu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* STT管理器状态 */
typedef enum {
    STT_STATE_IDLE = 0,         /* 空闲，等待语音 */
    STT_STATE_RECORDING,        /* VAD检测到语音，录音中 */
    STT_STATE_ENCODING,         /* 编码WAV */
    STT_STATE_UPLOADING,        /* 上传到百度API */
    STT_STATE_DISPLAYING,       /* 显示识别结果 */
    STT_STATE_ERROR             /* 出错 */
} stt_state_t;

/* STT结果回调 */
typedef void (*stt_result_callback_t)(const char *text);

/**
 * @brief 初始化STT管理器
 * @param callback  识别结果回调(可为NULL)
 * @return RT_EOK成功
 */
rt_err_t stt_manager_init(stt_result_callback_t callback);

/**
 * @brief 启动STT管理器(自动VAD检测+识别循环)
 * @return RT_EOK成功
 */
rt_err_t stt_manager_start(void);

/**
 * @brief 停止STT管理器
 * @return RT_EOK成功
 */
rt_err_t stt_manager_stop(void);

/**
 * @brief 获取当前状态
 */
stt_state_t stt_manager_get_state(void);

/**
 * @brief 获取最近一次识别结果文本
 */
const char *stt_manager_get_last_text(void);

/**
 * @brief 供audio_process回调调用 - 接收录音数据
 */
void stt_manager_feed_recording(const int32_t *pcm32, uint32_t samples,
                                uint32_t sample_rate);

#ifdef __cplusplus
}
#endif

#endif /* __STT_MANAGER_H__ */
