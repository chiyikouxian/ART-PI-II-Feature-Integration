/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2024-10-16     AI Assistant first version - AI Cloud Service Implementation
 * 2025-xx-xx     Migration    STT-only version for art_pi2_right
 */

#include <rtthread.h>
#include <string.h>
#include <stdlib.h>
#include "ai_cloud_service.h"
#include "web_client.h"

#define DBG_TAG "ai.cloud"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* AI服务配置 */
static ai_service_config_t g_ai_config = {0};
static rt_bool_t g_ai_initialized = RT_FALSE;

/* Base64编码表 */
static const char base64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Base64编码 */
static char *base64_encode(const uint8_t *data, uint32_t data_len)
{
    uint32_t encoded_len = ((data_len + 2) / 3) * 4;
    char *encoded = (char *)rt_malloc(encoded_len + 1);

    if (encoded == RT_NULL)
    {
        return RT_NULL;
    }

    uint32_t i, j;
    for (i = 0, j = 0; i < data_len;)
    {
        uint32_t octet_a = i < data_len ? data[i++] : 0;
        uint32_t octet_b = i < data_len ? data[i++] : 0;
        uint32_t octet_c = i < data_len ? data[i++] : 0;
        uint32_t triple = (octet_a << 16) + (octet_b << 8) + octet_c;

        encoded[j++] = base64_table[(triple >> 18) & 0x3F];
        encoded[j++] = base64_table[(triple >> 12) & 0x3F];
        encoded[j++] = base64_table[(triple >> 6) & 0x3F];
        encoded[j++] = base64_table[triple & 0x3F];
    }

    /* 添加填充 */
    for (i = 0; i < (3 - data_len % 3) % 3; i++)
    {
        encoded[encoded_len - 1 - i] = '=';
    }

    encoded[encoded_len] = '\0';
    return encoded;
}

/* 初始化AI云服务 */
int ai_cloud_service_init(ai_service_config_t *config)
{
    if (config == RT_NULL)
    {
        LOG_E("Invalid configuration");
        return -RT_EINVAL;
    }

    rt_memcpy(&g_ai_config, config, sizeof(ai_service_config_t));
    g_ai_initialized = RT_TRUE;

    LOG_I("AI cloud service initialized (Provider: %d)", config->provider);

    return RT_EOK;
}

/* 解析STT JSON响应，提取识别文本 */
static void parse_stt_response(http_response_t *http_resp, ai_response_t *response)
{
    /* 期望返回格式为 {"result":["识别文本"]} */
    char *result_start = strstr(http_resp->body, "\"result\"");
    if (result_start)
    {
        char *text_start = strchr(result_start, '[');
        if (text_start)
        {
            text_start = strchr(text_start, '\"');
            if (text_start)
            {
                text_start++;
                char *text_end = strchr(text_start, '\"');
                if (text_end)
                {
                    int text_len = text_end - text_start;
                    response->text_result = (char *)rt_malloc(text_len + 1);
                    if (response->text_result)
                    {
                        rt_memcpy(response->text_result, text_start, text_len);
                        response->text_result[text_len] = '\0';
                        response->error_code = 0;
                        LOG_I("Recognized text: %s", response->text_result);
                    }
                }
            }
        }
    }

    if (response->text_result == RT_NULL)
    {
        LOG_W("Failed to parse response, raw: %s", http_resp->body);
        response->error_code = -1;
        response->error_msg = rt_strdup("Failed to parse response");
    }
}

