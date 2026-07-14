/**
 * @file tcp_client.c
 * @brief Right-hand TCP client: upload IMU and battery data to PC.
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
#include "pc_discovery.h"
#include "adc_battery.h"
#include "net_io.h"
#include "net_manager.h"
#include "net_keepalive.h"
#include "net_session.h"
#include "net_scheduler.h"
#include "../mpu6050/mpu6050_thread.h"

#define DBG_SECTION_NAME    "tcp_cli"
#define DBG_LEVEL           DBG_INFO
#include <rtdbg.h>

static rt_bool_t tcp_running = RT_FALSE;
static rt_thread_t tcp_thread = RT_NULL;

static char send_buf[TCP_SEND_BUF_SIZE];
static char recv_buf[TCP_RECV_BUF_SIZE];

/* Dedicated line accumulator. Issue 5 fix: recv_buf is overwritten by
 * every recv() call, so it cannot also serve as the partial-line buffer.
 * The accumulator persists across recv() calls and only consumes bytes
 * after a complete '\n' has been parsed. */
static char line_acc[NET_IO_MAX_LINE];
static int  rx_acc_len = 0;

static tcp_recv_callback_t recv_callback = RT_NULL;
static tcp_line_callback_t line_callback  = RT_NULL;
static void              *line_callback_ud = RT_NULL;
static int current_sock = -1;

/* Generation captured when the current connection was established.
 * If this diverges from server_config_get_tcp_generation(), the PC
 * endpoint has changed and we must gracefully reconnect. */
static rt_uint32_t g_connected_gen = 0;

/* Link statistics for net_stat. */
static tcp_link_stats_t g_link_stats;

/* Fallback counter exposed to net_stat when net_io can't flag a parse
 * error precisely. Kept separate from rx_acc_len so the renderer doesn't
 * have to re-walk the accumulator. */
static rt_uint32_t tcp_parse_err = 0;

static void line_callback_silent_cb(const char *data, int len, void *user_data)
{
    /* Forward each parsed line to the registered line callback. The
     * accumulator only invokes us when a full LF-terminated line is
     * available, so partial commands never escape here. */
    if (line_callback != RT_NULL)
    {
        if (!net_keepalive_handle_line(current_sock, data, len))
        {
            line_callback(data, len, line_callback_ud);
        }
    }
    (void)user_data;
}

static int append_ch_6dof(char *p, int remain, int ch, const mpu_channel_data_t *d)
{
    if (d->valid)
    {
        return rt_snprintf(p, remain,
            "\"ch%d\":{\"ax\":%d,\"ay\":%d,\"az\":%d,\"gx\":%d,\"gy\":%d,\"gz\":%d}",
            ch, d->ax, d->ay, d->az, d->gx, d->gy, d->gz);
    }

    return rt_snprintf(p, remain,
        "\"ch%d\":{\"ax\":0,\"ay\":0,\"az\":0,\"gx\":0,\"gy\":0,\"gz\":0}",
        ch);
}

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

    return rt_snprintf(p, remain,
        "\"ch10\":{\"ax\":0,\"ay\":0,\"az\":0,"
        "\"gx\":0,\"gy\":0,\"gz\":0,"
        "\"mx\":0,\"my\":0,\"mz\":0}");
}

static int build_hand_json(char *p, int remain, const char *name, rt_bool_t real_data)
{
    int total = 0;
    int n;
    mpu_channel_data_t d;

    n = rt_snprintf(p, remain, "\"%s\":{", name);
    total += n; p += n; remain -= n;

    for (int i = 0; i < 10; i++)
    {
        if (i > 0)
        {
            *p = ',';
            p++;
            total++;
            remain--;
        }

        rt_memset(&d, 0, sizeof(d));
        if (real_data)
        {
            mpu_get_channel_data(i, &d);
        }

        n = append_ch_6dof(p, remain, i, &d);
        total += n; p += n; remain -= n;
    }

    *p = ',';
    p++;
    total++;
    remain--;

    rt_memset(&d, 0, sizeof(d));
    if (real_data)
    {
        mpu_get_channel_data(10, &d);
    }

    n = append_ch_9dof(p, remain, &d);
    total += n; p += n; remain -= n;

    *p = '}';
    p++;
    total++;
    remain--;

    return total;
}

static int build_json_data(char *buf, int size, rt_uint32_t count)
{
    char *p = buf;
    int remain = size - 2;
    int total = 0;
    int n;

    n = rt_snprintf(p, remain, "{\"device\":\"right\",\"id\":%u,", count);
    total += n; p += n; remain -= n;

    n = build_hand_json(p, remain, "right", RT_TRUE);
    total += n; p += n; remain -= n;

    *p = ',';
    p++;
    total++;
    remain--;

    n = rt_snprintf(p, remain, "\"battery\":{\"voltage_mv\":%u,\"percentage\":%u}",
                    battery_get_voltage(), battery_get_percentage());
    total += n; p += n; remain -= n;

    *p++ = '}';
    *p++ = '\n';
    total += 2;

    return total;
}

