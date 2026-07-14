/**
 * @file init_status.c
 * @brief 系统初始化状态显示模块实现
 */

#include "init_status.h"
#include "../IIC/tca9548a.h"
#include "../IIC/OLED/OLED.h"
#include "../IIC/OLED/OLED_Data.h"
#include "../IIC/i2c2_mutex.h"
#include "../applications/adc_battery.h"

/* OLED连接在TCA9548A的通道号 */
#define OLED_TCA9548A_CHANNEL   3

/* 状态显示位置 */
#define STATUS_START_Y          8       /* 状态栏下方开始 */
#define STATUS_LINE_HEIGHT      8       /* 每行高度（使用6x8小字体） */
#define STATUS_LABEL_X          0       /* 标签X位置 */
#define STATUS_VALUE_X          48      /* 状态值X位置 */

/* 电池状态栏参数（与uart_oled_thread.c保持一致） */
#define BAT_TEXT_X              48
#define BAT_TEXT_Y              0
#define BAT_ICON_W              16
#define BAT_ICON_H              8

/* 模块名称 */
static const char *module_names[INIT_MODULE_MAX] = {
    "WIFI:",
    "TCP: ",
    "I2C: ",
    "UART:",
    "IMU: ",
    "STT: "
};


/* 各模块的初始化状态 */
static init_status_t module_status[INIT_MODULE_MAX] = {
    INIT_STATUS_PENDING,
    INIT_STATUS_PENDING,
    INIT_STATUS_PENDING,
    INIT_STATUS_PENDING,
    INIT_STATUS_PENDING,
    INIT_STATUS_PENDING
};

/* IMU计数显示（ok_count/total_count，total_count为0表示未设置） */
static rt_uint8_t imu_ok_count = 0;
static rt_uint8_t imu_total_count = 0;

/* 是否正在显示初始化状态 */
static rt_bool_t is_showing_init_status = RT_FALSE;

/* I2C总线互斥锁操作 */
static rt_err_t oled_acquire_bus(void)
{
    if (i2c2_mutex_take(RT_WAITING_FOREVER) != RT_EOK)
        return -RT_ERROR;

    /* MPU6050线程使用RT-Thread I2C框架后可能改变GPIO状态，需要重新初始化 */
    OLED_I2C_Init();

    if (tca9548a_is_present())
    {
        tca9548a_disable_all_channels();
        tca9548a_select_channel(OLED_TCA9548A_CHANNEL);
    }

    return RT_EOK;
}

static void oled_release_bus(void)
{
    i2c2_mutex_release();
}

/* 根据电池百分比获取对应的电池图标 */
static const uint8_t *battery_get_icon(rt_uint8_t percentage)
{
    if (percentage >= 100)     return Battery100;
    else if (percentage >= 92) return Battery92_99;
    else if (percentage >= 84) return Battery84_91;
    else if (percentage >= 76) return Battery76_83;
    else if (percentage >= 68) return Battery68_75;
    else if (percentage >= 60) return Battery60_67;
    else if (percentage >= 52) return Battery52_59;
    else if (percentage >= 44) return Battery44_51;
    else if (percentage >= 36) return Battery36_43;
    else if (percentage >= 28) return Battery28_35;
    else if (percentage >= 19) return Battery19_27;
    else if (percentage >= 10) return Battery10_18;
    else if (percentage >= 1)  return Battery1_9;
    else                       return Battery0;
}

/* 更新电池状态栏 */
static void update_battery_bar(void)
{
    rt_uint32_t voltage_mv = battery_get_voltage();
    rt_uint8_t  percentage = battery_get_percentage();
    const uint8_t *icon = battery_get_icon(percentage);

    char bat_str[12];
    rt_snprintf(bat_str, sizeof(bat_str), "%d.%02dV%3d%%",
                voltage_mv / 1000,
                (voltage_mv % 1000) / 10,
                percentage);

    OLED_ClearArea(BAT_TEXT_X, BAT_TEXT_Y, 128 - BAT_TEXT_X, BAT_ICON_H);
    OLED_ShowString(BAT_TEXT_X, BAT_TEXT_Y, bat_str, OLED_6X8);
    OLED_ShowImage(128 - BAT_ICON_W, BAT_TEXT_Y, BAT_ICON_W, BAT_ICON_H, icon);
}

