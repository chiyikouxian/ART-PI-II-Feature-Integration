/**
 * @file    adc_battery.h
 * @brief   锂电池电压ADC采集线程
 *
 * @details 使用PC2引脚 (ADC1 Channel 12) 通过2个1KΩ电阻分压采集锂电池电压。
 *          分压比 = 1/2，满电压 4.1V，ADC采样点对应 2.05V。
 *          直接使用STM32 HAL库操作ADC (不依赖RT-Thread ADC框架)。
 */

#ifndef __ADC_BATTERY_H__
#define __ADC_BATTERY_H__

#include <rtthread.h>

/* 电池ADC线程配置 */
#define BATTERY_THREAD_STACK_SIZE   1024
#define BATTERY_THREAD_PRIORITY     22
#define BATTERY_THREAD_TIMESLICE    10

/* 电池参数 */
#define BATTERY_FULL_VOLTAGE        4200        /* 满电压 4.2V (mV) */
#define BATTERY_EMPTY_VOLTAGE       3000        /* 截止电压 3.0V (mV) */
#define BATTERY_DIVIDER_RATIO       2           /* 分压比: (R1+R2)/R2 = 2 */

/* ADC参数 (STM32H7RS ADC1 最大12位分辨率) */
#define ADC_REF_VOLTAGE             3300        /* 参考电压 3.3V (mV) */
#define ADC_RESOLUTION              4096        /* 12位ADC: 2^12 = 4096 */

/**
 * @brief   电池ADC采集线程入口函数
 * @param   parameter   线程参数 (未使用)
 */
void battery_thread_entry(void *parameter);

/**
 * @brief   获取当前电池电压 (mV)
 * @return  电池电压值，单位mV；未初始化时返回0
 */
rt_uint32_t battery_get_voltage(void);

/**
 * @brief   获取当前电池电量百分比
 * @return  电量百分比 0-100；未初始化时返回0
 */
rt_uint8_t battery_get_percentage(void);

#endif /* __ADC_BATTERY_H__ */
