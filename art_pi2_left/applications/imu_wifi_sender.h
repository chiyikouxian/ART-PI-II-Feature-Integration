/**
 * @file imu_wifi_sender.h
 * @brief WiFi发送原始IMU数据到Rock边缘端 - 左手
 *
 * 功能：
 * - 通过WiFi/TCP发送原始IMU数据（未滤波）到Rock边缘端
 * - 数据格式：CSV格式，与BLE一致
 * - 发送频率：90ms (11.1Hz)
 * - 与现有TCP客户端（发送滤波数据给前端）并行运行
 */

#ifndef __IMU_WIFI_SENDER_H
#define __IMU_WIFI_SENDER_H

#include <rtthread.h>

/* Rock服务器配置 */
#define ROCK_SERVER_IP       "192.168.245.50"
#define ROCK_SERVER_PORT     9101          /* 左手端口 */

/* 线程配置 */
#define IMU_WIFI_THREAD_PRIORITY     23
#define IMU_WIFI_THREAD_STACK_SIZE   3072
#define IMU_WIFI_THREAD_TIMESLICE    10

/* 发送配置 */
#define IMU_WIFI_SEND_INTERVAL       90    /* 90ms = 11.1Hz */
#define IMU_WIFI_SEND_BUF_SIZE       1024  /* CSV数据约200-250字节 */
#define IMU_WIFI_SEND_DEADLINE_MS    70    /* 单次发送最长阻塞 70ms,
                                            * 余下 20ms 留给 recv 与模型数据 */

/**
 * @brief  启动IMU WiFi发送线程
 * @return RT_EOK 成功, 其他值失败
 */
rt_err_t imu_wifi_sender_start(void);

/**
 * @brief  停止IMU WiFi发送线程
 * @return RT_EOK 成功, 其他值失败
 */
rt_err_t imu_wifi_sender_stop(void);

/**
 * @brief  检查IMU WiFi发送线程是否运行
 * @return RT_TRUE 运行中, RT_FALSE 未运行
 */
rt_bool_t imu_wifi_sender_is_running(void);

/**
 * @brief  原子重置当前活跃 WiFi 流 frame_seq 为 0。
 *         由 operation_mode 在 CMD:RESET_SEQ / CMD:START / CMD:STOP
 *         以及自动唤醒/ROCK rejection 路径调用。
 *         线程安全，可从任意上下文调用。
 */
void imu_wifi_sender_reset_frame_seq(void);

/**
 * @brief  队列发送模型数据（校准阈值）到Rock端
 * @param  data 要发送的数据
 * @param  len  数据长度
 */
void imu_wifi_sender_send_model(const char *data, int len);

#endif /* __IMU_WIFI_SENDER_H */
