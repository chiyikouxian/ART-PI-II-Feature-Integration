/**
 * @file tcp_client.c
 * @brief TCP客户端线程 - 发送传感器数据+电池电量到PC端
 * @note  通过MSH命令 tcp_start / tcp_stop 控制
 *        数据格式: JSON, 以\n结尾
 *
 *        通道映射 (左手 = 本机硬件):
 *        ch0~ch7:  I2C1 TCA9548A CH0~CH7 → MPU6050 (6-DOF)
 *        ch8~ch9:  I2C2 TCA9548A CH0~CH1 → MPU6050 (6-DOF)
 *        ch10:     I2C2 TCA9548A CH2     → MPU9250 (9-DOF)
 *
 *        若传感器未检测到 (valid=false), 该通道数据发送 0
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <sys/time.h>
#include <sys/socket.h>
#include <netdb.h>

#include "tcp_client.h"
#include "server_config.h"
#include "adc_battery.h"
#include "../mpu6050/mpu6050_thread.h"
#include "../vtx316/vtx316.h"

#define DBG_SECTION_NAME    "tcp_cli"
#define DBG_LEVEL           DBG_INFO
#include <rtdbg.h>

/* 线程运行标志 */
static rt_bool_t tcp_running = RT_FALSE;
static rt_thread_t tcp_thread = RT_NULL;

/* 服务器地址（从环境变量读取，可通过命令行参数临时覆盖） */
static char server_ip[16] = "";
static int  server_port = 0;

/* 发送缓冲区 - 使用静态分配避免栈溢出 */
static char send_buf[TCP_SEND_BUF_SIZE];

/* 接收缓冲区 */
static char recv_buf[TCP_RECV_BUF_SIZE];

/* 接收回调函数 */
static tcp_recv_callback_t recv_callback = RT_NULL;

/* 当前连接的socket (供 tcp_send_data 使用) */
static int current_sock = -1;
/* Cache the latest BLE-received translated text until it is uploaded once. */
static char translated_text_buf[257];
static volatile rt_bool_t translated_text_pending = RT_FALSE;
static volatile rt_uint32_t translated_text_seq = 0;
static rt_uint32_t translated_text_frame_seq = 0;

static void translated_text_clear_if_seq(rt_uint32_t seq)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (translated_text_pending && translated_text_seq == seq)
    {
        translated_text_buf[0] = '\0';
        translated_text_pending = RT_FALSE;
    }
    rt_hw_interrupt_enable(level);
}

static int append_json_escaped_string(char *p, int remain, const char *src)
{
    int total = 0;

    if (remain <= 0)
        return 0;

    if (remain > 1)
    {
        *p++ = '"';
        remain--;
        total++;
    }

    while (*src != '\0' && remain > 1)
    {
        unsigned char ch = (unsigned char)*src++;

        if (ch == '"' || ch == '\\')
        {
            if (remain <= 2)
                break;
            *p++ = '\\';
            *p++ = (char)ch;
            remain -= 2;
            total += 2;
        }
        else if (ch == '\r')
        {
            if (remain <= 2)
                break;
            *p++ = '\\';
            *p++ = 'r';
            remain -= 2;
            total += 2;
        }
        else if (ch == '\n')
        {
            if (remain <= 2)
                break;
            *p++ = '\\';
            *p++ = 'n';
            remain -= 2;
            total += 2;
        }
        else if (ch == '\t')
        {
            if (remain <= 2)
                break;
            *p++ = '\\';
            *p++ = 't';
            remain -= 2;
            total += 2;
        }
        else if (ch >= 0x20)
        {
            *p++ = (char)ch;
            remain--;
            total++;
        }
    }

    if (remain > 1)
    {
        *p++ = '"';
        total++;
    }

    return total;
}

/**
 * @brief  追加一个 6-DOF 通道 JSON (ch0~ch9)
 * @return 写入的字节数
 */
