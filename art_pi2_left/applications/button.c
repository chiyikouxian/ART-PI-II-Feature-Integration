/**
 * @file button.c
 * @brief PE0按键驱动 - 按下播报当前电量
 * @note  按键接在GND与PE0之间，内部上拉，低电平有效。
 *        采用20ms轮询 + 连续3次低电平确认消抖，
 *        每次按下触发一次语音播报，松开后才允许下次触发。
 */

#include <rtthread.h>
#include <rtdevice.h>
#include "drv_common.h"
#include "button.h"
#include "adc_battery.h"
#include "../vtx316/vtx316.h"

#define BUTTON_PIN          GET_PIN(E, 0)
#define DEBOUNCE_COUNT      3       /* 连续低电平确认次数 */
#define POLL_INTERVAL_MS    20      /* 轮询间隔 */

void button_thread_entry(void *parameter)
{
    rt_uint8_t debounce = 0;
    rt_bool_t  pressed  = RT_FALSE;
    char       speak_buf[16];

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

                rt_uint8_t pct = battery_get_percentage();
                rt_snprintf(speak_buf, sizeof(speak_buf), "电量%d", pct);
                rt_kprintf("[Button] Pressed, speak: %s\n", speak_buf);
                vtx316_speak(speak_buf);
            }
        }
        else
        {
            debounce = 0;
            pressed  = RT_FALSE;
        }
    }
}
