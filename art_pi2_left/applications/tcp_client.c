/**
 * @file tcp_client.c
 * @brief TCP客户端线程 - 发送传感器数据+电池电量到PC端
 * @note  通过MSH命令 tcp_start / tcp_stop 控制
 *        数据格式: JSON, 以\n结尾
 *
 *        通道映射 (左手 = 本机硬件):
 *        ch0~ch7:  I2C1 TCA9548A CH0~CH7 → MPU6050 (6-DOF)
 *        ch8~ch9:  I2C2 TCA9548A CH0~CH1 → MPU6050 (6-DOF)
 *        ch10:     I2C2 TCA9548A CH2     → ICM-20948 (9-DOF, AK09916 磁力计)
 *
 *        若传感器未检测到 (valid=false), 该通道数据发送 0
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>

#include <sys/time.h>
#include <sys/socket.h>
#include <netdb.h>

#include "tcp_client.h"
#include "server_config.h"
#include "pc_discovery.h"
#include "adc_battery.h"
#include "net_io.h"
#include "../mpu6050/mpu6050_thread.h"
#include "../vtx316/vtx316.h"

#define DBG_SECTION_NAME    "tcp_cli"
#define DBG_LEVEL           DBG_INFO
#include <rtdbg.h>

/* Link statistics. Exposed via the `tcp_left_stat` MSH command so the
 * left-hand console can mirror what net_stat shows on the right hand. */
typedef struct {
    rt_uint32_t frames_sent;
    rt_uint32_t frames_attempted;
    rt_uint32_t short_writes;
} left_tcp_stats_t;
static left_tcp_stats_t g_left_stats;

/* Thread control */
static rt_bool_t tcp_running = RT_FALSE;
static rt_thread_t tcp_thread = RT_NULL;

/* Send buffer — static to avoid stack overflow */
static char send_buf[TCP_SEND_BUF_SIZE];

/* Receive buffer */
static char recv_buf[TCP_RECV_BUF_SIZE];

/* Receive callback */
static tcp_recv_callback_t recv_callback = RT_NULL;

/* Current active socket (used by tcp_send_data helper) */
static int current_sock = -1;

/* The generation captured when the current connection was established.
 * If this diverges from server_config_get_tcp_generation(), the
 * peer endpoint has changed and we must gracefully reconnect. */
static rt_uint32_t g_connected_gen = 0;
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
 *
 * Each outer loop iteration reads a fresh snapshot of the TCP endpoint
 * from server_config (which is protected by its own mutex). If the
 * generation has changed since the last connection, we tear down the
 * old socket and reconnect to the new address. This allows pc_discovery
 * to update the endpoint without any cross-thread races or direct socket
 * manipulation.
 */
