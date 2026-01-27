/**
 * @file tca9548a.c
 * @brief TCA9548A IIC多路扩展模块驱动 (适配RT-Thread/ART-PI, 模拟IIC)
 * @note  使用与OLED相同的IIC引脚: PE1-SCL, PE2-SDA
 */

#include <rtthread.h>
#include <rtdevice.h>
#include "drv_common.h"
#include "tca9548a.h"
#include "OLED/OLED.h"

/* 模拟IIC延时(微秒) */
#define IIC_DELAY_US    2

/* IIC引脚操作宏 (复用OLED的引脚定义) */
#define TCA_SCL_HIGH()     rt_pin_write(OLED_SCL_PIN, PIN_HIGH)
#define TCA_SCL_LOW()      rt_pin_write(OLED_SCL_PIN, PIN_LOW)
#define TCA_SDA_HIGH()     rt_pin_write(OLED_SDA_PIN, PIN_HIGH)
#define TCA_SDA_LOW()      rt_pin_write(OLED_SDA_PIN, PIN_LOW)
#define TCA_SDA_READ()     rt_pin_read(OLED_SDA_PIN)

/* 当前选中的通道 */
static rt_uint8_t current_channel = 0xFF;

/**
 * @brief  微秒级延时
 * @param  us 延时微秒数
 */
static void tca_delay_us(rt_uint32_t us)
{
    rt_uint32_t delta;
    us = us * (SysTick->LOAD / (1000000 / RT_TICK_PER_SECOND));
    delta = SysTick->VAL;
    while (delta - SysTick->VAL < us);
}

/**
 * @brief  IIC起始信号
 */
static void TCA_I2C_Start(void)
{
    TCA_SDA_HIGH();
    TCA_SCL_HIGH();
    tca_delay_us(IIC_DELAY_US);
    TCA_SDA_LOW();
    tca_delay_us(IIC_DELAY_US);
    TCA_SCL_LOW();
}

/**
 * @brief  IIC停止信号
 */
static void TCA_I2C_Stop(void)
{
    TCA_SDA_LOW();
    TCA_SCL_HIGH();
    tca_delay_us(IIC_DELAY_US);
    TCA_SDA_HIGH();
    tca_delay_us(IIC_DELAY_US);
}

/**
 * @brief  IIC发送一个字节
 * @param  byte 要发送的字节
 * @return RT_EOK: 收到ACK, -RT_ERROR: 未收到ACK
 */
static rt_err_t TCA_I2C_SendByte(rt_uint8_t byte)
{
    rt_uint8_t i;
    rt_err_t ack;

    for (i = 0; i < 8; i++)
    {
        if (byte & (0x80 >> i))
        {
            TCA_SDA_HIGH();
        }
        else
        {
            TCA_SDA_LOW();
        }
        tca_delay_us(IIC_DELAY_US);
        TCA_SCL_HIGH();
        tca_delay_us(IIC_DELAY_US);
        TCA_SCL_LOW();
    }

    /* 释放SDA，读取ACK */
    TCA_SDA_HIGH();
    tca_delay_us(IIC_DELAY_US);
    TCA_SCL_HIGH();
    tca_delay_us(IIC_DELAY_US);

    /* 检查ACK (低电平表示ACK) */
    if (TCA_SDA_READ() == PIN_LOW)
    {
        ack = RT_EOK;
    }
    else
    {
        ack = -RT_ERROR;
    }

    TCA_SCL_LOW();
    return ack;
}

/**
 * @brief  TCA9548A初始化
 * @return RT_EOK 成功
 */
rt_err_t tca9548a_init(void)
{
    /* IIC引脚已在OLED_I2C_Init中初始化，这里只需配置SDA为双向 */
    rt_pin_mode(OLED_SDA_PIN, PIN_MODE_OUTPUT_OD);

    /* 禁用所有通道 */
    current_channel = 0xFF;

    rt_kprintf("[TCA9548A] Initialized\n");
    return RT_EOK;
}

/**
 * @brief  选择TCA9548A通道
 * @param  channel 通道号 (0-7)
 * @return RT_EOK 成功, -RT_ERROR 失败
 */
rt_err_t tca9548a_select_channel(rt_uint8_t channel)
{
    rt_uint8_t data;
    rt_err_t result;

    if (channel >= TCA9548A_MAX_CHANNEL)
    {
        rt_kprintf("[TCA9548A] Invalid channel %d (must be 0-7)\n", channel);
        return -RT_ERROR;
    }

    /* 如果已经选中该通道，直接返回 */
    if (current_channel == channel)
    {
        return RT_EOK;
    }

    data = (1 << channel);  /* 通道掩码 */

    TCA_I2C_Start();

    /* 发送TCA9548A地址 (写模式) */
    result = TCA_I2C_SendByte((TCA9548A_ADDR << 1) | 0x00);
    if (result != RT_EOK)
    {
        TCA_I2C_Stop();
        rt_kprintf("[TCA9548A] No ACK from device\n");
        return -RT_ERROR;
    }

    /* 发送通道选择数据 */
    result = TCA_I2C_SendByte(data);
    if (result != RT_EOK)
    {
        TCA_I2C_Stop();
        rt_kprintf("[TCA9548A] Failed to select channel %d\n", channel);
        return -RT_ERROR;
    }

    TCA_I2C_Stop();

    current_channel = channel;
    rt_thread_mdelay(5);  /* 等待通道切换稳定 */

    return RT_EOK;
}

/**
 * @brief  禁用所有通道
 * @return RT_EOK 成功, -RT_ERROR 失败
 */
rt_err_t tca9548a_disable_all_channels(void)
{
    rt_err_t result;

    TCA_I2C_Start();

    /* 发送TCA9548A地址 (写模式) */
    result = TCA_I2C_SendByte((TCA9548A_ADDR << 1) | 0x00);
    if (result != RT_EOK)
    {
        TCA_I2C_Stop();
        return -RT_ERROR;
    }

    /* 发送0x00禁用所有通道 */
    result = TCA_I2C_SendByte(0x00);
    TCA_I2C_Stop();

    if (result == RT_EOK)
    {
        current_channel = 0xFF;
    }

    return result;
}

/**
 * @brief  获取当前选中的通道
 * @return 当前通道号 (0-7), 0xFF表示无通道选中
 */
rt_uint8_t tca9548a_get_current_channel(void)
{
    return current_channel;
}
