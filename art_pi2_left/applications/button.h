/**
 * @file button.h
 * @brief PE0按键驱动 - 按下切换ROCK推理模式（单词/句子）
 * @note  按键接在GND与PE0之间，内部上拉，低电平有效
 */

#ifndef __BUTTON_H__
#define __BUTTON_H__

#include <rtthread.h>

#define BUTTON_THREAD_STACK_SIZE    512
#define BUTTON_THREAD_PRIORITY      25
#define BUTTON_THREAD_TIMESLICE     10

void button_thread_entry(void *parameter);

#endif /* __BUTTON_H__ */
