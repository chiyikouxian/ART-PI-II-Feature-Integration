/**
 * @file iic_thread.c
 * @brief IIC多路扩展模块驱动线程
 * @note  该线程用于驱动TCA9548A多路IIC扩展模块和OLED显示
 *        IIC引脚: PE1-SCL, PE2-SDA
 */

#include <rtthread.h>
#include <rtdevice.h>
#include "drv_common.h"
#include "iic_thread.h"
#include "tca9548a.h"
#include "OLED/OLED.h"
#include "i2c2_mutex.h"
#include "../UART/uart_oled_thread.h"

/* OLED连接在TCA9548A的通道号 */
#define OLED_TCA9548A_CHANNEL   3

/**
 * @brief  IIC/OLED线程入口函数
 * @param  parameter 线程参数(未使用)
 * @note   该函数供main.c创建线程时使用
 */
void iic_thread_entry(void *parameter)
{
    rt_kprintf("[IIC Thread] Started\n");

    /* 首先初始化IIC引脚 */
    OLED_I2C_Init();
    rt_kprintf("[IIC Thread] I2C GPIO initialized\n");

    /* 初始化TCA9548A (自动探测是否存在) */
    tca9548a_init();

    /* 获取I2C2总线互斥锁 */
    i2c2_mutex_take(1000);

    if (tca9548a_is_present())
    {
        /* TCA9548A存在: 切换到OLED所在的通道3 */
        if (tca9548a_select_channel(OLED_TCA9548A_CHANNEL) != RT_EOK)
        {
            i2c2_mutex_release();
            rt_kprintf("[IIC Thread] Failed to select channel %d\n", OLED_TCA9548A_CHANNEL);
            // oled_show_error_status("I2C FAIL");  // 由init_status模块统一管理
            return;
        }
        rt_kprintf("[IIC Thread] TCA9548A channel %d selected\n", OLED_TCA9548A_CHANNEL);
    }
    else
    {
        /* TCA9548A不存在: OLED直接接在I2C2上，无需通道选择 */
        rt_kprintf("[IIC Thread] No TCA9548A, OLED direct on I2C2\n");
    }

    /* 初始化OLED */
    OLED_Init();
    rt_kprintf("[IIC Thread] OLED initialized\n");

    if (tca9548a_is_present())
    {
        tca9548a_select_channel(OLED_TCA9548A_CHANNEL);
    }

    /* 显示初始状态 */
    OLED_Update();

    /* 释放I2C2总线互斥锁 */
    i2c2_mutex_release();

    /* 不再显示"OLED READY"，改为由init_status模块统一管理显示 */
    // oled_show_system_status("OLED READY");

    rt_kprintf("[IIC Thread] OLED display updated\n");
    rt_kprintf("[IIC Thread] Init complete, thread exit\n");
}