static int append_ch_6dof(char *p, int remain, int ch, const mpu_channel_data_t *d)
{
    if (d->valid)
    {
        return rt_snprintf(p, remain,
            "\"ch%d\":{\"ax\":%d,\"ay\":%d,\"az\":%d,\"gx\":%d,\"gy\":%d,\"gz\":%d}",
            ch, d->ax, d->ay, d->az, d->gx, d->gy, d->gz);
    }
    else
    {
        return rt_snprintf(p, remain,
            "\"ch%d\":{\"ax\":0,\"ay\":0,\"az\":0,\"gx\":0,\"gy\":0,\"gz\":0}",
            ch);
    }
}

/**
 * @brief  追加 ch10 的 9-DOF JSON
 * @return 写入的字节数
 */
static int append_ch_9dof(char *p, int remain, const mpu_channel_data_t *d)
{
    if (d->valid)
    {
        return rt_snprintf(p, remain,
            "\"ch10\":{\"ax\":%d,\"ay\":%d,\"az\":%d,"
            "\"gx\":%d,\"gy\":%d,\"gz\":%d,"
            "\"mx\":%d,\"my\":%d,\"mz\":%d}",
            d->ax, d->ay, d->az,
            d->gx, d->gy, d->gz,
            d->mx, d->my, d->mz);
    }
    else
    {
        return rt_snprintf(p, remain,
            "\"ch10\":{\"ax\":0,\"ay\":0,\"az\":0,"
            "\"gx\":0,\"gy\":0,\"gz\":0,"
            "\"mx\":0,\"my\":0,\"mz\":0}");
    }
}

/**
 * @brief  构建一只手的 JSON
 * @param  name      "left" 或 "right"
 * @param  real_data 是否读取真实传感器 (右手true, 左手false)
 * @return 写入的字节数
 */
static int build_hand_json(char *p, int remain, const char *name, rt_bool_t real_data)
{
    int total = 0;
    int n;
    mpu_channel_data_t d;

    /* "left":{ 或 "right":{ */
    n = rt_snprintf(p, remain, "\"%s\":{", name);
    total += n; p += n; remain -= n;

    /* ch0 ~ ch9: 6-DOF */
    for (int i = 0; i < 10; i++)
    {
        if (i > 0)
        {
            *p = ','; p++; total++; remain--;
        }

        rt_memset(&d, 0, sizeof(d));
        if (real_data)
        {
            mpu_get_channel_data(i, &d);
        }

        n = append_ch_6dof(p, remain, i, &d);
        total += n; p += n; remain -= n;
    }

    /* ch10: 9-DOF */
    *p = ','; p++; total++; remain--;

    rt_memset(&d, 0, sizeof(d));
    if (real_data)
    {
        mpu_get_channel_data(10, &d);
    }

    n = append_ch_9dof(p, remain, &d);
    total += n; p += n; remain -= n;

    /* 闭合 } */
    *p = '}'; p++; total++; remain--;

    return total;
}

/**
 * @brief  生成完整 JSON 数据包
 * @param  buf    输出缓冲区
 * @param  size   缓冲区大小
 * @param  count  数据包序号
 * @return 写入的字节数
 */
static int build_json_data(char *buf, int size, rt_uint32_t count)
{
    char *p = buf;
    int remain = size - 2;  /* 预留 \n 和 \0 */
    int total = 0;
    int n;

    char translated_text[sizeof(translated_text_buf)];
    rt_bool_t has_translated_text = RT_FALSE;

    rt_base_t level = rt_hw_interrupt_disable();
    if (translated_text_pending && translated_text_buf[0] != '\0')
    {
        rt_strncpy(translated_text, translated_text_buf, sizeof(translated_text) - 1);
        translated_text[sizeof(translated_text) - 1] = '\0';
        has_translated_text = RT_TRUE;
        translated_text_frame_seq = translated_text_seq;
    }
    else
    {
        translated_text_frame_seq = 0;
    }
    rt_hw_interrupt_enable(level);

    /* {"device":"left","id":N, */
    n = rt_snprintf(p, remain, "{\"device\":\"left\",\"id\":%u,", count);
    total += n; p += n; remain -= n;

    /* "left":{...} - 读取真实传感器 (本机为左手) */
    n = build_hand_json(p, remain, "left", RT_TRUE);
    total += n; p += n; remain -= n;

    /* "battery":{"voltage_mv":XXXX,"percentage":XX} */
    *p = ','; p++; total++; remain--;
    n = rt_snprintf(p, remain, "\"battery\":{\"voltage_mv\":%u,\"percentage\":%u}",
                    battery_get_voltage(), battery_get_percentage());
    total += n; p += n; remain -= n;

    if (has_translated_text)
    {
        *p = ','; p++; total++; remain--;
        n = rt_snprintf(p, remain, "\"translated_text\":");
        total += n; p += n; remain -= n;

        n = append_json_escaped_string(p, remain, translated_text);
        total += n; p += n; remain -= n;
    }

    /* }\n */
    *p++ = '}';
    *p++ = '\n';
    total += 2;

    return total;
}

