/*
 * stt_baidu.h - 百度语音识别API封装
 */
#ifndef __STT_BAIDU_H__
#define __STT_BAIDU_H__

#include <rtthread.h>
#include <stdint.h>
#include "stt_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* STT识别结果 */
typedef struct {
    char     text[STT_RESULT_MAX_LEN];  /* 识别文本 */
    int      err_no;                     /* 错误码(0=成功) */
    char     err_msg[64];               /* 错误信息 */
} stt_result_t;

/**
 * @brief 初始化百度STT模块(获取access_token)
 * @return RT_EOK成功
 */
rt_err_t stt_baidu_init(void);

/**
 * @brief 发送音频数据进行语音识别
 * @param wav_data  WAV格式音频数据(含头)
 * @param wav_len   数据长度
 * @param result    输出: 识别结果
 * @return RT_EOK成功
 */
rt_err_t stt_baidu_recognize(const uint8_t *wav_data, uint32_t wav_len,
                             stt_result_t *result);

/**
 * @brief 检查token是否有效
 * @return RT_TRUE有效
 */
rt_bool_t stt_baidu_token_valid(void);

#ifdef __cplusplus
}
#endif

#endif /* __STT_BAIDU_H__ */
