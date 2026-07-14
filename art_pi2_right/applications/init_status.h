/**
 * @file init_status.h
 * @brief 系统初始化状态显示模块 - 在OLED上显示各模块初始化状态
 *
 * @details OLED显示布局 (128×64像素):
 *          ┌────────────────────────────────────────────┐
 *          │ Y=0~7   (8px):  状态栏 [电压 X.XXV XX%][图标] │
 *          │ Y=8~15  (8px):  WIFI: OK/FAIL              │
 *          │ Y=16~23 (8px):  TCP:  OK/FAIL              │
 *          │ Y=24~31 (8px):  I2C:  OK/FAIL              │
 *          │ Y=32~39 (8px):  UART: OK/FAIL              │
 *          │ Y=40~47 (8px):  IMU:  X/11                 │
 *          │ Y=48~55 (8px):  STT:  OK/FAIL              │
 *          │ Y=56~63 (8px):  (预留)                     │
 *          └────────────────────────────────────────────┘
 */

#ifndef __INIT_STATUS_H
#define __INIT_STATUS_H

#include <rtthread.h>

/* 初始化模块类型 */
typedef enum
{
    INIT_MODULE_WIFI = 0,
    INIT_MODULE_TCP,
    INIT_MODULE_I2C,
    INIT_MODULE_UART,
    INIT_MODULE_IMU,
    INIT_MODULE_STT,
    INIT_MODULE_MAX
} init_module_t;

/* 初始化状态 */
typedef enum
{
    INIT_STATUS_PENDING,    /* 等待初始化 */
    INIT_STATUS_OK,         /* 初始化成功 */
    INIT_STATUS_FAIL        /* 初始化失败 */
} init_status_t;

/**
 * @brief  初始化状态显示模块
 * @note   在OLED上显示初始化状态界面，所有模块默认为PENDING状态
 */
void init_status_display_init(void);

/**
 * @brief  更新指定模块的初始化状态
 * @param  module 模块类型
 * @param  status 状态（OK/FAIL）
 */
void init_status_update(init_module_t module, init_status_t status);

/**
 * @brief  更新IMU模块的初始化计数（显示格式: X/Y）
 * @param  ok_count    成功初始化的IMU数量
 * @param  total_count IMU总数
 */
void init_status_update_imu_count(rt_uint8_t ok_count, rt_uint8_t total_count);

/**
 * @brief  检查是否所有模块都初始化成功
 * @return RT_TRUE 全部成功, RT_FALSE 有失败或未完成
 */
rt_bool_t init_status_all_ok(void);

/**
 * @brief  检查是否还在显示初始化状态
 * @return RT_TRUE 正在显示初始化状态, RT_FALSE 已切换到正常显示
 */
rt_bool_t init_status_is_showing(void);

/**
 * @brief  退出初始化状态显示，切换到正常显示模式
 * @note   通常在第一次收到语音转文字结果时调用
 */
void init_status_exit(void);

/**
 * @brief  在OLED左上角(0,0)显示校准状态文字（6x8字体）
 * @param  text 要显示的文字（如 "CAL" / "DONE"）
 */
void init_status_show_calib(const char *text);

/**
 * @brief  清除OLED左上角的校准状态文字
 */
void init_status_clear_calib(void);

#endif /* __INIT_STATUS_H */
