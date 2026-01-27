/**
 * @file iic_thread.h
 * @brief IIC多路扩展模块驱动线程头文件
 */

#ifndef __IIC_THREAD_H
#define __IIC_THREAD_H

#include <rtthread.h>

/* 线程配置 */
#define IIC_THREAD_PRIORITY     20
#define IIC_THREAD_STACK_SIZE   2048
#define IIC_THREAD_TIMESLICE    10

/* 函数声明 */
void iic_thread_entry(void *parameter);

#endif /* __IIC_THREAD_H */
