/*
 * stt_config.h - 百度语音识别API配置
 */
#ifndef __STT_CONFIG_H__
#define __STT_CONFIG_H__

/* ==================== 百度API配置 ==================== */
/* 请在百度AI开放平台申请: https://ai.baidu.com/tech/speech */
#define BAIDU_API_KEY       "hNsazCqEr3uBDhHEAdByUzKq"
#define BAIDU_SECRET_KEY    "7zWq8boftq2gFNOV0zhC9m0UB1E0culg"

/* 百度API服务器 */
#define BAIDU_TOKEN_HOST    "aip.baidubce.com"
#define BAIDU_TOKEN_PATH    "/oauth/2.0/token"
#define BAIDU_ASR_HOST      "vop.baidu.com"
#define BAIDU_ASR_PATH      "/server_api/"
#define BAIDU_TOKEN_PORT    80
#define BAIDU_ASR_PORT      80

/* ==================== 音频参数 ==================== */
#define STT_SAMPLE_RATE     16000       /* 百度要求16000Hz */
#define STT_BIT_DEPTH       16          /* 16-bit PCM */
#define STT_CHANNEL         1           /* 单声道 */

/* 录音限制 */
#define STT_MAX_RECORD_SEC  3           /* 最大录音时长(秒) - 减少内存占用 */
#define STT_MIN_RECORD_MS   500         /* 最小有效录音时长(毫秒) */

/* ==================== 网络参数 ==================== */
#define HTTP_RECV_BUF_SIZE  4096        /* HTTP接收缓冲区 */
#define HTTP_SEND_TIMEOUT   10          /* 发送超时(秒) */
#define HTTP_RECV_TIMEOUT   15          /* 接收超时(秒) */
#define HTTP_TOKEN_LEN      128         /* Token最大长度 */

/* ==================== STT结果 ==================== */
#define STT_RESULT_MAX_LEN  256         /* 识别结果最大长度 */

#endif /* __STT_CONFIG_H__ */
