/**
 * @file vtx316.h
 * @brief VTX316语音合成芯片驱动头文件
 * @note  通过USART1 (PF13-TX, PF12-RX) 与VTX316通信
 *        协议: [帧头0xFD] [长度高] [长度低] [命令字0x01] [编码0x05=UTF-8] [文本...]
 *        播报完成回复: 0x41 0x4F
 */

#ifndef __VTX316_H
#define __VTX316_H

#include <rtthread.h>

/* 线程配置 */
#define VTX316_THREAD_PRIORITY     20
#define VTX316_THREAD_STACK_SIZE   2048
#define VTX316_THREAD_TIMESLICE    10

/* VTX316使用的串口设备名 (USART1: PF13-TX, PF12-RX) */
#define VTX316_UART_NAME           "uart1"
#define VTX316_UART_BAUDRATE       115200

/* VTX316协议帧常量 */
#define VTX316_FRAME_HEADER        0xFD
#define VTX316_CMD_SPEAK           0x01    /* 合成播报命令 */
#define VTX316_ENCODING_UTF8       0x05    /* UTF-8编码 */

/* VTX316播报完成回复 */
#define VTX316_REPLY_DONE_0        0x41    /* 'A' */
#define VTX316_REPLY_DONE_1        0x4F    /* 'O' */

/* 播报完成等待超时 (ms) */
#define VTX316_SPEAK_TIMEOUT_MS    30000

/* 启动阶段语音错误码 */
typedef enum
{
    VTX316_BOOT_ERROR_SENSOR = 0,
    VTX316_BOOT_ERROR_WIFI_DEVICE,      /* WLAN设备未就绪 */
    VTX316_BOOT_ERROR_WIFI_CONNECT,     /* WiFi连接调用失败 */
    VTX316_BOOT_ERROR_WIFI_TIMEOUT,     /* WiFi连接超时 */
    VTX316_BOOT_ERROR_TCP,
    VTX316_BOOT_ERROR_VOICE,
    VTX316_BOOT_ERROR_BLE,
    VTX316_BOOT_ERROR_MAX
} vtx316_boot_error_t;

/**
 * @brief  VTX316语音播报 - 发送UTF-8文本到语音合成芯片 (非阻塞)
 * @param  text: UTF-8编码的文本字符串
 * @note   如果当前正在播报则直接返回不发送
 */
void vtx316_speak(const char *text);
rt_err_t vtx316_speak_wait(const char *text);
rt_bool_t vtx316_is_busy(void);
rt_bool_t vtx316_is_ready(void);
void vtx316_report_boot_error(vtx316_boot_error_t error);
void vtx316_report_boot_success(void);
void vtx316_thread_entry(void *parameter);
void vtx316_tcp_recv_handler(const char *data, int len);

#endif /* __VTX316_H */
