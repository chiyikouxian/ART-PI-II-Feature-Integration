/**
 * @file tca9548a.h
 * @brief TCA9548A IIC多路扩展模块驱动头文件 (适配RT-Thread/ART-PI, 模拟IIC)
 */

#ifndef __TCA9548A_H
#define __TCA9548A_H

#include <rtthread.h>
#include <rtdevice.h>

/* TCA9548A I2C地址 (A0=A1=A2=GND时为0x70) */
#define TCA9548A_ADDR       0x70

/* TCA9548A通道数 */
#define TCA9548A_MAX_CHANNEL    8

/* 函数声明 */
rt_err_t tca9548a_init(void);
rt_err_t tca9548a_select_channel(rt_uint8_t channel);
rt_err_t tca9548a_disable_all_channels(void);
rt_uint8_t tca9548a_get_current_channel(void);

#endif /* __TCA9548A_H */
