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

/* Rock服务器配置
 * Plan B（固定内部网络）：ROCK 自建热点（热点接口 wlan1，热点侧固定 IP
 * 192.168.1.1/24；热点 SSID/密码为已通过真机验证的凭据，定义在 main.c）。
 * 左手固定连接 192.168.1.1:9101。
 * 上电流程：连接热点 -> 等待 DHCP/WiFi Ready -> 主动连接下面地址。
 * ROCK 的外部 WiFi 接口地址变化不影响本内部链路。 */
#define ROCK_SERVER_IP       "192.168.1.1"
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

/**
 * @brief  ROCK 推理模式（决定滑动窗口使用词级还是句级参数）。
 */
typedef enum {
    INFER_MODE_WORD     = 0,
    INFER_MODE_SENTENCE = 1,
} inference_mode_t;

/**
 * @brief  查询当前期望的 ROCK 推理模式。
 *
 * @note   在 imu_wifi_sender 启动前调用，结果为默认值 INFER_MODE_SENTENCE。
 *         线程安全（原子读取）。
 */
inference_mode_t imu_wifi_sender_get_inference_mode(void);

/**
 * @brief  触发 ROCK 推理模式切换请求。
 *
 *         本调用不直接发送数据，仅把“期望模式 + generation”原子地翻转，
 *         真正的协议发送由 imu_wifi_sender 线程在同一 socket 上完成。
 *
 * @note   - 按键线程与 IMU WiFi 线程均可调用，线程安全。
 *         - 在 imu_wifi_sender 尚未启动时调用也安全（状态被保存，
 *           下一次连接建立后第一帧数据前会自动发布）。
 *         - 不依赖任何额外的 mutex/event 初始化。
 *         - generation 按无符号整数自然回绕，比较时仅判等。
 */
void imu_wifi_sender_request_inference_mode_switch(void);

#endif /* __IMU_WIFI_SENDER_H */
