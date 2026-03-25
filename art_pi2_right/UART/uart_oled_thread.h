/**
 * @file uart_oled_thread.h
 * @brief 串口接收线程头文件 - 接收串口文本并显示到OLED
 */

#ifndef __UART_OLED_THREAD_H
#define __UART_OLED_THREAD_H

#include <rtthread.h>

/* 线程配置 */
#define UART_OLED_THREAD_PRIORITY     18
#define UART_OLED_THREAD_STACK_SIZE   2048
#define UART_OLED_THREAD_TIMESLICE    10

/* 串口设备名称 */
#define UART_OLED_DEVICE_NAME         "uart1"

/* 函数声明 */
void uart_oled_thread_entry(void *parameter);

/**
 * @brief  将文本显示到OLED屏幕 (支持中文, 自动分行和滚动)
 * @param  text  UTF-8 编码的文本字符串
 * @note   可从任意线程调用, 内部会获取I2C总线互斥锁
 *         每行最多显示 8 个中文字符或 16 个 ASCII 字符
 *         超过 3 行时自动向上滚动
 */
void oled_show_stt_result(const char *text);

#endif /* __UART_OLED_THREAD_H */
