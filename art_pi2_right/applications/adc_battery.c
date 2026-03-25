/**
 * @file    adc_battery.c
 * @brief   锂电池电压ADC采集线程实现 (直接使用STM32 HAL库)
 *
 * @details 硬件连接:
 *          锂电池(+) ──── R1(1KΩ) ──┬── R2(1KΩ) ──── GND
 *                                    │
 *                                   PC2 (ADC1_INP12)
 *
 *          分压公式: V_adc = V_bat × R2/(R1+R2) = V_bat × 1/2
 *          实际电压: V_bat = V_adc × 2
 *
 *          满电压 4.1V 时，ADC采样点电压 = 2.05V
 *          ADC 12位分辨率，参考电压 3.3V
 *          ADC值 = 2.05 / 3.3 × 4096 ≈ 2546
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include "adc_battery.h"

/* 电池电压全局变量 (mV) */
static rt_uint32_t g_battery_voltage_mv = 0;
/* 电池电量百分比 */
static rt_uint8_t g_battery_percentage = 0;

/* ADC句柄 */
static ADC_HandleTypeDef hadc1;

/**
 * @brief   获取当前电池电压
 */
rt_uint32_t battery_get_voltage(void)
{
    return g_battery_voltage_mv;
}

/**
 * @brief   获取当前电池电量百分比
 */
rt_uint8_t battery_get_percentage(void)
{
    return g_battery_percentage;
}

/**
 * @brief   初始化ADC1硬件 (PC2 = ADC1 Channel 12)
 * @return  RT_EOK 成功, -RT_ERROR 失败
 */
static rt_err_t battery_adc_init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    ADC_ChannelConfTypeDef sConfig = {0};

    /* 使能时钟 */
    __HAL_RCC_ADC12_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    /* 配置PC2为模拟输入 */
    GPIO_InitStruct.Pin = GPIO_PIN_2;
    GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    /* 配置ADC1 */
    hadc1.Instance = ADC1;
    hadc1.Init.ClockPrescaler        = ADC_CLOCK_SYNC_PCLK_DIV4;
    hadc1.Init.Resolution            = ADC_RESOLUTION_12B;
    hadc1.Init.ScanConvMode          = ADC_SCAN_DISABLE;
    hadc1.Init.EOCSelection          = ADC_EOC_SINGLE_CONV;
    hadc1.Init.LowPowerAutoWait      = DISABLE;
    hadc1.Init.ContinuousConvMode    = DISABLE;
    hadc1.Init.NbrOfConversion       = 1;
    hadc1.Init.DiscontinuousConvMode = DISABLE;
    hadc1.Init.ExternalTrigConv      = ADC_SOFTWARE_START;
    hadc1.Init.ExternalTrigConvEdge  = ADC_EXTERNALTRIGCONVEDGE_NONE;
    hadc1.Init.ConversionDataManagement = ADC_CONVERSIONDATA_DR;
    hadc1.Init.Overrun               = ADC_OVR_DATA_OVERWRITTEN;
    hadc1.Init.OversamplingMode      = DISABLE;

    if (HAL_ADC_Init(&hadc1) != HAL_OK)
    {
        rt_kprintf("[Battery] ADC1 init failed!\n");
        return -RT_ERROR;
    }

    /* 校准ADC */
    if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED) != HAL_OK)
    {
        rt_kprintf("[Battery] ADC1 calibration failed!\n");
        return -RT_ERROR;
    }

    /* 配置通道12 (PC2) */
    sConfig.Channel      = ADC_CHANNEL_12;
    sConfig.Rank         = ADC_REGULAR_RANK_1;
    sConfig.SamplingTime = ADC_SAMPLETIME_47CYCLES_5;
    sConfig.SingleDiff   = ADC_SINGLE_ENDED;
    sConfig.OffsetNumber = ADC_OFFSET_NONE;
    sConfig.Offset       = 0;

    if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
    {
        rt_kprintf("[Battery] ADC1 channel config failed!\n");
        return -RT_ERROR;
    }

    rt_kprintf("[Battery] ADC1 CH12 (PC2) initialized OK\n");
    return RT_EOK;
}

