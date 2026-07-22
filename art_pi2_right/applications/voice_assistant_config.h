/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2024-10-16     AI Assistant first version - Voice Assistant Configuration
 */

#ifndef __VOICE_ASSISTANT_CONFIG_H__
#define __VOICE_ASSISTANT_CONFIG_H__

/* ==================== AI Service Config ==================== */

/* AI Service Provider
 * 0 - Baidu AI (https://ai.baidu.com/)
 * 1 - iFlytek  (https://www.xfyun.cn/)
 * 2 - Aliyun   (https://www.aliyun.com/)
 * 3 - Custom
 */
#define AI_SERVICE_PROVIDER     1  /* iFlytek */

/* Baidu AI Config */
#define BAIDU_API_KEY           "your_baidu_api_key_here"
#define BAIDU_SECRET_KEY        "your_baidu_secret_key_here"
#define BAIDU_APP_ID            "your_baidu_app_id_here"
#define BAIDU_STT_URL           "http://vop.baidu.com/server_api"

/* iFlytek Config */
#define XFYUN_API_KEY           "c934cd65a7a2da392cf718c1b7687b11"
#define XFYUN_API_SECRET        "ZTg4YzEwM2M0ZTIzMzIzZjE1ZmFmYWU5"
#define XFYUN_APP_ID            "c050bb08"
/* STT URL is built at runtime by voice_assistant.c using
 *   server_config_get_stt_ip() + server_config_get_stt_port()
 * Both getters are fixed read-only mirrors of ROCK_SERVER_IP /
 * ROCK_STT_SERVER_PORT (rock_config.h) — there is no runtime path to
 * override the STT host/port. `va_reload_stt` only re-initialises the
 * AI service against this same fixed address; it cannot change it.
 */

/* Aliyun Config */
#define ALIYUN_API_KEY          "your_aliyun_api_key_here"
#define ALIYUN_API_SECRET       "your_aliyun_api_secret_here"
#define ALIYUN_APP_ID           "your_aliyun_app_id_here"
#define ALIYUN_STT_URL          "http://nls-gateway.cn-shanghai.aliyuncs.com/stream/v1/asr"

/* Custom API Config */
#define CUSTOM_API_KEY          "your_custom_api_key_here"
#define CUSTOM_API_SECRET       "your_custom_api_secret_here"
#define CUSTOM_APP_ID           "your_custom_app_id_here"
#define CUSTOM_API_URL          "http://your-custom-api-server.com/api/voice"

/* VAD parameters (VAD_THRESHOLD, VAD_HANGOVER_FRAMES,
 * AUDIO_PROCESS_MAX_RECORD_SEC) live in audio_process.h and are
 * the actual source of truth at runtime. */

#endif /* __VOICE_ASSISTANT_CONFIG_H__ */
