/**
 * @file    main.c
 * @brief   ART-Pi2 多传感器数据采集与WiFi传输系统 - 主程序入口
 *
 * @details 本项目基于 ART-Pi2 开发板 (STM32H7RS MCU) 和 RT-Thread RTOS，
 *          实现了一个多路IMU传感器数据采集与WiFi远程传输系统。
 *
 *          系统架构 (多线程):
 *          ┌─────────────────────────────────────────────────────┐
 *          │  main线程: LED心跳闪烁 + 创建以下子线程               │
 *          ├─────────────────────────────────────────────────────┤
 *          │  iic_drv线程:   初始化I2C总线、TCA9548A、OLED显示     │
 *          │  uart_oled线程: 接收UART1数据并显示到OLED屏幕         │
 *          │  mpu6050线程:   10Hz周期采集11路IMU传感器数据          │
 *          │  bat_adc线程:   ADC采集锂电池电压(PC2分压)           │
 *          │  tcp_cli线程:   通过WiFi将传感器数据JSON发送到PC端     │
 *          │                 (由MSH命令 tcp_start/tcp_stop 控制)   │
 *          └─────────────────────────────────────────────────────┘
 *
 *          硬件连接:
 *          - LED:   PO5 (心跳指示)
 *          - I2C1:  PB8-SCL, PB9-SDA → TCA9548A #1 → 8路MPU6050
 *          - I2C2:  PE1-SCL, PE2-SDA → TCA9548A #2 → 2路MPU6050 + 1路ICM-20948 + OLED
 *          - UART1: PF13-TX, PF12-RX → 外部串口输入
 *          - ADC1:  PC2 (CH12) → 电阻分压采集锂电池电压
 *          - WiFi:  板载CYW43438模块 → TCP发送传感器数据
 *
 * Copyright (c) 2006-2020, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2020-09-02     RT-Thread    first version
 */

#include <rtthread.h>
#include <rtdevice.h>
#include "drv_common.h"
#include "../IIC/iic_thread.h"
#include "../UART/uart_oled_thread.h"
#include "../mpu6050/mpu6050_thread.h"
#include "adc_battery.h"
#include "voice_assistant.h"

/* LED心跳指示灯引脚: PO5 */
#define LED_PIN GET_PIN(O, 5)

/* WiFi接入点配置 (通过MSH命令 wifi join 使用) */
#define WIFI_SSID "baohan"
#define WIFI_PASSWORD "88888887"

/* IIC/OLED线程: 负责I2C总线初始化、TCA9548A探测、OLED显示初始化 */
static struct rt_thread iic_thread;
static rt_uint8_t iic_thread_stack[IIC_THREAD_STACK_SIZE];

/* 串口接收/OLED显示线程: 从UART1接收文本并滚动显示到OLED */
static struct rt_thread uart_oled_thread;
static rt_uint8_t uart_oled_thread_stack[UART_OLED_THREAD_STACK_SIZE];

/* MPU6050数据采集线程: 以10Hz频率轮询11路IMU传感器 */
static struct rt_thread mpu6050_thread;
static rt_uint8_t mpu6050_thread_stack[MPU6050_THREAD_STACK_SIZE];

/* 电池电压ADC采集线程: 通过PC2分压采集锂电池电压 */
static struct rt_thread battery_thread;
static rt_uint8_t battery_thread_stack[BATTERY_THREAD_STACK_SIZE];

/**
 * @brief   应用程序主入口
 * @details 初始化LED引脚，依次创建IIC、UART/OLED、MPU6050、电池ADC四个工作线程，
 *          随后进入LED心跳闪烁循环 (500ms亮/500ms灭)。
 *          TCP客户端线程不在此创建，而是通过MSH命令动态启停。
 * @return  RT_EOK (正常情况下不会返回)
 */
