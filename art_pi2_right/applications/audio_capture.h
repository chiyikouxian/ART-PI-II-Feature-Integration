/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2024-10-16     AI Assistant first version - Audio Capture Module
 */

#ifndef __AUDIO_CAPTURE_H__
#define __AUDIO_CAPTURE_H__

#include <rtthread.h>

/* 音频采样配置 */
#define AUDIO_SAMPLE_RATE       16000   /* 采样率 16kHz */
#define AUDIO_CHANNELS          1       /* 单声道 */
#define AUDIO_BITS_PER_SAMPLE   16      /* 16位采样 */
#define AUDIO_BUFFER_SIZE       (1024 * 8)  /* 8KB缓冲区（降低内存占用）*/

/* 音频采集状态 */
typedef enum {
    AUDIO_CAPTURE_IDLE = 0,
    AUDIO_CAPTURE_RECORDING,
    AUDIO_CAPTURE_STOPPED
} audio_capture_state_t;

/* 音频采集回调函数类型 */
typedef void (*audio_capture_callback)(uint8_t *data, uint32_t size);

/* 音频采集接口 */
int audio_capture_init(void);
int audio_capture_start(void);
int audio_capture_stop(void);
int audio_capture_set_callback(audio_capture_callback callback);
audio_capture_state_t audio_capture_get_state(void);
int audio_capture_read(uint8_t *buffer, uint32_t size, uint32_t timeout);

#endif /* __AUDIO_CAPTURE_H__ */