static void tcp_client_thread_entry(void *parameter)
{
    int sock = -1;
    struct sockaddr_in server_addr;
    rt_uint32_t count = 0;

    /* Bootstrap: read the initial endpoint with its generation in ONE call. */
    char server_ip[16];
    int  server_port;
    rt_uint32_t gen;
    server_config_get_tcp_endpoint(server_ip, sizeof(server_ip), &server_port, &gen);
    g_connected_gen = gen;

    LOG_I("TCP client thread started");
    LOG_I("Target server: %s:%d (gen=%u)", server_ip, server_port, g_connected_gen);

    while (tcp_running)
    {
        /* Wait for Wi-Fi to be ready before wasting cycles on a dead link */
        if (!rt_wlan_is_ready())
        {
            LOG_W("TCP: waiting for Wi-Fi...");
            rt_thread_mdelay(1000);
            continue;
        }

        /* Grab fresh endpoint with its generation. If generation unchanged,
         * keep using the current address (no DNS re-lookup or stale flip). */
        server_config_get_tcp_endpoint(server_ip, sizeof(server_ip), &server_port, &gen);
        if (gen != g_connected_gen)
        {
            LOG_W("Endpoint changed (gen %u -> %u), reconnecting...",
                  g_connected_gen, gen);
            g_connected_gen = gen;
        }

        /* Create socket */
        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock < 0)
        {
            LOG_E("Create socket failed");
            rt_thread_mdelay(3000);
            continue;
        }

        /* Set send timeout 3s */
        {
            struct timeval timeout;
            timeout.tv_sec = 3;
            timeout.tv_usec = 0;
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        }

        /* Connect */
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(server_port);
        server_addr.sin_addr.s_addr = inet_addr(server_ip);
        rt_memset(&(server_addr.sin_zero), 0, sizeof(server_addr.sin_zero));

        LOG_I("Connecting to %s:%d ...", server_ip, server_port);
        int ret = connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
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
        g_connected_gen = gen;  /* use the snapshot gen captured above */

        /* Register VTX316 as TCP receive callback */
        tcp_set_recv_callback(vtx316_tcp_recv_handler);

        /* Set receive timeout = TCP_SEND_INTERVAL */
        {
            struct timeval recv_tv;
            recv_tv.tv_sec = 0;
            recv_tv.tv_usec = TCP_SEND_INTERVAL * 1000;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &recv_tv, sizeof(recv_tv));
        }

        /* Send HELLO banner */
        {
            char hello[64];
            int hl = rt_snprintf(hello, sizeof(hello),
                "HELLO,L,%08x,%08x\n",
                (unsigned int)rt_tick_get(), (unsigned int)rt_tick_get());
            net_io_send_all(sock, hello, hl, 5000);
        }

        /* Inner send/receive loop */
        while (tcp_running)
        {
            /* Check if endpoint changed (e.g. PC IP changed while connected).
             * Use atomic snapshot so IP and generation are always consistent. */
            rt_uint32_t gen;
            server_config_get_tcp_endpoint(server_ip, sizeof(server_ip), &server_port, &gen);
            if (gen != g_connected_gen)
            {
                LOG_W("Endpoint changed (gen %u -> %u), reconnecting...",
                      g_connected_gen, gen);
                g_connected_gen = gen;
                break;  /* exit inner loop, reconnect with new address */
            }

            /* Send sensor data */
            count++;
            g_left_stats.frames_attempted++;
            int len = build_json_data(send_buf, sizeof(send_buf), count);

            rt_tick_t deadline = rt_tick_get() + (RT_TICK_PER_SECOND * 80 / 1000);
            int sent = net_io_send_all_deadline(sock, send_buf, len,
                                                TCP_SEND_INTERVAL - 10,
                                                deadline);
            if (sent < len)
            {
                g_left_stats.short_writes++;
                if (sent < 0)
                    LOG_W("Send failed, reconnecting...");
                else
                    LOG_W("Send short write %d/%d, reconnecting...", sent, len);
                break;
            }

            g_left_stats.frames_sent++;

            if (translated_text_frame_seq != 0)
            {
                translated_text_clear_if_seq(translated_text_frame_seq);
            }

            LOG_D("Sent[%u]: %d bytes", count, sent);

            /* Try to receive server data (timeout returns automatically) */
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
                /* recv_len < 0: timeout, no data, continue */
            }
        }

        /* Close connection and loop to reconnect */
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
 * @brief  启动TCP客户端（始终从 server_config 读取端点，支持动态切换）
 * @note   MSH命令: tcp_start
 *         若通过 pc_discovery 发现了 PC，将自动使用发现的地址。
 */
int tcp_client_start(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (tcp_running)
    {
        rt_kprintf("TCP client is already running\n");
        return -1;
    }

    /* Endpoint is always read from server_config at connect time.
     * server_config_update_tcp_endpoint() may have already been called
     * by pc_discovery before we reach here. */

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
        {
            char ip[16];
            int port;
            server_config_get_tcp_endpoint(ip, sizeof(ip), &port, RT_NULL);
            rt_kprintf("TCP client started -> %s:%d\n", ip, port);
        }
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

/**
 * @brief  Print left-hand TCP link statistics (issue 8: parity with the
 *         right-hand net_stat). Mirrors tcp_link_stats_t from the right
 *         project.
 */
static void cmd_tcp_left_stat(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    rt_kprintf("\n=== Left-hand TCP Link Statistics ===\n");
    rt_kprintf(" frames_attempted: %u\n", g_left_stats.frames_attempted);
    rt_kprintf(" frames_sent:      %u\n", g_left_stats.frames_sent);
    rt_kprintf(" short_writes:     %u\n", g_left_stats.short_writes);
    rt_kprintf("====================================\n");
}
MSH_CMD_EXPORT(cmd_tcp_left_stat, Show left-hand TCP link stats);
