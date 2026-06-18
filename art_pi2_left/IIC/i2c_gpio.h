/**
 * @file i2c_gpio.h
 * @brief I2C2 GPIO引脚定义及初始化 (PE1-SCL, PE2-SDA)
 * @note  供TCA9548A bit-bang驱动和其他需要GPIO模拟I2C的模块使用
 */

#ifndef __I2C_GPIO_H
#define __I2C_GPIO_H

#include <rtthread.h>
#include <rtdevice.h>
#include "drv_gpio.h"

/* I2C2 GPIO引脚定义: PE1-SCL, PE2-SDA */
#define I2C2_SCL_PIN    GET_PIN(E, 1)
#define I2C2_SDA_PIN    GET_PIN(E, 2)

/**
 * @brief  初始化I2C2 GPIO引脚为开漏输出模式
 */
void i2c_gpio_init(void);

#endif /* __I2C_GPIO_H */
