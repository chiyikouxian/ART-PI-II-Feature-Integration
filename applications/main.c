/*
 * Copyright (c) 2006-2020, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2020-09-02     RT-Thread    first version
 * 2025-01-27     AI           Added WiFi and INMP441 audio capture support
 */

#include <rtthread.h>
#include <rtdevice.h>
#include "drv_common.h"
#include "../IIC/iic_thread.h"

#ifdef RT_USING_WIFI
#include <wlan_mgnt.h>
#include <wlan_cfg.h>
#include <wlan_prot.h>
#endif

#define LED_PIN GET_PIN(O, 5)

#define WIFI_SSID "CMCC-Vm3m"
#define WIFI_PASSWORD "w3wegscf"

/* WiFi连接状态 */
static rt_bool_t wifi_connected = RT_FALSE;
static char wifi_ssid[32] = {0};
static char wifi_password[64] = {0};

/* IIC/OLED线程控制块和栈 */
static struct rt_thread iic_thread;
static rt_uint8_t iic_thread_stack[IIC_THREAD_STACK_SIZE];

/* 获取WiFi连接状态 */
rt_bool_t get_wifi_connected(void)
{
    return wifi_connected;
}

/* 获取WiFi SSID */
const char* get_wifi_ssid(void)
{
    return wifi_ssid;
}

/* 获取WiFi密码 */
const char* get_wifi_password(void)
{
    return wifi_password;
}

#ifdef RT_USING_WIFI
/* 等待WiFi底层初始化完成 */
static rt_err_t wait_wlan_init_done(rt_uint32_t time_ms)
{
    rt_uint32_t time_cnt = 0;
    rt_device_t device = RT_NULL;

    /* 等待wlan设备注册完成 */
    while (time_cnt <= (time_ms / 100))
    {
        device = rt_device_find(RT_WLAN_DEVICE_STA_NAME);
        if (device != RT_NULL)
        {
            break;
        }
        time_cnt++;
        rt_thread_mdelay(100);
    }

    if (device == RT_NULL)
    {
        return -RT_ETIMEOUT;
    }

    /* 额外等待一段时间确保驱动完全就绪 */
    rt_thread_mdelay(500);

    return RT_EOK;
}

/* WiFi连接函数 */
static rt_err_t wifi_connect(const char *ssid, const char *password)
{
    rt_err_t result = RT_EOK;

    /* 保存SSID和密码 */
    rt_strncpy(wifi_ssid, ssid, sizeof(wifi_ssid) - 1);
    rt_strncpy(wifi_password, password, sizeof(wifi_password) - 1);

    rt_kprintf("\n========== WiFi Connection ==========\n");
    rt_kprintf("[WiFi] SSID: %s\n", ssid);
    rt_kprintf("[WiFi] Password: %s\n", password);
    rt_kprintf("[WiFi] Connecting...\n");

    /* 连接WiFi */
    result = rt_wlan_connect(ssid, password);

    if (result == RT_EOK)
    {
        /* 等待连接就绪 */
        rt_uint32_t timeout = 0;
        while (!rt_wlan_is_ready() && timeout < 100)
        {
            rt_thread_mdelay(100);
            timeout++;
        }

        if (rt_wlan_is_ready())
        {
            wifi_connected = RT_TRUE;
            rt_kprintf("[WiFi] Connected successfully!\n");
            rt_kprintf("[WiFi] Status: Connected\n");
        }
        else
        {
            wifi_connected = RT_FALSE;
            rt_kprintf("[WiFi] Connection timeout!\n");
            rt_kprintf("[WiFi] Status: Disconnected\n");
        }
    }
    else
    {
        wifi_connected = RT_FALSE;
        rt_kprintf("[WiFi] Connection failed! Error: %d\n", result);
        rt_kprintf("[WiFi] Status: Disconnected\n");
    }

    rt_kprintf("======================================\n\n");

    return result;
}
#endif

int main(void)
{
    rt_uint32_t count = 1;
    rt_err_t result;

    rt_pin_mode(LED_PIN, PIN_MODE_OUTPUT);

    /* 等待系统完全就绪 */
    rt_thread_mdelay(100);

#ifdef RT_USING_WIFI
    /* 等待WiFi底层初始化完成 */
    rt_kprintf("[Main] Waiting for WiFi initialization...\n");
    if (wait_wlan_init_done(10000) == RT_EOK)
    {
        rt_kprintf("[Main] WiFi initialization done\n");

        /* 连接WiFi */
        wifi_connect(WIFI_SSID, WIFI_PASSWORD);
    }
    else
    {
        rt_kprintf("[Main] WiFi initialization timeout!\n");
    }
#endif

    /* WiFi连接完成后，创建并启动IIC/OLED线程 */
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

    /* 注意: SAI/INMP441音频采集模块已移动到SAI文件夹 */
    /* 音频系统通过INIT_APP_EXPORT自动初始化，无需在此处调用 */

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
static int vtor_config(void)
{
    /* Vector Table Relocation in Internal XSPI2_BASE */
    SCB->VTOR = XSPI2_BASE;
    return 0;
}
INIT_BOARD_EXPORT(vtor_config);
