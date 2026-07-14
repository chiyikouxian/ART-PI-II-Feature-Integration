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
#define BAIDU_TTS_URL           "http://tsn.baidu.com/text2audio"

/* iFlytek Config */
#define XFYUN_API_KEY           "c934cd65a7a2da392cf718c1b7687b11"
#define XFYUN_API_SECRET        "ZTg4YzEwM2M0ZTIzMzIzZjE1ZmFmYWU5"
#define XFYUN_APP_ID            "c050bb08"
/* STT URL: Point to PC proxy server running tools/xfyun_proxy.py
 * Change the IP to your PC's LAN IP address */
#define XFYUN_STT_URL           "http://192.168.92.92:8080/stt"
#define XFYUN_TTS_URL           "http://api.xfyun.cn/v1/service/v1/tts"

/* Aliyun Config */
#define ALIYUN_API_KEY          "your_aliyun_api_key_here"
#define ALIYUN_API_SECRET       "your_aliyun_api_secret_here"
#define ALIYUN_APP_ID           "your_aliyun_app_id_here"
#define ALIYUN_STT_URL          "http://nls-gateway.cn-shanghai.aliyuncs.com/stream/v1/asr"
#define ALIYUN_TTS_URL          "http://nls-gateway.cn-shanghai.aliyuncs.com/stream/v1/tts"

/* Custom API Config */
#define CUSTOM_API_KEY          "your_custom_api_key_here"
#define CUSTOM_API_SECRET       "your_custom_api_secret_here"
#define CUSTOM_APP_ID           "your_custom_app_id_here"
#define CUSTOM_API_URL          "http://your-custom-api-server.com/api/voice"

/* ==================== Audio Config ==================== */

/* Sample Rate (Hz) */
#define VOICE_SAMPLE_RATE       16000

/* Channels */
#define VOICE_CHANNELS          1

/* Bits per sample */
#define VOICE_BITS_PER_SAMPLE   16

/* Max recording duration (seconds) - VAD will auto-stop when speech ends */
#define VOICE_RECORD_DURATION   5

/* Audio buffer size (bytes) */
#define VOICE_BUFFER_SIZE       (VOICE_SAMPLE_RATE * VOICE_CHANNELS * (VOICE_BITS_PER_SAMPLE / 8) * VOICE_RECORD_DURATION)

/* ==================== Wakeup Config ==================== */

/* Wakeup word detection (set to 0 for manual trigger only) */
#define VOICE_WAKEUP_ENABLE     0

/* Wakeup word */
#define VOICE_WAKEUP_WORD       "HiXiaoShi"

/* VAD (Voice Activity Detection) - integrated via audio_process module
 * See audio_process.h for VAD parameters:
 *   VAD_THRESHOLD, VAD_HANGOVER_FRAMES, AUDIO_PROCESS_MAX_RECORD_SEC */
#define VOICE_VAD_ENABLE        1

/* Silence threshold */
#define VOICE_SILENCE_THRESHOLD 100

/* Silence duration (ms) */
#define VOICE_SILENCE_DURATION  1000

/* ==================== Network Config ==================== */

/* WiFi SSID */
#define WIFI_SSID               "baohan"

/* WiFi Password */
#define WIFI_PASSWORD           "88888887"

/* Network timeout (seconds) */
#define NETWORK_TIMEOUT         30

/* ==================== Debug Config ==================== */

/* Log level: 0-ERROR, 1-WARNING, 2-INFO, 3-DEBUG */
#define VOICE_ASSISTANT_LOG_LEVEL   2

/* Save audio file to SD card (for debugging) */
#define VOICE_SAVE_AUDIO_FILE       0

/* Audio file save path */
#define VOICE_AUDIO_FILE_PATH       "/sdcard/voice_record.pcm"

/* ==================== Feature Switches ==================== */

/* Enable speech-to-text */
#define VOICE_STT_ENABLE        1

/* Enable text-to-speech (requires HTTPS proxy, currently not supported) */
#define VOICE_TTS_ENABLE        0

/* Enable full duplex mode (STT+TTS, set to 0 for STT only) */
#define VOICE_FULL_DUPLEX_ENABLE    0

/* Enable local command recognition (no network needed) */
#define VOICE_LOCAL_CMD_ENABLE  0

#endif /* __VOICE_ASSISTANT_CONFIG_H__ */
