/**
 * @file iic_thread.c
 * @brief IIC多路扩展模块驱动线程
 * @note  该线程用于初始化I2C总线互斥锁和TCA9548A多路IIC扩展模块
 *        IIC引脚: PE1-SCL, PE2-SDA
 */

#include <rtthread.h>
#include <rtdevice.h>
#include "drv_common.h"
#include "iic_thread.h"
#include "tca9548a.h"
#include "i2c_gpio.h"
#include "i2c2_mutex.h"

/**
 * @brief  IIC初始化线程入口函数
 * @param  parameter 线程参数(未使用)
 * @note   该函数供main.c创建线程时使用
 */
void iic_thread_entry(void *parameter)
{
    rt_kprintf("[IIC Thread] Started\n");

    /* 初始化I2C2总线互斥锁 */
    i2c2_mutex_init();

    /* 初始化I2C2 GPIO引脚 (PE1-SCL, PE2-SDA) */
    i2c_gpio_init();
    rt_kprintf("[IIC Thread] I2C GPIO initialized\n");

    /* 初始化TCA9548A (自动探测是否存在) */
    tca9548a_init();

    if (tca9548a_is_present())
    {
        rt_kprintf("[IIC Thread] TCA9548A detected\n");
    }
    else
    {
        rt_kprintf("[IIC Thread] No TCA9548A detected\n");
    }

    rt_kprintf("[IIC Thread] Init complete, thread exit\n");
}
