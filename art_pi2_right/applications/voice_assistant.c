/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2024-10-16     AI Assistant first version - Voice Assistant Implementation
 * 2025-xx-xx     Migration    STT-only version for art_pi2_right
 */

#include <rtthread.h>
#include <rtdevice.h>
#include "voice_assistant.h"
#include "voice_assistant_config.h"
#include "server_config.h"
#include "audio_capture.h"
#include "audio_process.h"
#include "ai_cloud_service.h"
#include "../UART/uart_oled_thread.h"

#define DBG_TAG "voice.assistant"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* 语音助手控制结构 */
static struct {
    voice_assistant_state_t state;
    rt_thread_t main_thread;
    rt_sem_t trigger_sem;
    rt_sem_t speech_ready_sem;      /* VAD录音完成信号量 */
    rt_bool_t running;
    rt_bool_t initialized;
    /* VAD录音结果 (由回调填充) */
    int16_t *pcm16_buffer;          /* 转换后的16位PCM数据 */
    uint32_t pcm16_size;            /* PCM数据大小(字节) */
} voice_assistant_ctrl = {
    .state = VOICE_ASSISTANT_IDLE,
    .main_thread = RT_NULL,
    .trigger_sem = RT_NULL,
    .speech_ready_sem = RT_NULL,
    .running = RT_FALSE,
    .initialized = RT_FALSE,
    .pcm16_buffer = RT_NULL,
    .pcm16_size = 0
};

/* 前置声明 */
static void speech_data_handler(audio_recording_t *recording);

/**
 * @brief VAD语音录音完成回调 (由audio_process模块在检测到语音结束时调用)
 *        将32位PCM数据转换为16位PCM，通过信号量通知主线程
 */
static void speech_data_handler(audio_recording_t *recording)
{
    if (recording == RT_NULL || recording->data == RT_NULL || recording->size == 0)
    {
        LOG_W("Empty recording data");
        return;
    }

    uint32_t duration_ms = (recording->end_time - recording->start_time)
                           * 1000 / RT_TICK_PER_SECOND;
    LOG_I("VAD speech callback: %d samples, %d ms", recording->size, duration_ms);

    /* 释放上一次的缓冲区 */
    if (voice_assistant_ctrl.pcm16_buffer != RT_NULL)
    {
        rt_free(voice_assistant_ctrl.pcm16_buffer);
        voice_assistant_ctrl.pcm16_buffer = RT_NULL;
    }

    /* 分配16位PCM缓冲区 */
    voice_assistant_ctrl.pcm16_buffer = rt_malloc(recording->size * sizeof(int16_t));
    if (voice_assistant_ctrl.pcm16_buffer == RT_NULL)
    {
        LOG_E("Failed to allocate PCM16 buffer");
        voice_assistant_ctrl.pcm16_size = 0;
        rt_sem_release(voice_assistant_ctrl.speech_ready_sem);
        return;
    }

    /* 将32位PCM转换为16位PCM (24-bit已右移8位, 再右移8位变16位) */
    audio_convert_32to16(recording->data, voice_assistant_ctrl.pcm16_buffer, recording->size);
    voice_assistant_ctrl.pcm16_size = recording->size * sizeof(int16_t);

    LOG_I("Converted to 16-bit PCM: %d bytes", voice_assistant_ctrl.pcm16_size);

    /* 通知主线程录音数据已就绪 */
    rt_sem_release(voice_assistant_ctrl.speech_ready_sem);
}