/**
 * @brief  TCP客户端线程入口
 */
static void tcp_client_thread_entry(void *parameter)
{
    int sock = -1;
    struct sockaddr_in server_addr;
    rt_uint32_t count = 0;
    int ret;

    LOG_I("TCP client thread started");
    LOG_I("Target server: %s:%d", server_ip, server_port);

    while (tcp_running)
    {
        /* 创建 socket */
        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock < 0)
        {
            LOG_E("Create socket failed");
            rt_thread_mdelay(3000);
            continue;
        }

        /* 设置发送超时 3秒 */
        struct timeval timeout;
        timeout.tv_sec = 3;
        timeout.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        /* 连接服务器 */
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(server_port);
        server_addr.sin_addr.s_addr = inet_addr(server_ip);
        rt_memset(&(server_addr.sin_zero), 0, sizeof(server_addr.sin_zero));

        LOG_I("Connecting to %s:%d ...", server_ip, server_port);
        ret = connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
        if (ret < 0)
        {
            LOG_W("Connect failed, retry in 3s...");
            closesocket(sock);
            sock = -1;
            rt_thread_mdelay(3000);
            continue;
        }

        LOG_I("Connected!");
        current_sock = sock;

        /* 注册VTX316作为TCP接收回调: TCP收到 "SAY:文本" 将转发到语音模块 */
        tcp_set_recv_callback(vtx316_tcp_recv_handler);

        /* 设置接收超时为 TCP_SEND_INTERVAL (100ms) */
        {
            struct timeval recv_tv;
            recv_tv.tv_sec = 0;
            recv_tv.tv_usec = TCP_SEND_INTERVAL * 1000;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &recv_tv, sizeof(recv_tv));
        }

        /* 持续收发数据 */
        while (tcp_running)
        {
            /* 发送传感器数据 */
            count++;
            int len = build_json_data(send_buf, sizeof(send_buf), count);

            ret = send(sock, send_buf, len, 0);
            if (ret <= 0)
            {
                LOG_W("Send failed, reconnecting...");
                break;
            }

            if (ret == len && translated_text_frame_seq != 0)
            {
                translated_text_clear_if_seq(translated_text_frame_seq);
            }

            LOG_D("Sent[%u]: %d bytes", count, ret);

            /* 尝试接收服务器下发的数据 (超时100ms自动返回) */
            {
                int recv_len = recv(sock, recv_buf, sizeof(recv_buf) - 1, 0);
                if (recv_len > 0)
                {
                    recv_buf[recv_len] = '\0';
                    if (recv_callback != RT_NULL)
                    {
                        recv_callback(recv_buf, recv_len);
                    }
                    else
                    {
                        LOG_I("Recv[%d]: %s", recv_len, recv_buf);
                    }
                }
                else if (recv_len == 0)
                {
                    LOG_W("Server closed connection");
                    break;
                }
                /* recv_len < 0: 超时, 无数据, 继续下一轮发送 */
            }
        }

        /* 关闭连接 */
        current_sock = -1;
        if (sock >= 0)
        {
            closesocket(sock);
            sock = -1;
        }
    }

    LOG_I("TCP client thread stopped");
}

/**
 * @brief  启动TCP客户端
 * @note   MSH命令: tcp_start [server_ip] [port]
 *         示例: tcp_start 192.168.1.100 9109
 */