static void tcp_client_thread_entry(void *parameter)
{
    int sock = -1;
    struct sockaddr_in server_addr;
    rt_uint32_t count = 0;
    int ret;
    rt_uint32_t skip_cnt = 0;
    static int consecutive_fail = 0;

    /* Bootstrap: read endpoint with generation in ONE call. */
    char server_ip[16];
    int  server_port;
    rt_uint32_t gen;
    server_config_get_tcp_endpoint(server_ip, sizeof(server_ip), &server_port, &gen);
    g_connected_gen = gen;

    LOG_I("TCP client thread started");
    LOG_I("Target server: %s:%d (gen=%u)", server_ip, server_port, g_connected_gen);

    while (tcp_running)
    {
        /* Wait for the WiFi supervisor before we waste cycles opening
         * sockets against a dead link. */
        if (net_manager_wait_ready(5000) != RT_EOK)
        {
            LOG_W("TCP client: net not ready, backoff");
            continue;
        }

        /* Grab fresh endpoint with its generation. If unchanged, keep address. */
        server_config_get_tcp_endpoint(server_ip, sizeof(server_ip), &server_port, &gen);
        if (gen != g_connected_gen)
        {
            LOG_W("Endpoint changed (gen %u -> %u), reconnecting...",
                  g_connected_gen, gen);
            g_connected_gen = gen;
        }

        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock < 0)
        {
            LOG_E("Create socket failed");
            rt_thread_mdelay(3000);
            continue;
        }

        {
            struct timeval timeout;
            timeout.tv_sec = 3;
            timeout.tv_usec = 0;
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        }

        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(server_port);
        server_addr.sin_addr.s_addr = inet_addr(server_ip);
        rt_memset(&(server_addr.sin_zero), 0, sizeof(server_addr.sin_zero));

        LOG_I("Connecting to %s:%d ...", server_ip, server_port);
        ret = connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
        if (ret < 0)
        {
            LOG_W("Connect failed, retry in 3s...");
            consecutive_fail++;
            if (consecutive_fail >= 3)
            {
                consecutive_fail = 0;
                net_manager_notify_disconnected();
            }
            closesocket(sock);
            sock = -1;
            rt_thread_mdelay(3000);
            continue;
        }

        LOG_I("Connected!");
        current_sock = sock;
        g_connected_gen = gen;  /* use the snapshot gen captured above */
        consecutive_fail = 0;

        /* Reset the line accumulator so a half-received command from a
         * previous session can't fire as soon as the new socket goes up. */
        rx_acc_len = 0;

        /* Announce the new session before any data frame so the PC
         * frontend can flush its cache. Use role 'C' (client) so
         * net_session does NOT call the ROCK-specific seq_reset. */
        net_session_announce(sock, 'C');

        /* Start the application-level heartbeat + enable SO_KEEPALIVE.
         * The peer (PC frontend) is expected to echo PONG. */
        net_keepalive_start(sock, 'C');

        {
            struct timeval recv_tv;
            recv_tv.tv_sec = 0;
            recv_tv.tv_usec = TCP_SEND_INTERVAL * 1000;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &recv_tv, sizeof(recv_tv));
        }

        rt_tick_t cycle_start = rt_tick_get();

        while (tcp_running)
        {
            cycle_start = rt_tick_get();

            /* Check if PC endpoint changed — use atomic snapshot. */
            {
                server_config_get_tcp_endpoint(server_ip, sizeof(server_ip),
                                              &server_port, &gen);
                if (gen != g_connected_gen)
                {
                    LOG_W("Endpoint changed (gen %u -> %u), reconnecting...",
                          g_connected_gen, gen);
                    g_connected_gen = gen;
                    break;
                }
            }

            /* Step 1: scheduler may tell us to drop this frame. */
            rt_bool_t skip_frame = net_scheduler_should_skip(NET_PRIO_NORMAL);
            int       sent       = 0;
            int       len        = 0;

            if (!skip_frame)
            {
                g_link_stats.frames_attempted++;
                len = build_json_data(send_buf, sizeof(send_buf), count);

                rt_tick_t deadline = cycle_start + (RT_TICK_PER_SECOND * 80 / 1000);
                sent = net_io_send_all_deadline(sock, send_buf, len,
                                                TCP_SEND_DEADLINE_MS,
                                                deadline);
                if (sent < len)
                {
                    g_link_stats.short_writes++;
                    if (sent < 0)
                        LOG_W("Send failed, reconnecting...");
                    else
                        LOG_W("Send short write %d/%d, reconnecting...", sent, len);
                    break;
                }
                g_link_stats.frames_sent++;
                LOG_D("Sent[%u]: %d bytes", count, sent);
            }
            else
            {
                skip_cnt++;
                g_link_stats.frames_skipped++;
                if ((skip_cnt & 0x1F) == 1)
                    LOG_I("TCP client: congested, skipping frame (count=%u)", skip_cnt);
            }

            /* Step 2: drain whatever the peer sent. */
            {
                int recv_len = recv(sock, recv_buf, sizeof(recv_buf) - 1, 0);
                if (recv_len > 0)
                {
                    recv_buf[recv_len] = '\0';
                    net_keepalive_on_rx(sock);

                    if (line_callback != RT_NULL)
                    {
                        rt_uint32_t err_before = tcp_parse_err;
                        net_io_consume_lines(line_acc, &rx_acc_len,
                                             (int)sizeof(line_acc),
                                             recv_buf, recv_len,
                                             line_callback_silent_cb,
                                             RT_NULL,
                                             &tcp_parse_err);
                        if (tcp_parse_err > err_before)
                            g_link_stats.rx_parse_errors +=
                                (tcp_parse_err - err_before);
                    }

                    if (recv_callback != RT_NULL)
                    {
                        recv_callback(recv_buf, recv_len);
                    }
                    else if (line_callback == RT_NULL)
                    {
                        LOG_I("Recv[%d]: %s", recv_len, recv_buf);
                    }
                }
                else if (recv_len == 0)
                {
                    LOG_W("Server closed connection");
                    break;
                }
            }

            /* Step 3: feed cycle duration to the congestion detector. */
            {
                rt_tick_t now_tick  = rt_tick_get();
                rt_tick_t elapsed_ticks = now_tick - cycle_start;
                rt_uint32_t elapsed_ms = elapsed_ticks * 1000 / RT_TICK_PER_SECOND;
                net_scheduler_on_tick(elapsed_ms);
            }

            /* Step 4: application-level PING/PONG watchdog. */
            rt_err_t ka = net_keepalive_tick(sock);
            if (ka != RT_EOK)
            {
                LOG_W("TCP client: keepalive watchdog tripped, reconnecting");
                break;
            }

            /* Step 5: cadence sleep. */
            rt_tick_t cycle_used = rt_tick_get() - cycle_start;
            rt_tick_t cycle_budget = (rt_tick_t)(RT_TICK_PER_SECOND * TCP_SEND_INTERVAL / 1000);
            if (cycle_used < cycle_budget)
            {
                rt_thread_mdelay((cycle_budget - cycle_used) * 1000 / RT_TICK_PER_SECOND);
            }

            if (!skip_frame)
                count++;
        }

        current_sock = -1;
        net_keepalive_stop(sock);
        if (sock >= 0)
        {
            closesocket(sock);
            sock = -1;
        }
    }

    LOG_I("TCP client thread stopped");
}