/* 语音助手主线程 - 持续监听模式，音量超过阈值自动触发识别 */
static void voice_assistant_thread_entry(void *parameter)
{
    ai_response_t ai_response;
    int ret;
    uint32_t total_read;
    uint8_t *audio_buffer;

    LOG_I("Voice assistant thread started");

    /* 启动INMP441硬件采集 */
    ret = audio_capture_start();
    if (ret != RT_EOK)
    {
        LOG_E("Failed to start audio capture");
        return;
    }

    /* 启动VAD语音处理 */
    ret = audio_process_start();
    if (ret != RT_EOK)
    {
        LOG_E("Failed to start audio processing");
        audio_capture_stop();
        return;
    }

    voice_assistant_ctrl.state = VOICE_ASSISTANT_LISTENING;
    LOG_I("Waiting for speech...");

    while (voice_assistant_ctrl.running)
    {
        /* 等待VAD检测到语音 */
        ret = rt_sem_take(voice_assistant_ctrl.speech_ready_sem, RT_WAITING_FOREVER);

        if (!voice_assistant_ctrl.running)
            break;

        if (ret != RT_EOK)
            continue;

        /* 取出录音数据 (取得所有权，防止回调释放) */
        total_read = voice_assistant_ctrl.pcm16_size;
        audio_buffer = (uint8_t *)voice_assistant_ctrl.pcm16_buffer;
        voice_assistant_ctrl.pcm16_buffer = RT_NULL;
        voice_assistant_ctrl.pcm16_size = 0;

        /* 立刻重启采集和VAD (STT处理期间继续监听) */
        rt_thread_mdelay(100);
        audio_capture_start();
        audio_process_start();

        /* 检查数据有效性 */
        if (audio_buffer == RT_NULL || total_read < 3200)
        {
            /* 3200 bytes = 1600 samples = 100ms，太短的音频跳过 */
            if (audio_buffer != RT_NULL)
                rt_free(audio_buffer);
            continue;
        }

        LOG_I("Speech captured: %d bytes, sending to STT...", total_read);

        /* 处理语音数据 (此时采集已在后台继续运行) */
        voice_assistant_ctrl.state = VOICE_ASSISTANT_PROCESSING;

        rt_memset(&ai_response, 0, sizeof(ai_response_t));

        ret = ai_cloud_service_speech_to_text(audio_buffer, total_read, &ai_response);

        /* 释放音频数据 (STT已完成或失败) */
        rt_free(audio_buffer);
        audio_buffer = RT_NULL;

        if (ret != RT_EOK || ai_response.error_code != 0)
        {
            LOG_E("STT failed: %s",
                  ai_response.error_msg ? ai_response.error_msg : "Unknown error");
            ai_cloud_service_free_response(&ai_response);
            voice_assistant_ctrl.state = VOICE_ASSISTANT_LISTENING;
            continue;
        }

        /* 检查识别结果是否为空 */
        if (ai_response.text_result == RT_NULL ||
            ai_response.text_result[0] == '\0')
        {
            LOG_D("Empty STT result, skipping");
            ai_cloud_service_free_response(&ai_response);
            voice_assistant_ctrl.state = VOICE_ASSISTANT_LISTENING;
            continue;
        }

        /* 显示识别结果 */
        rt_kprintf("\n[Voice] You said: %s\n", ai_response.text_result);

        /* 将识别结果显示到OLED屏幕 */
        oled_show_stt_result(ai_response.text_result);

        ai_cloud_service_free_response(&ai_response);
        voice_assistant_ctrl.state = VOICE_ASSISTANT_LISTENING;
    }

    /* 清理 */
    audio_process_stop();
    audio_capture_stop();
    if (voice_assistant_ctrl.pcm16_buffer != RT_NULL)
    {
        rt_free(voice_assistant_ctrl.pcm16_buffer);
        voice_assistant_ctrl.pcm16_buffer = RT_NULL;
    }
    voice_assistant_ctrl.state = VOICE_ASSISTANT_IDLE;

    LOG_I("Voice assistant thread stopped");
}