int tcp_client_start(int argc, char **argv)
{
    if (tcp_running)
    {
        rt_kprintf("TCP client is already running\n");
        return -1;
    }

    /* 从环境变量读取配置 */
    if (argc < 2)
    {
        server_config_get_tcp_ip(server_ip, sizeof(server_ip));
        server_port = server_config_get_tcp_port();
    }

    /* 解析可选参数（临时覆盖） */
    if (argc >= 2)
    {
        rt_strncpy(server_ip, argv[1], sizeof(server_ip) - 1);
        server_ip[sizeof(server_ip) - 1] = '\0';
    }
    if (argc >= 3)
    {
        server_port = atoi(argv[2]);
    }

    tcp_running = RT_TRUE;

    tcp_thread = rt_thread_create("tcp_cli",
                                   tcp_client_thread_entry,
                                   RT_NULL,
                                   TCP_THREAD_STACK_SIZE,
                                   TCP_THREAD_PRIORITY,
                                   TCP_THREAD_TIMESLICE);

    if (tcp_thread != RT_NULL)
    {
        rt_thread_startup(tcp_thread);
        rt_kprintf("TCP client started -> %s:%d\n", server_ip, server_port);
    }
    else
    {
        tcp_running = RT_FALSE;
        rt_kprintf("TCP client thread create failed!\n");
        return -1;
    }

    return 0;
}

/**
 * @brief  停止TCP客户端
 * @note   MSH命令: tcp_stop
 */
int tcp_client_stop(void)
{
    if (!tcp_running)
    {
        rt_kprintf("TCP client is not running\n");
        return -1;
    }

    tcp_running = RT_FALSE;
    rt_kprintf("TCP client stopping...\n");

    return 0;
}

/**
 * @brief  注册接收数据回调函数
 * @param  callback  回调函数指针, NULL 则恢复默认打印行为
 */
void tcp_set_recv_callback(tcp_recv_callback_t callback)
{
    recv_callback = callback;
}

/**
 * @brief  通过当前TCP连接发送自定义数据
 * @param  data  数据指针
 * @param  len   数据长度
 * @return 发送的字节数, 失败返回 -1
 */
int tcp_send_data(const char *data, int len)
{
    if (current_sock < 0)
    {
        LOG_W("TCP not connected");
        return -1;
    }

    return send(current_sock, data, len, 0);
}

void tcp_set_translated_text(const char *text, int len)
{
    int copy_len;
    const char *start;

    if (text == RT_NULL || len <= 0)
        return;

    start = text;

    if (len > 4 &&
        (start[0] == 'S' || start[0] == 's') &&
        (start[1] == 'A' || start[1] == 'a') &&
        (start[2] == 'Y' || start[2] == 'y') &&
        start[3] == ':')
    {
        start += 4;
        len -= 4;
    }

    while (len > 0 && (*start == ' ' || *start == '\t'))
    {
        start++;
        len--;
    }

    while (len > 0 && (start[len - 1] == '\r' || start[len - 1] == '\n'))
    {
        len--;
    }

    if (len <= 0)
        return;

    copy_len = len;
    if (copy_len > (int)sizeof(translated_text_buf) - 1)
        copy_len = sizeof(translated_text_buf) - 1;

    {
        rt_base_t level = rt_hw_interrupt_disable();
        rt_memcpy(translated_text_buf, start, copy_len);
        translated_text_buf[copy_len] = '\0';
        translated_text_seq++;
        if (translated_text_seq == 0)
            translated_text_seq = 1;
        translated_text_pending = RT_TRUE;
        rt_hw_interrupt_enable(level);
    }
}

/* 导出MSH命令 */
MSH_CMD_EXPORT(tcp_client_start, Start TCP client: tcp_client_start [ip] [port]);
MSH_CMD_EXPORT(tcp_client_stop, Stop TCP client);

/* 也导出简短别名 */
MSH_CMD_EXPORT_ALIAS(tcp_client_start, tcp_start, Start TCP client: tcp_start [ip] [port]);
MSH_CMD_EXPORT_ALIAS(tcp_client_stop, tcp_stop, Stop TCP client);
