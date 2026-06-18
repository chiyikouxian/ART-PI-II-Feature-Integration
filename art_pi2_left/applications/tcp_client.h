/**
 * @file tcp_client.h
 * @brief TCP客户端线程 - 双向通信: 发送传感器数据到PC / 接收PC下发数据
 */

#ifndef __TCP_CLIENT_H
#define __TCP_CLIENT_H

#include <rtthread.h>

/* 服务器配置 - 修改为你电脑的IP地址 */
#define TCP_SERVER_IP       "192.168.221.217"
#define TCP_SERVER_PORT     8266

/* 线程配置 */
#define TCP_THREAD_PRIORITY     22
#define TCP_THREAD_STACK_SIZE   4096
#define TCP_THREAD_TIMESLICE    10

/* 发送缓冲区大小 (22通道JSON约2KB, 留余量) */
#define TCP_SEND_BUF_SIZE       4096

/* 接收缓冲区大小 */
#define TCP_RECV_BUF_SIZE       1024

/* 数据发送间隔 (ms), 与传感器采样率10Hz匹配 */
#define TCP_SEND_INTERVAL       100

/**
 * @brief  接收数据回调函数类型
 * @param  data  接收到的数据指针
 * @param  len   数据长度
 */
typedef void (*tcp_recv_callback_t)(const char *data, int len);

/* 函数声明 */
int tcp_client_start(int argc, char **argv);
int tcp_client_stop(void);
void tcp_set_recv_callback(tcp_recv_callback_t callback);
int tcp_send_data(const char *data, int len);
void tcp_set_translated_text(const char *text, int len);

#endif /* __TCP_CLIENT_H */
