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

/* STT状态显示 */
#include "../STT/stt_manager.h"

/* OLED连接在TCA9548A的通道号 */
#define OLED_TCA9548A_CHANNEL   3

/* 外部函数声明 - 获取WiFi状态 */
extern rt_bool_t get_wifi_connected(void);
extern const char* get_wifi_ssid(void);
extern const char* get_wifi_password(void);

/* STT状态文本 */
static const char *stt_state_text[] = {
    "Listening...",
    "Recording...",
    "Encoding...",
    "Uploading...",
    "Result:",
    "Error!"
};

/**
 * @brief  IIC/OLED线程入口函数
 * @param  parameter 线程参数(未使用)
 * @note   该函数供main.c创建线程时使用
 */
void iic_thread_entry(void *parameter)
{
    rt_bool_t wifi_status;
    const char *ssid;
    const char *password;
    stt_state_t last_stt_state = STT_STATE_IDLE;

    rt_kprintf("[IIC Thread] Started\n");

    /* 首先初始化IIC引脚 */
    OLED_I2C_Init();
    rt_kprintf("[IIC Thread] I2C GPIO initialized\n");

    /* 初始化TCA9548A */
    tca9548a_init();

    /* 切换到OLED所在的通道3 */
    if (tca9548a_select_channel(OLED_TCA9548A_CHANNEL) != RT_EOK)
    {
        rt_kprintf("[IIC Thread] Failed to select channel %d\n", OLED_TCA9548A_CHANNEL);
        return;
    }
    rt_kprintf("[IIC Thread] TCA9548A channel %d selected\n", OLED_TCA9548A_CHANNEL);

    /* 初始化OLED (跳过GPIO初始化，因为已经初始化过了) */
    OLED_Init();
    rt_kprintf("[IIC Thread] OLED initialized\n");

    /* 再次确保在OLED通道 */
    tca9548a_select_channel(OLED_TCA9548A_CHANNEL);

    /* 获取WiFi状态信息 */
    wifi_status = get_wifi_connected();
    ssid = get_wifi_ssid();
    password = get_wifi_password();

    /* 打印WiFi连接信息到串口 */
    rt_kprintf("\n========== OLED/WiFi Info ==========\n");
    rt_kprintf("[OLED] WiFi Status: %s\n", wifi_status ? "Connected" : "Disconnected");
    rt_kprintf("[OLED] WiFi SSID: %s\n", ssid);
    rt_kprintf("[OLED] WiFi Password: %s\n", password);
    rt_kprintf("=====================================\n\n");

    /* 显示欢迎信息和WiFi状态 */
    OLED_ShowString(0, 0, "ART-PI2", OLED_8X16);

    if (wifi_status)
        OLED_ShowString(0, 16, "WiFi:OK", OLED_8X16);
    else
        OLED_ShowString(0, 16, "WiFi:NO", OLED_8X16);

    /* 显示SSID */
    OLED_ShowString(0, 32, "SSID:", OLED_6X8);
    OLED_ShowString(30, 32, (char*)ssid, OLED_6X8);

    /* STT状态行(第5-6行, Y=40-48) */
    OLED_ShowString(0, 48, "STT:Waiting...", OLED_6X8);

    /* STT结果行(第7-8行, Y=56) */
    OLED_ShowString(0, 56, "", OLED_6X8);

    OLED_Update();
    rt_kprintf("[IIC Thread] OLED display updated\n");

    while (1)
    {
        /* 确保在OLED通道 */
        tca9548a_select_channel(OLED_TCA9548A_CHANNEL);

        /* 更新WiFi状态 */
        wifi_status = get_wifi_connected();
        if (wifi_status)
            OLED_ShowString(0, 16, "WiFi:OK", OLED_8X16);
        else
            OLED_ShowString(0, 16, "WiFi:NO", OLED_8X16);

        /* 更新STT状态 */
        stt_state_t cur_state = stt_manager_get_state();

        if (cur_state != last_stt_state)
        {
            /* 清除STT行 */
            OLED_ClearArea(0, 40, 128, 24);

            if (cur_state < sizeof(stt_state_text) / sizeof(stt_state_text[0]))
            {
                OLED_ShowString(0, 40, (char *)stt_state_text[cur_state], OLED_6X8);
            }

            /* 如果有识别结果，显示在最后一行 */
            if (cur_state == STT_STATE_DISPLAYING)
            {
                const char *text = stt_manager_get_last_text();
                if (text && text[0] != '\0')
                {
                    /* 截取前21字符显示(OLED 128px / 6px = 21字符) */
                    OLED_ShowString(0, 48, (char *)text, OLED_6X8);
                }
            }

            last_stt_state = cur_state;
        }

        /* 更新OLED显示数据 */
        OLED_Update();

        /* 每300ms更新一次 */
        rt_thread_mdelay(300);
    }
}
