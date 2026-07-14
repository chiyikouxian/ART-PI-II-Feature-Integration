/**
 * @file    main.c
 * @brief   ART-Pi2 多传感器数据采集与WiFi传输系统 - 主程序入口
 *
 * @details 本项目基于 ART-Pi2 开发板 (STM32H7RS MCU) 和 RT-Thread RTOS，
 *          实现了一个多路IMU传感器数据采集与WiFi远程传输系统。
 *
 *          系统架构 (多线程):
 *          ┌─────────────────────────────────────────────────────┐
 *          │  main线程: LED心跳闪烁 + 创建以下子线程             │
 *          ├─────────────────────────────────────────────────────┤
 *          │  iic_drv线程:    初始化I2C总线、TCA9548A、OLED显示  │
 *          │  uart_oled线程:  OLED单区显示                       │
 *          │                  显示I2S语音转文字结果              │
 *          │  mpu6050线程:    10Hz周期采集11路IMU传感器数据      │
 *          │  bat_adc线程:    ADC采集锂电池电压(PC2分压)         │
 *          │  autostart线程:  上电自启动(一次性):                │
 *          │                  WiFi连接 → TCP客户端 → 语音助手    │
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
#include "tcp_client.h"
#include "pc_discovery.h"
#include "server_config.h"
#include "voice_assistant.h"
#include "imu_wifi_sender.h"
#include "operation_mode.h"
#include "init_status.h"
#include "net_manager.h"
#include "net_session.h"
#include <wlan_mgnt.h>
#include "../IIC/i2c2_mutex.h"

rt_bool_t right_ble_init_ok(void);
int right_ble_app_init(void);

/* LED心跳指示灯引脚: PO5 */
#define LED_PIN GET_PIN(O, 5)

/* WiFi接入点配置 (通过MSH命令 wifi join 使用) */
#define WIFI_SSID "baohan"
#define WIFI_PASSWORD "88888887"

/* 自启动线程配置 */
#define AUTOSTART_THREAD_STACK_SIZE   4096
#define AUTOSTART_THREAD_PRIORITY     25
#define AUTOSTART_WIFI_TIMEOUT_MS     15000   /* WiFi连接超时15秒 */
#define AUTOSTART_WIFI_RETRY_MAX      3       /* WiFi最大重试次数 */

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
 * @brief   自启动线程: WiFi连接 → TCP客户端 → 语音助手(I2S)
 * @details 上电后自动依次执行:
 *          1. 等待WiFi驱动就绪 (5秒)
 *          2. 连接WiFi (SSID/密码定义在宏中, 最多重试3次)
 *          3. 等待网络协议栈就绪 (获取IP地址)
 *          4. 启动TCP客户端 (连接到PC端服务器)
 *          5. 初始化并启动语音助手 (含I2S驱动初始化)
 *          完成后线程自动退出。各模块仍可通过MSH命令手动控制。
 */
static void autostart_thread_entry(void *parameter)
{
    rt_err_t ret;

    rt_kprintf("[Autostart] Starting auto-initialization sequence...\n");

    /* ===== 第1步: 等待WiFi驱动就绪 ===== */
    rt_kprintf("[Autostart] Waiting for WiFi driver...\n");
    rt_thread_mdelay(5000);

    /* ===== 第2步: 等待 net_manager 将 WiFi 就绪 =====
     * net_manager owns the only rt_wlan_connect() call; we just wait on
     * its ready semaphore. If net_manager is already in READY this returns
     * immediately. This replaces the previous direct rt_wlan_connect()
     * loop, eliminating duplicate connect attempts and state races. */
    rt_kprintf("[Autostart] Waiting for net_manager to bring up WiFi...\n");
    if (net_manager_wait_ready(AUTOSTART_WIFI_TIMEOUT_MS) != RT_EOK)
    {
        rt_kprintf("[Autostart] net_manager did not reach READY in %dms\n",
                   AUTOSTART_WIFI_TIMEOUT_MS);
        init_status_update(INIT_MODULE_WIFI, INIT_STATUS_FAIL);
        /* Continue anyway — TCP will retry via its own net_manager_wait_ready(). */
    }
    else
    {
        rt_kprintf("[Autostart] Network ready (WiFi up via net_manager)\n");
        init_status_update(INIT_MODULE_WIFI, INIT_STATUS_OK);
    }

    /* ===== 第3步: 等待传感器校准完成 ===== */
    rt_kprintf("[Autostart] Waiting for sensor calibration (6 seconds)...\n");
    rt_thread_mdelay(6000);  /* ICM需要250样本×20ms=5秒，加1秒余量 */

    /* ===== 第4步: 启动PC UDP发现线程 ===== */
    rt_kprintf("[Autostart] Starting PC discovery (UDP :9108)...\n");
    {
        rt_err_t r = pc_discovery_start();
        if (r == RT_EOK)
        {
            rt_kprintf("[Autostart] PC discovery started\n");
            /* Wait up to 3s for first broadcast; if timeout, proceed anyway
             * using the default address — discovery keeps running in background. */
            rt_kprintf("[Autostart] Waiting for PC broadcast (up to 3s)...\n");
            if (pc_discovery_wait_server(3000) == RT_EOK)
            {
                rt_kprintf("[Autostart] PC discovered, starting TCP client\n");
            }
            else
            {
                rt_kprintf("[Autostart] PC discovery timeout, using default address\n");
            }
        }
        else
        {
            rt_kprintf("[Autostart] PC discovery start failed: %d\n", r);
        }
    }

    /* ===== 第5步: 启动TCP客户端 ===== */
    rt_kprintf("[Autostart] Starting TCP client...\n");
    {
        char *argv[] = {"tcp_start"};
        if (tcp_client_start(1, argv) != RT_EOK)
        {
            init_status_update(INIT_MODULE_TCP, INIT_STATUS_FAIL);
        }
        else
        {
            init_status_update(INIT_MODULE_TCP, INIT_STATUS_OK);
        }
    }

    /* 等待TCP连接建立 */
    rt_thread_mdelay(2000);

    /* ===== 第6步: 启动IMU WiFi发送线程 ===== */
    operation_mode_init();
    rt_kprintf("[Autostart] Starting IMU WiFi sender...\n");
    ret = imu_wifi_sender_start();
    if (ret == RT_EOK)
    {
        rt_kprintf("[Autostart] IMU WiFi sender started successfully\n");
    }
    else
    {
        rt_kprintf("[Autostart] IMU WiFi sender start failed: %d\n", ret);
        /* 不影响后续流程，允许继续运行 */
    }

    /* ===== 第7步: 初始化并启动语音助手 ===== */
    rt_kprintf("[Autostart] Initializing voice assistant...\n");
    ret = voice_assistant_init();
    if (ret == RT_EOK)
    {
        rt_kprintf("[Autostart] Voice assistant initialized, starting...\n");
        init_status_update(INIT_MODULE_STT, INIT_STATUS_OK);
        voice_assistant_start();
    }
    else
    {
        rt_kprintf("[Autostart] Voice assistant init failed (err=%d)\n", ret);
        init_status_update(INIT_MODULE_STT, INIT_STATUS_FAIL);
    }

    rt_kprintf("[Autostart] Auto-initialization complete\n");
}

