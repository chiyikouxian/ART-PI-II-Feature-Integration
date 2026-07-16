/**
 * @file button.c
 * @brief PE0按键驱动 - 按下切换ROCK推理模式（单词/句子）
 * @note  按键接在GND与PE0之间，内部上拉，低电平有效。
 *        采用20ms轮询 + 连续3次低电平确认消抖，
 *        每次按下触发一次模式切换请求，松开后才允许下次触发。
 *
 *        按键线程本身不访问socket；它只调用 imu_wifi_sender 的
 *        模式切换请求接口，由 IMU WiFi 发送线程负责实际发送。
 */

#include <rtthread.h>
#include <rtdevice.h>
#include "drv_common.h"
#include "button.h"
#include "imu_wifi_sender.h"

#define BUTTON_PIN          GET_PIN(E, 0)
#define DEBOUNCE_COUNT      3       /* 连续低电平确认次数 */
#define POLL_INTERVAL_MS    20      /* 轮询间隔 */

void button_thread_entry(void *parameter)
{
    rt_uint8_t debounce = 0;
    rt_bool_t  pressed  = RT_FALSE;

    (void)parameter;

    rt_pin_mode(BUTTON_PIN, PIN_MODE_INPUT_PULLUP);
    rt_kprintf("[Button] PE0 initialized (pull-up, active low)\n");

    while (1)
    {
        rt_thread_mdelay(POLL_INTERVAL_MS);

        if (rt_pin_read(BUTTON_PIN) == PIN_LOW)
        {
            if (debounce < DEBOUNCE_COUNT)
                debounce++;

            if (debounce == DEBOUNCE_COUNT && !pressed)
            {
                pressed = RT_TRUE;

                rt_kprintf("[Button] Pressed, request ROCK mode switch\n");
                imu_wifi_sender_request_inference_mode_switch();
            }
        }
        else
        {
            debounce = 0;
            pressed  = RT_FALSE;
        }
    }
}