int tcp_client_start(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (tcp_running)
    {
        rt_kprintf("TCP client is already running\n");
        return -1;
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
        rt_kprintf("TCP client started\n");
    }
    else
    {
        tcp_running = RT_FALSE;
        rt_kprintf("TCP client thread create failed!\n");
        return -1;
    }

    return 0;
}

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

void tcp_set_recv_callback(tcp_recv_callback_t callback)
{
    recv_callback = callback;
}

void tcp_set_line_callback(tcp_line_callback_t callback, void *user_data)
{
    line_callback    = callback;
    line_callback_ud = user_data;
    rx_acc_len       = 0;  /* reset accumulator so old tail can't leak */
}

int tcp_send_data(const char *data, int len)
{
    if (current_sock < 0)
    {
        LOG_W("TCP not connected");
        return -1;
    }

    /* Treat the helper's contract as authoritative: < 0 on error,
     * < len means short write — both surface as -1 to the caller. */
    int n = net_io_send_all(current_sock, data, len, TCP_SEND_DEADLINE_MS);
    return (n == len) ? n : -1;
}

void tcp_get_link_stats(tcp_link_stats_t *out)
{
    if (out == RT_NULL) return;
    rt_base_t level = rt_hw_interrupt_disable();
    *out = g_link_stats;
    rt_hw_interrupt_enable(level);
}

MSH_CMD_EXPORT(tcp_client_start, Start TCP client: tcp_client_start [ip] [port]);
MSH_CMD_EXPORT(tcp_client_stop, Stop TCP client);

MSH_CMD_EXPORT_ALIAS(tcp_client_start, tcp_start, Start TCP client: tcp_start [ip] [port]);
MSH_CMD_EXPORT_ALIAS(tcp_client_stop, tcp_stop, Stop TCP client);