/**
 * @brief   读取一次ADC值
 * @return  ADC原始值 (0-4095)，失败返回0
 */
static rt_uint32_t battery_adc_read(void)
{
    /* 启动转换 */
    HAL_ADC_Start(&hadc1);

    /* 等待转换完成 */
    if (HAL_ADC_PollForConversion(&hadc1, 100) != HAL_OK)
    {
        return 0;
    }

    /* 获取转换结果 */
    return (rt_uint32_t)HAL_ADC_GetValue(&hadc1);
}

/**
 * @brief   将ADC原始值转换为电池实际电压 (mV)
 * @param   adc_value   ADC原始采样值 (12位)
 * @return  电池实际电压，单位mV
 */
static rt_uint32_t adc_to_battery_voltage(rt_uint32_t adc_value)
{
    rt_uint32_t adc_mv;
    rt_uint32_t bat_mv;

    /* ADC采样点电压 (mV) = adc_value / 4096 × 3300 */
    adc_mv = (adc_value * ADC_REF_VOLTAGE) / ADC_RESOLUTION;

    /* 实际电池电压 = ADC电压 × 分压比 */
    bat_mv = adc_mv * BATTERY_DIVIDER_RATIO;

    return bat_mv;
}

/**
 * @brief   计算电池电量百分比 (线性映射)
 * @param   voltage_mv  电池电压，单位mV
 * @return  电量百分比 0-100
 */
static rt_uint8_t calculate_percentage(rt_uint32_t voltage_mv)
{
    if (voltage_mv >= BATTERY_FULL_VOLTAGE)
        return 100;
    if (voltage_mv <= BATTERY_EMPTY_VOLTAGE)
        return 0;

    return (rt_uint8_t)((voltage_mv - BATTERY_EMPTY_VOLTAGE) * 100
                        / (BATTERY_FULL_VOLTAGE - BATTERY_EMPTY_VOLTAGE));
}

/**
 * @brief   电池ADC采集线程入口
 * @details 每1秒采集一次ADC值，进行滑动平均滤波后计算电池电压和电量。
 *          采集结果通过 battery_get_voltage() 和 battery_get_percentage() 获取。
 */
void battery_thread_entry(void *parameter)
{
    rt_uint32_t adc_value = 0;

    /* 滑动平均滤波缓冲区 */
    #define FILTER_SIZE 8
    rt_uint32_t filter_buf[FILTER_SIZE] = {0};
    rt_uint8_t filter_idx = 0;
    rt_uint8_t filter_count = 0;

    (void)parameter;

    /* 等待系统稳定 */
    rt_thread_mdelay(500);

    /* 初始化ADC硬件 */
    if (battery_adc_init() != RT_EOK)
    {
        rt_kprintf("[Battery] ADC init failed, thread exit\n");
        return;
    }

    while (1)
    {
        /* 读取ADC值 */
        adc_value = battery_adc_read();

        /* 存入滤波缓冲区 */
        filter_buf[filter_idx] = adc_value;
        filter_idx = (filter_idx + 1) % FILTER_SIZE;
        if (filter_count < FILTER_SIZE)
            filter_count++;

        /* 计算滑动平均 */
        rt_uint32_t sum = 0;
        for (rt_uint8_t i = 0; i < filter_count; i++)
        {
            sum += filter_buf[i];
        }
        rt_uint32_t avg_adc = sum / filter_count;

        /* 转换为电池电压 */
        g_battery_voltage_mv = adc_to_battery_voltage(avg_adc);

        /* 计算电量百分比 */
        g_battery_percentage = calculate_percentage(g_battery_voltage_mv);

        /* 每1秒采集一次 */
        rt_thread_mdelay(1000);
    }
}

/**
 * @brief   MSH命令: 查询电池电压和电量
 */
static void bat(void)
{
    rt_kprintf("Battery: %u.%03uV  %u%%\n",
               g_battery_voltage_mv / 1000,
               g_battery_voltage_mv % 1000,
               g_battery_percentage);
}
MSH_CMD_EXPORT(bat, Show battery voltage and percentage);