/* 初始化语音助手 */
int voice_assistant_init(void)
{
    ai_service_config_t ai_config;
    int ret;

    if (voice_assistant_ctrl.initialized)
    {
        LOG_W("Voice assistant already initialized");
        return RT_EOK;
    }

    LOG_I("Initializing voice assistant...");

    /* 初始化音频采集 (INMP441 SAI驱动) */
    ret = audio_capture_init();
    if (ret != RT_EOK)
    {
        LOG_E("Failed to initialize audio capture");
        return ret;
    }

    /* 初始化音频处理模块 (VAD语音活动检测) */
    ret = audio_process_init(speech_data_handler);
    if (ret != RT_EOK)
    {
        LOG_E("Failed to initialize audio processing");
        return ret;
    }

    /* 配置AI服务 */
    rt_memset(&ai_config, 0, sizeof(ai_config));
    ai_config.provider = AI_SERVICE_PROVIDER;

#if AI_SERVICE_PROVIDER == 0  /* 百度AI */
    strncpy(ai_config.api_key, BAIDU_API_KEY, sizeof(ai_config.api_key) - 1);
    strncpy(ai_config.api_secret, BAIDU_SECRET_KEY, sizeof(ai_config.api_secret) - 1);
    strncpy(ai_config.app_id, BAIDU_APP_ID, sizeof(ai_config.app_id) - 1);
    strncpy(ai_config.api_url, BAIDU_STT_URL, sizeof(ai_config.api_url) - 1);
#elif AI_SERVICE_PROVIDER == 1  /* 讯飞 */
    strncpy(ai_config.api_key, XFYUN_API_KEY, sizeof(ai_config.api_key) - 1);
    strncpy(ai_config.api_secret, XFYUN_API_SECRET, sizeof(ai_config.api_secret) - 1);
    strncpy(ai_config.app_id, XFYUN_APP_ID, sizeof(ai_config.app_id) - 1);
    /* 动态构建STT URL */
    {
        char ip_buf[16];
        int port;
        server_config_get_stt_ip(ip_buf, sizeof(ip_buf));
        port = server_config_get_stt_port();
        rt_snprintf(ai_config.api_url, sizeof(ai_config.api_url) - 1,
                    "http://%s:%d/stt", ip_buf, port);
    }
#elif AI_SERVICE_PROVIDER == 2  /* 阿里云 */
    strncpy(ai_config.api_key, ALIYUN_API_KEY, sizeof(ai_config.api_key) - 1);
    strncpy(ai_config.api_secret, ALIYUN_API_SECRET, sizeof(ai_config.api_secret) - 1);
    strncpy(ai_config.app_id, ALIYUN_APP_ID, sizeof(ai_config.app_id) - 1);
    strncpy(ai_config.api_url, ALIYUN_STT_URL, sizeof(ai_config.api_url) - 1);
#else  /* 自定义 */
    strncpy(ai_config.api_key, CUSTOM_API_KEY, sizeof(ai_config.api_key) - 1);
    strncpy(ai_config.api_secret, CUSTOM_API_SECRET, sizeof(ai_config.api_secret) - 1);
    strncpy(ai_config.app_id, CUSTOM_APP_ID, sizeof(ai_config.app_id) - 1);
    strncpy(ai_config.api_url, CUSTOM_API_URL, sizeof(ai_config.api_url) - 1);
#endif

    /* 初始化AI云服务 */
    ret = ai_cloud_service_init(&ai_config);
    if (ret != RT_EOK)
    {
        LOG_E("Failed to initialize AI cloud service");
        return ret;
    }

    /* 创建触发信号量 */
    voice_assistant_ctrl.trigger_sem = rt_sem_create("va_trigger", 0, RT_IPC_FLAG_FIFO);
    if (voice_assistant_ctrl.trigger_sem == RT_NULL)
    {
        LOG_E("Failed to create trigger semaphore");
        return -RT_ERROR;
    }

    /* 创建VAD录音完成信号量 */
    voice_assistant_ctrl.speech_ready_sem = rt_sem_create("va_speech", 0, RT_IPC_FLAG_FIFO);
    if (voice_assistant_ctrl.speech_ready_sem == RT_NULL)
    {
        LOG_E("Failed to create speech ready semaphore");
        return -RT_ERROR;
    }

    voice_assistant_ctrl.initialized = RT_TRUE;
    voice_assistant_ctrl.state = VOICE_ASSISTANT_IDLE;

    LOG_I("Voice assistant initialized successfully");

    return RT_EOK;
}

/* 启动语音助手 */
int voice_assistant_start(void)
{
    if (!voice_assistant_ctrl.initialized)
    {
        LOG_E("Voice assistant not initialized");
        return -RT_ERROR;
    }

    if (voice_assistant_ctrl.running)
    {
        LOG_W("Voice assistant already running");
        return RT_EOK;
    }

    voice_assistant_ctrl.running = RT_TRUE;

    /* 创建语音助手主线程 */
    voice_assistant_ctrl.main_thread = rt_thread_create("voice_asst",
                                                         voice_assistant_thread_entry,
                                                         RT_NULL,
                                                         4096,
                                                         10,
                                                         10);
    if (voice_assistant_ctrl.main_thread == RT_NULL)
    {
        LOG_E("Failed to create voice assistant thread");
        voice_assistant_ctrl.running = RT_FALSE;
        return -RT_ERROR;
    }

    rt_thread_startup(voice_assistant_ctrl.main_thread);

    LOG_I("Voice assistant started (threshold=%u, min_speech=%d frames, hangover=1s)",
          VAD_THRESHOLD, VAD_MIN_SPEECH_FRAMES);

    return RT_EOK;
}