/* 渲染所有模块状态 */
static void render_all_status(void)
{
    int i;
    int y_pos;

    /* 清空屏幕 */
    OLED_Clear();

    /* 显示电池状态栏 */
    update_battery_bar();

    /* 显示各模块状态，PENDING状态不显示任何内容 */
    for (i = 0; i < INIT_MODULE_MAX; i++)
    {
        y_pos = STATUS_START_Y + i * STATUS_LINE_HEIGHT;

        if (i == INIT_MODULE_IMU)
        {
            /* IMU显示计数格式: X/Y */
            if (imu_total_count > 0)
            {
                char imu_str[8];
                OLED_ShowString(STATUS_LABEL_X, y_pos, (char *)module_names[i], OLED_6X8);
                rt_snprintf(imu_str, sizeof(imu_str), "%d/%d", imu_ok_count, imu_total_count);
                OLED_ShowString(STATUS_VALUE_X, y_pos, imu_str, OLED_6X8);
            }
        }
        else if (module_status[i] == INIT_STATUS_OK)
        {
            OLED_ShowString(STATUS_LABEL_X, y_pos, (char *)module_names[i], OLED_6X8);
            OLED_ShowString(STATUS_VALUE_X, y_pos, "OK", OLED_6X8);
        }
        else if (module_status[i] == INIT_STATUS_FAIL)
        {
            OLED_ShowString(STATUS_LABEL_X, y_pos, (char *)module_names[i], OLED_6X8);
            OLED_ShowString(STATUS_VALUE_X, y_pos, "FAIL", OLED_6X8);
        }
    }

    /* 刷新显示到OLED硬件 */
    OLED_Update();

    /* 延迟一下，确保显示已经刷新到硬件 */
    rt_thread_mdelay(10);
}

void init_status_display_init(void)
{
    int i;

    /* 重置所有状态为PENDING */
    for (i = 0; i < INIT_MODULE_MAX; i++)
    {
        module_status[i] = INIT_STATUS_PENDING;
    }

    /* 设置为正在显示初始化状态 */
    is_showing_init_status = RT_TRUE;

    /* 获取I2C总线并显示初始状态 */
    if (oled_acquire_bus() != RT_EOK)
        return;

    render_all_status();
    oled_release_bus();
}

void init_status_update(init_module_t module, init_status_t status)
{
    if (module >= INIT_MODULE_MAX)
        return;

    rt_kprintf("[INIT_STATUS] Update module %d to status %d (before: %d)\n",
               module, status, module_status[module]);

    /* 更新状态 */
    module_status[module] = status;

    rt_kprintf("[INIT_STATUS] After update: module_status[%d] = %d\n",
               module, module_status[module]);

    /* 刷新显示（无论是否在显示初始化状态） */
    if (oled_acquire_bus() != RT_EOK)
    {
        rt_kprintf("[INIT_STATUS] Failed to acquire OLED bus\n");
        return;
    }

    render_all_status();
    oled_release_bus();
    rt_kprintf("[INIT_STATUS] Display updated\n");
}

rt_bool_t init_status_all_ok(void)
{
    int i;

    for (i = 0; i < INIT_MODULE_MAX; i++)
    {
        if (module_status[i] != INIT_STATUS_OK)
            return RT_FALSE;
    }

    return RT_TRUE;
}

rt_bool_t init_status_is_showing(void)
{
    return is_showing_init_status;
}

void init_status_exit(void)
{
    rt_kprintf("[INIT_STATUS] Exiting init status display\n");
    is_showing_init_status = RT_FALSE;
}

void init_status_update_imu_count(rt_uint8_t ok_count, rt_uint8_t total_count)
{
    imu_ok_count = ok_count;
    imu_total_count = total_count;

    rt_kprintf("[INIT_STATUS] IMU count: %d/%d\n", ok_count, total_count);

    if (oled_acquire_bus() != RT_EOK)
    {
        rt_kprintf("[INIT_STATUS] Failed to acquire OLED bus\n");
        return;
    }

    render_all_status();
    oled_release_bus();
    rt_kprintf("[INIT_STATUS] IMU display updated\n");
}

/* 校准文字显示区域：左上角 (0,0)，宽度48px足以显示 "DONE" */
#define CALIB_TEXT_X    0
#define CALIB_TEXT_Y    0
#define CALIB_TEXT_W    30
#define CALIB_TEXT_H    8

void init_status_show_calib(const char *text)
{
    if (oled_acquire_bus() != RT_EOK)
        return;

    OLED_ClearArea(CALIB_TEXT_X, CALIB_TEXT_Y, CALIB_TEXT_W, CALIB_TEXT_H);
    OLED_ShowString(CALIB_TEXT_X, CALIB_TEXT_Y, (char *)text, OLED_6X8);
    OLED_UpdateArea(CALIB_TEXT_X, CALIB_TEXT_Y, CALIB_TEXT_W, CALIB_TEXT_H);

    oled_release_bus();
}

void init_status_clear_calib(void)
{
    if (oled_acquire_bus() != RT_EOK)
        return;

    OLED_ClearArea(CALIB_TEXT_X, CALIB_TEXT_Y, CALIB_TEXT_W, CALIB_TEXT_H);
    OLED_UpdateArea(CALIB_TEXT_X, CALIB_TEXT_Y, CALIB_TEXT_W, CALIB_TEXT_H);

    oled_release_bus();
}
