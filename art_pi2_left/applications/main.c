/**
 * @file    main.c
 * @brief   ART-Pi2 多传感器数据采集与WiFi传输系统 - 主程序入口
 *
 * @details 本项目基于 ART-Pi2 开发板 (STM32H7RS MCU) 和 RT-Thread RTOS，
 *          实现了一个多路IMU传感器数据采集与WiFi远程传输系统。
 *
 *          系统架构 (多线程):
 *          ┌──────────────────────────────────────────────────────┐
 *          │  main线程: LED心跳闪烁 + 创建以下子线程              │
 *          ├──────────────────────────────────────────────────────┤
 *          │  iic_drv线程:   初始化I2C总线、TCA9548A              │
 *          │  vtx316线程:    初始化VTX316语音合成模块(UART1)      │
 *          │  mpu6050线程:   10Hz周期采集11路IMU传感器数据        │
 *          │  bat_adc线程:   ADC采集锂电池电压(PC2分压)           │
 *          │  tcp_cli线程:   通过WiFi将传感器+电池数据JSON发送到PC│
 *          │                 (由MSH命令 tcp_start/tcp_stop 控制)  │
 *          │  BLE线程(自动):  NimBLE外设广播"ART-Pi2-IMU-L"       │
 *          │                 Notify发送145字节IMU数据(10Hz)       │
 *          │                 接收文本"SAY:xxx"触发VTX316语音播报  │
 *          └──────────────────────────────────────────────────────┘
 *
 *          硬件连接:
 *          - LED:   PO5 (心跳指示)
 *          - I2C1:  PB8-SCL, PB9-SDA → TCA9548A #1 → 8路MPU6050
 *          - I2C2:  PE1-SCL, PE2-SDA → TCA9548A #2 → 2路MPU6050 + 1路ICM-20948
 *          - UART1: PF13-TX, PF12-RX → VTX316语音合成模块
 *          - ADC1:  PC2 (CH12) → 电阻分压采集锂电池电压
 *          - WiFi:  板载CYW43438模块 → TCP发送传感器+电池数据
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
#include <wlan_mgnt.h>
#include "stm32h7rsxx.h"
#include "drv_common.h"
#include "../IIC/iic_thread.h"
#include "../vtx316/vtx316.h"
#include "../mpu6050/mpu6050_thread.h"
#include "adc_battery.h"
#include "tcp_client.h"
#include "pc_discovery.h"
#include "server_config.h"
#include "imu_wifi_sender.h"
#include "operation_mode.h"
#include "button.h"

rt_bool_t left_ble_init_ok(void);
int left_ble_app_init(void);

/* LED心跳指示灯引脚: PO5 */
#define LED_PIN GET_PIN(O, 5)

#define WLAN_DEVICE_TIMEOUT_MS    10000
#define WLAN_CONNECT_TIMEOUT_MS   15000
#define TCP_START_DELAY_MS        2000

/* WiFi接入点配置 (通过MSH命令 wifi join 使用) */
#define WIFI_SSID "baohan"
#define WIFI_PASSWORD "88888887"

/* IIC初始化线程: 负责I2C总线初始化、TCA9548A探测 */
static struct rt_thread iic_thread;
static rt_uint8_t iic_thread_stack[IIC_THREAD_STACK_SIZE];

/* VTX316语音合成线程: 初始化UART1并发送开机语音 */
static struct rt_thread vtx316_thread;
static rt_uint8_t vtx316_thread_stack[VTX316_THREAD_STACK_SIZE];

/* MPU6050数据采集线程: 以10Hz频率轮询11路IMU传感器 */
static struct rt_thread mpu6050_thread;
static rt_uint8_t mpu6050_thread_stack[MPU6050_THREAD_STACK_SIZE];

/* 电池ADC采集线程: 通过PC2分压采集锂电池电压 */
static struct rt_thread battery_thread;
static rt_uint8_t battery_thread_stack[BATTERY_THREAD_STACK_SIZE];

/* 按键线程: PE0按下播报当前电量 */
static struct rt_thread button_thread;
static rt_uint8_t button_thread_stack[BUTTON_THREAD_STACK_SIZE];

