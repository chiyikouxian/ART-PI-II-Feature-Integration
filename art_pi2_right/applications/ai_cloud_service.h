/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2024-10-16     AI Assistant first version - AI Cloud Service Module
 */

#ifndef __AI_CLOUD_SERVICE_H__
#define __AI_CLOUD_SERVICE_H__

#include <rtthread.h>

/* AI服务提供商 */
typedef enum {
    AI_SERVICE_BAIDU = 0,      /* 百度AI */
    AI_SERVICE_XFYUN,          /* 讯飞 */
    AI_SERVICE_ALIYUN,         /* 阿里云 */
    AI_SERVICE_CUSTOM          /* 自定义 */
} ai_service_provider_t;

/* AI服务配置 */
typedef struct {
    ai_service_provider_t provider;
    char api_key[128];
    char api_secret[128];
    char app_id[64];
    char api_url[256];
} ai_service_config_t;

/* AI响应结构 */
typedef struct {
    int error_code;
    char *text_result;        /* 识别出的文本 */
    char *audio_result;       /* AI回复的音频数据 */
    uint32_t audio_len;       /* 音频数据长度 */
    char *error_msg;          /* 错误信息 */
} ai_response_t;

/* AI云服务接口 */
int ai_cloud_service_init(ai_service_config_t *config);
int ai_cloud_service_speech_to_text(const uint8_t *audio_data, uint32_t audio_len, 
                                     ai_response_t *response);
int ai_cloud_service_text_to_speech(const char *text, ai_response_t *response);
int ai_cloud_service_full_duplex(const uint8_t *audio_data, uint32_t audio_len,
                                  ai_response_t *response);
void ai_cloud_service_free_response(ai_response_t *response);

#endif /* __AI_CLOUD_SERVICE_H__ */

