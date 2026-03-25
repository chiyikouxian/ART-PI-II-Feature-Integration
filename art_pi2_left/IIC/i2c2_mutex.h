/**
 * @file i2c2_mutex.h
 * @brief I2C2总线共享互斥锁
 * @note  I2C2 (PE1/PE2) 被OLED显示和MPU6050传感器共享
 *        所有访问I2C2总线的操作必须先获取此互斥锁
 */

#ifndef __I2C2_MUTEX_H
#define __I2C2_MUTEX_H

#include <rtthread.h>

/* 初始化I2C2总线互斥锁 */
void i2c2_mutex_init(void);

/* 获取I2C2总线互斥锁 (阻塞等待) */
rt_err_t i2c2_mutex_take(rt_int32_t timeout_ms);

/* 释放I2C2总线互斥锁 */
rt_err_t i2c2_mutex_release(void);

#endif /* __I2C2_MUTEX_H */