int main(void)
{
    rt_uint32_t count = 1;
    rt_err_t result;

    /* 配置LED引脚为推挽输出 */
    rt_pin_mode(LED_PIN, PIN_MODE_OUTPUT);

    /* 等待系统外设和驱动完全就绪 */
    rt_thread_mdelay(100);

    /* --- 线程1: IIC/OLED初始化 (一次性任务，完成后自动退出) --- */
    result = rt_thread_init(&iic_thread,
                            "iic_drv",
                            iic_thread_entry,
                            RT_NULL,
                            &iic_thread_stack[0],
                            sizeof(iic_thread_stack),
                            IIC_THREAD_PRIORITY,
                            IIC_THREAD_TIMESLICE);

    if (result == RT_EOK)
    {
        rt_thread_startup(&iic_thread);
        rt_kprintf("[Main] IIC/OLED thread created successfully\n");
    }
    else
    {
        rt_kprintf("[Main] Failed to create IIC/OLED thread\n");
    }

    /* --- 线程2: UART接收 + OLED显示 (常驻运行) --- */
    result = rt_thread_init(&uart_oled_thread,
                            "uart_oled",
                            uart_oled_thread_entry,
                            RT_NULL,
                            &uart_oled_thread_stack[0],
                            sizeof(uart_oled_thread_stack),
                            UART_OLED_THREAD_PRIORITY,
                            UART_OLED_THREAD_TIMESLICE);

    if (result == RT_EOK)
    {
        rt_thread_startup(&uart_oled_thread);
        rt_kprintf("[Main] UART_OLED thread created successfully\n");
    }
    else
    {
        rt_kprintf("[Main] Failed to create UART_OLED thread\n");
    }

    /* --- 线程3: MPU6050/ICM-20948多路传感器数据采集 (常驻运行) --- */
    result = rt_thread_init(&mpu6050_thread,
                            "mpu6050",
                            mpu6050_thread_entry,
                            RT_NULL,
                            &mpu6050_thread_stack[0],
                            sizeof(mpu6050_thread_stack),
                            MPU6050_THREAD_PRIORITY,
                            MPU6050_THREAD_TIMESLICE);

    if (result == RT_EOK)
    {
        rt_thread_startup(&mpu6050_thread);
        rt_kprintf("[Main] MPU6050 thread created successfully\n");
    }
    else
    {
        rt_kprintf("[Main] Failed to create MPU6050 thread\n");
    }

    /* --- 线程4: 电池电压ADC采集 (常驻运行，PC2分压采集) --- */
    result = rt_thread_init(&battery_thread,
                            "bat_adc",
                            battery_thread_entry,
                            RT_NULL,
                            &battery_thread_stack[0],
                            sizeof(battery_thread_stack),
                            BATTERY_THREAD_PRIORITY,
                            BATTERY_THREAD_TIMESLICE);

    if (result == RT_EOK)
    {
        rt_thread_startup(&battery_thread);
        rt_kprintf("[Main] Battery ADC thread created successfully\n");
    }
    else
    {
        rt_kprintf("[Main] Failed to create Battery ADC thread\n");
    }

    /* TCP客户端线程通过MSH命令 tcp_start/tcp_stop 动态控制启停，
     * 不在此处创建。使用方法:
     *   msh> wifi join <SSID> <password>    -- 先连接WiFi
     *   msh> tcp_start [ip] [port]          -- 启动TCP发送
     *   msh> tcp_stop                       -- 停止TCP发送
     */

    /* 主线程进入LED心跳闪烁循环，表示系统正常运行 */
    while(count++)
    {
        rt_thread_mdelay(500);
        rt_pin_write(LED_PIN, PIN_HIGH);
        rt_thread_mdelay(500);
        rt_pin_write(LED_PIN, PIN_LOW);
    }
    return RT_EOK;
}

#include "stm32h7rsxx.h"
/**
 * @brief   重定位中断向量表到XSPI2外部Flash地址
 * @details ART-Pi2从外部XSPI2 Flash启动时，需要将向量表基地址
 *          指向XSPI2_BASE，否则中断无法正确响应。
 *          通过INIT_BOARD_EXPORT在板级初始化阶段自动执行。
 * @return  0 成功
 */
static int vtor_config(void)
{
    /* Vector Table Relocation in Internal XSPI2_BASE */
    SCB->VTOR = XSPI2_BASE;
    return 0;
}
INIT_BOARD_EXPORT(vtor_config);