/**
 * @brief   应用程序主入口
 * @details 初始化LED引脚，创建各工作线程及自启动线程，
 *          随后进入LED心跳闪烁循环 (500ms亮/500ms灭)。
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
    rt_bool_t init_success = RT_TRUE;
    rt_bool_t iic_ok = RT_FALSE;
    rt_bool_t uart_ok = RT_FALSE;
    rt_bool_t imu_ok = RT_FALSE;
    rt_bool_t autostart_ok = RT_FALSE;

    /* 配置LED引脚为推挽输出 */
    rt_pin_mode(LED_PIN, PIN_MODE_OUTPUT);

    /* 提前初始化I2C2互斥锁，避免并发线程在锁未初始化时访问 */
    i2c2_mutex_init();

    /* Issue 4 fix: initialise net_session's mutex once, up front, so the
     * tcp_client and imu_wifi_sender threads can NEVER race on a lazy
     * mutex-init. Both call net_session_announce() from their own threads. */
    net_session_init();

    /* 初始化 server_config 的互斥锁（必须在任何发现/TCP线程前调用） */
    server_config_init();

    /* 初始化发现模块（必须在任何发现线程前调用） */
    pc_discovery_init();

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
        iic_ok = RT_TRUE;
        rt_kprintf("[Main] IIC/OLED thread created successfully\n");
    }
    else
    {
        rt_kprintf("[Main] Failed to create IIC/OLED thread\n");
        init_success = RT_FALSE;
    }

    /* 等待IIC线程完成OLED初始化（约1-2秒） */
    rt_thread_mdelay(2500);

    /* 初始化状态显示界面（必须在OLED初始化完成后） */
    init_status_display_init();

    /* 更新I2C状态 */
    if (iic_ok)
    {
        init_status_update(INIT_MODULE_I2C, INIT_STATUS_OK);
    }
    else
    {
        init_status_update(INIT_MODULE_I2C, INIT_STATUS_FAIL);
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
        uart_ok = RT_TRUE;
        rt_kprintf("[Main] UART_OLED thread created successfully\n");
        init_status_update(INIT_MODULE_UART, INIT_STATUS_OK);
    }
    else
    {
        rt_kprintf("[Main] Failed to create UART_OLED thread\n");
        init_success = RT_FALSE;
        init_status_update(INIT_MODULE_UART, INIT_STATUS_FAIL);
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
        imu_ok = RT_TRUE;
        rt_kprintf("[Main] MPU6050 thread created successfully\n");
        /* IMU计数由mpu6050_thread扫描完成后通过init_status_update_imu_count上报 */
    }
    else
    {
        rt_kprintf("[Main] Failed to create MPU6050 thread\n");
        init_success = RT_FALSE;
        init_status_update_imu_count(0, MPU_TOTAL_CHANNELS);
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

    /* --- 线程5: 自启动 (WiFi → BLE → TCP → 语音助手, 一次性任务) --- */

    /* --- 线程6: 网络管理（独立状态机，负责 WiFi 持续恢复） ---
     * 必须比自启动早，否则自启动阶段的 WiFi 连接将不会通过 net_manager。 */
    if (net_manager_start() == RT_EOK)
    {
        rt_kprintf("[Main] net_manager started\n");
    }
    else
    {
        rt_kprintf("[Main] net_manager failed to start\n");
    }

    rt_thread_t autostart = rt_thread_create("autostart",
                                              autostart_thread_entry,
                                              RT_NULL,
                                              AUTOSTART_THREAD_STACK_SIZE,
                                              AUTOSTART_THREAD_PRIORITY,
                                              10);
    if (autostart != RT_NULL)
    {
        rt_thread_startup(autostart);
        autostart_ok = RT_TRUE;
        rt_kprintf("[Main] Autostart thread created\n");
    }
    else
    {
        rt_kprintf("[Main] Failed to create autostart thread\n");
        init_success = RT_FALSE;
        init_status_update(INIT_MODULE_WIFI, INIT_STATUS_FAIL);
    }

    /* 不再需要显示"系统初始化成功"，状态已在界面上显示 */

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
