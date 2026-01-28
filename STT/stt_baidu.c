/*
 * stt_baidu.c - 百度语音识别API封装
 *
 * 百度语音REST API流程:
 * 1. 用API_KEY + SECRET_KEY获取access_token
 * 2. POST音频数据(JSON内嵌Base64或直接发送raw PCM/WAV)到ASR接口
 * 3. 解析JSON返回结果
 *
 * 本实现使用"原始音频数据上传"方式(非Base64)，节省内存和带宽。
 * POST URL: http://vop.baidu.com/server_api/?dev_pid=1537&cuid=xxx&token=xxx
 * Content-Type: audio/wav;rate=16000
 */
#include "stt_baidu.h"
#include "http_client.h"
#include <string.h>
#include <stdlib.h>

/* access_token缓存 */
static char g_access_token[HTTP_TOKEN_LEN] = {0};
static rt_bool_t g_token_valid = RT_FALSE;

/* 设备唯一标识 */
#define BAIDU_CUID  "art_pi2_stt_device"

/* dev_pid: 1537=普通话+标点 */
#define BAIDU_DEV_PID   "1537"

/* ==================== 内部: 简易JSON字段提取 ==================== */

/**
 * @brief 从JSON字符串中提取字符串值
 *        查找 "key":"value" 或 "key": "value"
 */
static rt_bool_t json_get_string(const char *json, const char *key,
                                  char *out, uint32_t out_size)
{
    char pattern[64];
    rt_snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    char *pos = strstr(json, pattern);
    if (pos == RT_NULL)
        return RT_FALSE;

    /* 跳过key和冒号 */
    pos += rt_strlen(pattern);
    while (*pos == ' ' || *pos == ':' || *pos == '\t') pos++;

    if (*pos != '"')
        return RT_FALSE;

    pos++; /* 跳过开头引号 */
    char *end = strchr(pos, '"');
    if (end == RT_NULL)
        return RT_FALSE;

    uint32_t len = end - pos;
    if (len >= out_size)
        len = out_size - 1;
    rt_memcpy(out, pos, len);
    out[len] = '\0';
    return RT_TRUE;
}

/**
 * @brief 从JSON中提取整数值
 *        查找 "key":123 或 "key": 123
 */
static rt_bool_t json_get_int(const char *json, const char *key, int *out)
{
    char pattern[64];
    rt_snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    char *pos = strstr(json, pattern);
    if (pos == RT_NULL)
        return RT_FALSE;

    pos += rt_strlen(pattern);
    while (*pos == ' ' || *pos == ':' || *pos == '\t') pos++;

    *out = atoi(pos);
    return RT_TRUE;
}

/**
 * @brief 从JSON数组 "result":["text"] 中提取第一个字符串
 */
static rt_bool_t json_get_first_array_string(const char *json, const char *key,
                                              char *out, uint32_t out_size)
{
    char pattern[64];
    rt_snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    char *pos = strstr(json, pattern);
    if (pos == RT_NULL)
        return RT_FALSE;

    /* 找到 '[' */
    pos = strchr(pos, '[');
    if (pos == RT_NULL)
        return RT_FALSE;

    /* 找到第一个 '"' */
    pos = strchr(pos, '"');
    if (pos == RT_NULL)
        return RT_FALSE;

    pos++; /* 跳过引号 */
    char *end = strchr(pos, '"');
    if (end == RT_NULL)
        return RT_FALSE;

    uint32_t len = end - pos;
    if (len >= out_size)
        len = out_size - 1;
    rt_memcpy(out, pos, len);
    out[len] = '\0';
    return RT_TRUE;
}

/* ==================== Token获取 ==================== */