/* 停止语音助手 */
int voice_assistant_stop(void)
{
    if (!voice_assistant_ctrl.running)
    {
        return RT_EOK;
    }

    /* 停止音频处理 */
    audio_process_stop();
    audio_capture_stop();

    voice_assistant_ctrl.running = RT_FALSE;

    /* 释放信号量以退出线程 */
    rt_sem_release(voice_assistant_ctrl.trigger_sem);
    rt_sem_release(voice_assistant_ctrl.speech_ready_sem);

    /* 等待线程结束 */
    rt_thread_mdelay(500);

    LOG_I("Voice assistant stopped");

    return RT_EOK;
}

/* 获取语音助手状态 */
voice_assistant_state_t voice_assistant_get_state(void)
{
    return voice_assistant_ctrl.state;
}

/* 手动触发语音识别 */
int voice_assistant_trigger(void)
{
    if (!voice_assistant_ctrl.initialized || !voice_assistant_ctrl.running)
    {
        LOG_E("Voice assistant not ready");
        return -RT_ERROR;
    }

    if (voice_assistant_ctrl.state != VOICE_ASSISTANT_IDLE)
    {
        LOG_W("Voice assistant is busy (state: %d)", voice_assistant_ctrl.state);
        return -RT_ERROR;
    }

    LOG_I("Voice assistant triggered");
    rt_sem_release(voice_assistant_ctrl.trigger_sem);

    return RT_EOK;
}

/* 导出MSH命令 */
#ifdef FINSH_USING_MSH

static int cmd_va_init(int argc, char **argv)
{
    return voice_assistant_init();
}
MSH_CMD_EXPORT_ALIAS(cmd_va_init, va_init, Initialize voice assistant);

static int cmd_va_start(int argc, char **argv)
{
    return voice_assistant_start();
}
MSH_CMD_EXPORT_ALIAS(cmd_va_start, va_start, Start voice assistant);

static int cmd_va_stop(int argc, char **argv)
{
    return voice_assistant_stop();
}
MSH_CMD_EXPORT_ALIAS(cmd_va_stop, va_stop, Stop voice assistant);

static int cmd_va_trigger(int argc, char **argv)
{
    return voice_assistant_trigger();
}
MSH_CMD_EXPORT_ALIAS(cmd_va_trigger, va_trigger, Trigger voice recognition);

static int cmd_va_status(int argc, char **argv)
{
    const char *state_str[] = {
        "IDLE",
        "LISTENING",
        "PROCESSING",
        "SPEAKING",
        "ERROR"
    };

    rt_kprintf("Voice Assistant Status:\n");
    rt_kprintf("  Initialized: %s\n", voice_assistant_ctrl.initialized ? "Yes" : "No");
    rt_kprintf("  Running: %s\n", voice_assistant_ctrl.running ? "Yes" : "No");
    rt_kprintf("  State: %s\n", state_str[voice_assistant_ctrl.state]);

    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_va_status, va_status, Show voice assistant status);

static int cmd_va_reload_stt(int argc, char **argv)
{
    char ip_buf[16];
    int port;
    ai_service_config_t ai_config;

    if (!voice_assistant_ctrl.initialized)
    {
        rt_kprintf("Voice assistant not initialized\n");
        return -1;
    }

    /* 重新读取STT配置 */
    rt_memset(&ai_config, 0, sizeof(ai_config));
    ai_config.provider = AI_SERVICE_PROVIDER;

#if AI_SERVICE_PROVIDER == 1  /* 讯飞 */
    strncpy(ai_config.api_key, XFYUN_API_KEY, sizeof(ai_config.api_key) - 1);
    strncpy(ai_config.api_secret, XFYUN_API_SECRET, sizeof(ai_config.api_secret) - 1);
    strncpy(ai_config.app_id, XFYUN_APP_ID, sizeof(ai_config.app_id) - 1);

    /* 动态构建STT URL */
    server_config_get_stt_ip(ip_buf, sizeof(ip_buf));
    port = server_config_get_stt_port();
    rt_snprintf(ai_config.api_url, sizeof(ai_config.api_url) - 1,
                "http://%s:%d/stt", ip_buf, port);

    rt_kprintf("Reloading STT config:\n");
    rt_kprintf("  STT Server: %s:%d\n", ip_buf, port);
    rt_kprintf("  STT URL: %s\n", ai_config.api_url);

    /* 重新初始化AI云服务 */
    if (ai_cloud_service_init(&ai_config) != RT_EOK)
    {
        rt_kprintf("Failed to reload STT config\n");
        return -1;
    }

    rt_kprintf("STT config reloaded successfully\n");
#else
    rt_kprintf("STT reload only supported for iFlytek provider\n");
#endif

    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_va_reload_stt, va_reload_stt, Reload STT server config);

#endif
