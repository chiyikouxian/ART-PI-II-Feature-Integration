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

#endif /* __UART_OLED_THREAD_H */