rt_err_t stt_baidu_init(void)
{
    http_response_t resp;
    char path[256];

    rt_kprintf("[BaiduSTT] Requesting access_token...\n");

    /* GET http://aip.baidubce.com/oauth/2.0/token?
     *     grant_type=client_credentials&
     *     client_id=API_KEY&
     *     client_secret=SECRET_KEY
     */
    rt_snprintf(path, sizeof(path),
        "%s?grant_type=client_credentials&client_id=%s&client_secret=%s",
        BAIDU_TOKEN_PATH, BAIDU_API_KEY, BAIDU_SECRET_KEY);

    rt_err_t ret = http_get(BAIDU_TOKEN_HOST, BAIDU_TOKEN_PORT, path, &resp);
    if (ret != RT_EOK)
    {
        rt_kprintf("[BaiduSTT] Token request failed\n");
        return ret;
    }

    if (resp.status_code != 200 || resp.body == RT_NULL)
    {
        rt_kprintf("[BaiduSTT] Token HTTP error: %d\n", resp.status_code);
        http_response_free(&resp);
        return -RT_ERROR;
    }

    /* 从JSON提取access_token */
    if (json_get_string(resp.body, "access_token", g_access_token, sizeof(g_access_token)))
    {
        g_token_valid = RT_TRUE;
        rt_kprintf("[BaiduSTT] Token obtained: %.16s...\n", g_access_token);
    }
    else
    {
        rt_kprintf("[BaiduSTT] Token parse failed\n");
        rt_kprintf("[BaiduSTT] Response: %s\n", resp.body);
        ret = -RT_ERROR;
    }

    http_response_free(&resp);
    return ret;
}

/* ==================== 语音识别 ==================== */

rt_err_t stt_baidu_recognize(const uint8_t *wav_data, uint32_t wav_len,
                             stt_result_t *result)
{
    http_response_t resp;
    char path[256];
    rt_err_t ret;

    if (!g_token_valid)
    {
        rt_kprintf("[BaiduSTT] Token not valid, re-initializing...\n");
        ret = stt_baidu_init();
        if (ret != RT_EOK)
            return ret;
    }

    rt_memset(result, 0, sizeof(stt_result_t));

    /* 构造URL:
     * POST http://vop.baidu.com/server_api/?dev_pid=1537&cuid=xxx&token=xxx
     * Content-Type: audio/wav;rate=16000
     * Body: 原始WAV数据
     */
    rt_snprintf(path, sizeof(path),
        "%s?dev_pid=%s&cuid=%s&token=%s",
        BAIDU_ASR_PATH, BAIDU_DEV_PID, BAIDU_CUID, g_access_token);

    rt_kprintf("[BaiduSTT] Sending %d bytes audio...\n", wav_len);

    ret = http_post(BAIDU_ASR_HOST, BAIDU_ASR_PORT,
                    path,
                    wav_data, wav_len,
                    "audio/wav;rate=16000",
                    &resp);

    if (ret != RT_EOK)
    {
        rt_kprintf("[BaiduSTT] Request failed\n");
        result->err_no = -1;
        rt_strncpy(result->err_msg, "HTTP request failed", sizeof(result->err_msg) - 1);
        return ret;
    }

    if (resp.body == RT_NULL)
    {
        rt_kprintf("[BaiduSTT] Empty response\n");
        result->err_no = -2;
        rt_strncpy(result->err_msg, "Empty response", sizeof(result->err_msg) - 1);
        http_response_free(&resp);
        return -RT_ERROR;
    }

    rt_kprintf("[BaiduSTT] Response(%d): %s\n", resp.status_code, resp.body);

    /* 解析JSON结果:
     * 成功: {"corpus_no":"...","err_msg":"success.","err_no":0,"result":["识别文本"],"sn":"..."}
     * 失败: {"err_msg":"...","err_no":3301,"sn":"..."}
     */
    json_get_int(resp.body, "err_no", &result->err_no);
    json_get_string(resp.body, "err_msg", result->err_msg, sizeof(result->err_msg));

    if (result->err_no == 0)
    {
        json_get_first_array_string(resp.body, "result", result->text, sizeof(result->text));
        rt_kprintf("[BaiduSTT] Result: %s\n", result->text);
    }
    else
    {
        rt_kprintf("[BaiduSTT] Error %d: %s\n", result->err_no, result->err_msg);
        /* Token过期(err_no=3302)则标记无效 */
        if (result->err_no == 3302)
            g_token_valid = RT_FALSE;
    }

    http_response_free(&resp);
    return (result->err_no == 0) ? RT_EOK : -RT_ERROR;
}

rt_bool_t stt_baidu_token_valid(void)
{
    return g_token_valid;
}
