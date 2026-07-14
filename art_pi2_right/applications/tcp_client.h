/**
 * @file tcp_client.h
 * @brief TCP客户端线程 - 双向通信: 发送传感器数据到PC / 接收PC下发数据
 */

#ifndef __TCP_CLIENT_H
#define __TCP_CLIENT_H

#include <rtthread.h>

/* 服务器配置 - 修改为你电脑的IP地址 */
#define TCP_SERVER_IP       "192.168.157.217"
#define TCP_SERVER_PORT     9109

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

/* 单次 send() 的最大允许耗时：90% 的周期，防止单次写阻塞下一帧。 */
#define TCP_SEND_DEADLINE_MS    90

/**
 * @brief  接收数据回调函数类型
 * @param  data  接收到的数据指针
 * @param  len   数据长度
 */
typedef void (*tcp_recv_callback_t)(const char *data, int len);

/**
 * @brief  Line-oriented callback used after the ring accumulator parses a
 *         complete LF-terminated command out of the stream. data is NOT
 *         NUL-terminated; len is the number of bytes excluding the LF.
 *
 *         Optional. If left NULL, only the legacy recv_callback fires.
 */
typedef void (*tcp_line_callback_t)(const char *data, int len, void *user_data);

/**
 * @brief  Statistics snapshot for the PC TCP link. Exposed via net_stat.
 *
 * frames_attempted counts every loop iteration (skipped + failed + OK).
 * frames_sent only increments after a successful complete write, so
 * (frames_sent / frames_attempted) is the real delivery ratio.
 */
typedef struct {
    rt_uint32_t frames_sent;        /* fully written frames         */
    rt_uint32_t frames_attempted;   /* every iteration (incl skip) */
    rt_uint32_t frames_skipped;     /* dropped by scheduler         */
    rt_uint32_t short_writes;       /* partial send that broke conn */
    rt_uint32_t rx_parse_errors;    /* malformed/overflowed lines   */
} tcp_link_stats_t;

/**
 * @brief  Read-only snapshot of PC TCP link counters.
 */
void tcp_get_link_stats(tcp_link_stats_t *out);

/* 函数声明 */
int tcp_client_start(int argc, char **argv);
int tcp_client_stop(void);
void tcp_set_recv_callback(tcp_recv_callback_t callback);
void tcp_set_line_callback(tcp_line_callback_t callback, void *user_data);
int tcp_send_data(const char *data, int len);

#endif /* __TCP_CLIENT_H */
