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

/**
 * @brief  VTX316语音播报 - 发送UTF-8文本到语音合成芯片 (非阻塞)
 * @param  text: UTF-8编码的文本字符串
 * @note   如果当前正在播报则直接返回不发送
 */
void vtx316_speak(const char *text);

/**
 * @brief  VTX316语音播报 - 阻塞等待上一次播报完成后再发送
 * @param  text: UTF-8编码的文本字符串
 * @return RT_EOK 成功, -RT_ETIMEOUT 等待超时
 */
rt_err_t vtx316_speak_wait(const char *text);

/**
 * @brief  查询VTX316是否正在播报
 * @return RT_TRUE 正在播报, RT_FALSE 空闲
 */
rt_bool_t vtx316_is_busy(void);

/**
 * @brief  VTX316线程入口函数
 * @param  parameter 线程参数(未使用)
 */
void vtx316_thread_entry(void *parameter);

/**
 * @brief  TCP接收回调: 解析文本命令并通过VTX316播报
 * @param  data  接收到的数据 (格式: "SAY:要播报的文本")
 * @param  len   数据长度
 */
void vtx316_tcp_recv_handler(const char *data, int len);

#endif /* __VTX316_H */
