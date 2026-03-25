/**
 * @file i2c_gpio.c
 * @brief I2C2 GPIO引脚初始化实现 (PE1-SCL, PE2-SDA)
 */

#include "i2c_gpio.h"

/**
 * @brief  初始化I2C2 GPIO引脚为开漏输出模式
 * @note   将SCL和SDA设为开漏模式并释放(拉高)
 */
void i2c_gpio_init(void)
{
    rt_pin_mode(I2C2_SCL_PIN, PIN_MODE_OUTPUT_OD);
    rt_pin_mode(I2C2_SDA_PIN, PIN_MODE_OUTPUT_OD);

    rt_pin_write(I2C2_SCL_PIN, PIN_HIGH);
    rt_pin_write(I2C2_SDA_PIN, PIN_HIGH);
}