/* 语音识别（Speech to Text）*/
int ai_cloud_service_speech_to_text(const uint8_t *audio_data, uint32_t audio_len,
                                     ai_response_t *response)
{
    http_response_t http_resp;
    int ret = -RT_ERROR;

    if (!g_ai_initialized)
    {
        LOG_E("AI service not initialized");
        return -RT_ERROR;
    }

    if (audio_data == RT_NULL || audio_len == 0 || response == RT_NULL)
    {
        LOG_E("Invalid parameters");
        return -RT_EINVAL;
    }

    rt_memset(response, 0, sizeof(ai_response_t));

    LOG_I("Starting speech to text (audio_len: %d bytes)", audio_len);

    if (g_ai_config.provider == AI_SERVICE_XFYUN)
    {
        /* 讯飞代理模式：直接发送原始 PCM 数据，无需 Base64 编码
         * 代理服务器 (xfyun_proxy.py) 支持接收 application/octet-stream 的原始 PCM
         * 这样可以节省 ~150KB 内存（无需 base64 + JSON 缓冲区） */
        LOG_I("Sending raw PCM to iFlytek proxy (%d bytes)", audio_len);

        ret = web_client_post(g_ai_config.api_url,
                              (const char *)audio_data, audio_len,
                              "application/octet-stream", &http_resp);
    }
    else
    {
        /* 百度/阿里云/自定义：使用 Base64 + JSON 格式 */
        char *json_data = RT_NULL;
        char *audio_base64 = RT_NULL;

        audio_base64 = base64_encode(audio_data, audio_len);
        if (audio_base64 == RT_NULL)
        {
            LOG_E("Failed to encode audio data");
            return -RT_ERROR;
        }

        uint32_t base64_len = strlen(audio_base64);
        json_data = (char *)rt_malloc(base64_len + 512);
        if (json_data == RT_NULL)
        {
            LOG_E("Failed to allocate JSON buffer");
            rt_free(audio_base64);
            return -RT_ERROR;
        }

        if (g_ai_config.provider == AI_SERVICE_BAIDU)
        {
            rt_snprintf(json_data, base64_len + 512,
                        "{\"format\":\"pcm\",\"rate\":16000,\"channel\":1,"
                        "\"cuid\":\"%s\",\"token\":\"%s\",\"speech\":\"%s\",\"len\":%d}",
                        g_ai_config.app_id, g_ai_config.api_key, audio_base64, audio_len);
        }
        else
        {
            rt_snprintf(json_data, base64_len + 512,
                        "{\"audio_data\":\"%s\",\"audio_len\":%d,\"format\":\"pcm\","
                        "\"sample_rate\":16000,\"channels\":1}",
                        audio_base64, audio_len);
        }

        rt_free(audio_base64);

        ret = web_client_post(g_ai_config.api_url, json_data, strlen(json_data),
                              "application/json", &http_resp);

        rt_free(json_data);
    }

    if (ret == RT_EOK && http_resp.status_code == 200)
    {
        LOG_I("Speech to text successful");
        parse_stt_response(&http_resp, response);
        web_client_free_response(&http_resp);
    }
    else
    {
        LOG_E("HTTP request failed (status: %d)", http_resp.status_code);
        response->error_code = http_resp.status_code;
        response->error_msg = rt_strdup("HTTP request failed");
        web_client_free_response(&http_resp);
    }

    return ret;
}

/* 语音合成（Text to Speech）- STT-only版本，不支持 */
int ai_cloud_service_text_to_speech(const char *text, ai_response_t *response)
{
    LOG_W("TTS not supported in STT-only build");
    return -RT_ERROR;
}

/* 全双工模式 - STT-only版本，不支持 */
int ai_cloud_service_full_duplex(const uint8_t *audio_data, uint32_t audio_len,
                                  ai_response_t *response)
{
    LOG_W("Full duplex not supported in STT-only build");
    return -RT_ERROR;
}

/* 释放响应数据 */
void ai_cloud_service_free_response(ai_response_t *response)
{
    if (response)
    {
        if (response->text_result)
        {
            rt_free(response->text_result);
            response->text_result = RT_NULL;
        }
        if (response->audio_result)
        {
            rt_free(response->audio_result);
            response->audio_result = RT_NULL;
        }
        if (response->error_msg)
        {
            rt_free(response->error_msg);
            response->error_msg = RT_NULL;
        }
        response->audio_len = 0;
        response->error_code = 0;
    }
}