/**
 * @brief   应用程序主入口
 * @details 初始化LED引脚，依次创建IIC、VTX316、MPU6050、电池ADC四个工作线程，
 *          随后进入LED心跳闪烁循环 (500ms亮/500ms灭)。
 *          TCP客户端线程不在此创建，而是通过MSH命令动态启停。
 * @return  RT_EOK (正常情况下不会返回)
 */
int main(void)
{
    /*
     * AI辅助参考：DeepSeek-R1，2025-10-20。
     * 用途：对 RT-Thread / Linux / FreeRTOS 架构选型做方案对比；
     * 本启动流程、线程划分和硬件初始化顺序由团队结合 ART-Pi2 实际工程人工实现。
     */
    rt_uint32_t count = 1;
    rt_err_t result;
    rt_int32_t wait_ms;
    rt_bool_t wifi_connected = RT_FALSE;
    rt_bool_t init_success = RT_TRUE;
    rt_device_t wlan_dev;

    /* 配置LED引脚为推挽输出 */
    rt_pin_mode(LED_PIN, PIN_MODE_OUTPUT);

    /* 等待系统外设和驱动完全就绪 */
    rt_thread_mdelay(100);

    /* 初始化 server_config 的互斥锁（必须在任何发现/TCP线程前调用） */
    server_config_init();

    /* 初始化发现模块（必须在任何发现线程前调用） */
    pc_discovery_init();

    /* --- 线程1: IIC初始化 (一次性任务，完成后自动退出) --- */
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
        rt_kprintf("[Main] IIC thread created successfully\n");
    }
    else
    {
        rt_kprintf("[Main] Failed to create IIC thread\n");
        init_success = RT_FALSE;
        vtx316_report_boot_error(VTX316_BOOT_ERROR_SENSOR);
    }

    /* --- 在VTX线程前等待WLAN设备出现并连接WiFi --- */
    wlan_dev = RT_NULL;
    wait_ms = 0;
    while (wlan_dev == RT_NULL && wait_ms < WLAN_DEVICE_TIMEOUT_MS)
    {
        wlan_dev = rt_device_find(RT_WLAN_DEVICE_STA_NAME);
        if (wlan_dev == RT_NULL)
        {
            rt_thread_mdelay(200);
            wait_ms += 200;
        }
    }

    if (wlan_dev != RT_NULL)
    {
        rt_kprintf("[Main] WLAN device ready, start connect: %s\n", WIFI_SSID);
        result = rt_wlan_connect(WIFI_SSID, WIFI_PASSWORD);
        if (result == RT_EOK)
        {
            wait_ms = 0;
            while (!rt_wlan_is_connected() && wait_ms < WLAN_CONNECT_TIMEOUT_MS)
            {
                rt_thread_mdelay(200);
                wait_ms += 200;
            }

            if (rt_wlan_is_connected())
            {
                wifi_connected = RT_TRUE;
                rt_kprintf("[Main] WiFi connected successfully\n");
                rt_thread_mdelay(TCP_START_DELAY_MS);

                /* --- WiFi连接成功后初始化BLE --- */
#if 0  /* 暂时禁用BLE功能 - 保留代码以便后续启用 */
                rt_kprintf("[Main] Starting BLE initialization after WiFi connected...\n");
                result = left_ble_app_init();
                if (result != RT_EOK)
                {
                    rt_kprintf("[Main] BLE initialization failed: %d\n", result);
                    init_success = RT_FALSE;
                    vtx316_report_boot_error(VTX316_BOOT_ERROR_BLE);
                }
                else
                {
                    rt_kprintf("[Main] BLE initialized successfully\n");
                }
#endif  /* 禁用BLE功能结束 */
            }
            else
            {
                rt_kprintf("[Main] WiFi connect timeout\n");
                init_success = RT_FALSE;
                vtx316_report_boot_error(VTX316_BOOT_ERROR_WIFI_TIMEOUT);
            }
        }
        else
        {
            rt_kprintf("[Main] WiFi connect failed: %d\n", result);
            init_success = RT_FALSE;
            vtx316_report_boot_error(VTX316_BOOT_ERROR_WIFI_CONNECT);
        }
    }
    else
    {
        rt_kprintf("[Main] WLAN device not ready within %d ms\n", WLAN_DEVICE_TIMEOUT_MS);
        init_success = RT_FALSE;
        vtx316_report_boot_error(VTX316_BOOT_ERROR_WIFI_DEVICE);
    }

    /* --- 线程2: VTX316语音合成初始化 (一次性任务，完成后自动退出) --- */
    result = rt_thread_init(&vtx316_thread,
                            "vtx316",
                            vtx316_thread_entry,
                            RT_NULL,
                            &vtx316_thread_stack[0],
                            sizeof(vtx316_thread_stack),
                            VTX316_THREAD_PRIORITY,
                            VTX316_THREAD_TIMESLICE);

    if (result == RT_EOK)
    {
        rt_thread_startup(&vtx316_thread);
        rt_kprintf("[Main] VTX316 thread created successfully\n");
    }
    else
    {
        rt_kprintf("[Main] Failed to create VTX316 thread\n");
        init_success = RT_FALSE;
        vtx316_report_boot_error(VTX316_BOOT_ERROR_VOICE);
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
        init_success = RT_FALSE;
        vtx316_report_boot_error(VTX316_BOOT_ERROR_SENSOR);
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

    /* --- 线程5: PE0按键 - 按下播报当前电量 (常驻运行) --- */
    result = rt_thread_init(&button_thread,
                            "button",
                            button_thread_entry,
                            RT_NULL,
                            &button_thread_stack[0],
                            sizeof(button_thread_stack),
                            BUTTON_THREAD_PRIORITY,
                            BUTTON_THREAD_TIMESLICE);

    if (result == RT_EOK)
    {
        rt_thread_startup(&button_thread);
        rt_kprintf("[Main] Button thread created successfully\n");
    }
    else
    {
        rt_kprintf("[Main] Failed to create Button thread\n");
    }

    /* --- 等待传感器校准完成 --- */
    rt_kprintf("[Main] Waiting for sensor calibration (6 seconds)...\n");
    rt_thread_mdelay(6000);  /* ICM需要250样本×20ms=5秒，加1秒余量 */

    /* --- 联网成功后自动启动PC发现线程和TCP客户端 --- */
    if (wifi_connected)
    {
        /* Start PC UDP discovery first — it runs in the background and updates
         * server_config when the PC is found. Even if the PC is not yet running,
         * discovery continues indefinitely and will pick up the PC as soon as
         * the user starts the Python frontend. */
        result = pc_discovery_start();
        if (result == RT_EOK)
        {
            rt_kprintf("[Main] PC discovery started (UDP :9108)\n");
        }
        else
        {
            rt_kprintf("[Main] PC discovery start failed: %d\n", result);
        }

        /* Wait up to 3 seconds for at least one valid PC broadcast.
         * If timeout expires we still proceed — the default server_config
         * will be used for the first TCP attempt, and discovery continues
         * in the background. */
        rt_kprintf("[Main] Waiting for PC discovery (up to 3s)...\n");
        if (pc_discovery_wait_server(3000) == RT_EOK)
        {
            rt_kprintf("[Main] PC discovered, starting TCP client\n");
        }
        else
        {
            rt_kprintf("[Main] PC discovery timeout, using default address\n");
        }

        /* Now start TCP client — it will read the current server_config
         * endpoint (discovered or default) and keep the generation counter
         * to auto-reconnect whenever pc_discovery updates the address. */
        result = tcp_client_start(0, RT_NULL);
        if (result == 0)
        {
            rt_kprintf("[Main] TCP client started automatically\n");
        }
        else
        {
            rt_kprintf("[Main] TCP client auto start failed: %d\n", result);
            init_success = RT_FALSE;
            vtx316_report_boot_error(VTX316_BOOT_ERROR_TCP);
        }

        /* --- 启动操作模式状态机（含自动唤醒+校准线程） --- */
        operation_mode_init();

        /* --- 启动IMU WiFi发送线程（发送原始数据到Rock） --- */
        result = imu_wifi_sender_start();
        if (result == RT_EOK)
        {
            rt_kprintf("[Main] IMU WiFi sender started automatically\n");
        }
        else
        {
            rt_kprintf("[Main] IMU WiFi sender auto start failed: %d\n", result);
            /* 不影响init_success，允许继续运行 */
        }
    }
    else
    {
        rt_kprintf("[Main] Skip TCP auto start because WiFi is not connected\n");
    }

    if (init_success)
    {
        vtx316_report_boot_success();
    }

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
