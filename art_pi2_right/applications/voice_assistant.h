/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2024-10-16     AI Assistant first version - Voice Assistant Main Module
 */

#ifndef __VOICE_ASSISTANT_H__
#define __VOICE_ASSISTANT_H__

#include <rtthread.h>

/* 语音助手状态 */
typedef enum {
    VOICE_ASSISTANT_IDLE = 0,      /* 空闲状态 */
    VOICE_ASSISTANT_LISTENING,     /* 正在监听 */
    VOICE_ASSISTANT_PROCESSING,    /* 正在处理 */
    VOICE_ASSISTANT_SPEAKING,      /* 正在播放回复 */
    VOICE_ASSISTANT_ERROR          /* 错误状态 */
} voice_assistant_state_t;

/* 语音助手接口 */
int voice_assistant_init(void);
int voice_assistant_start(void);
int voice_assistant_stop(void);
voice_assistant_state_t voice_assistant_get_state(void);

/* 手动触发语音识别（用于按键触发）*/
int voice_assistant_trigger(void);

#endif /* __VOICE_ASSISTANT_H__ */

