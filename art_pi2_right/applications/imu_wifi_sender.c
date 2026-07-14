/**
 * @file imu_wifi_sender.c
 * @brief WiFi双向通信 - 发送IMU数据 + 接收命令 - 右手
 *
 * 发送数据格式（CSV）：
 * [DATA]timestamp_ms,hand_type,ch0-9(60个6轴值),ch10(9个9轴值)\n
 *
 * 接收命令格式：
 * CMD:命令 → 操作模式控制
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include <sys/time.h>
#include <sys/socket.h>
#include <netdb.h>

#include "imu_wifi_sender.h"
#include "operation_mode.h"
#include "net_io.h"
#include "net_manager.h"
#include "net_keepalive.h"
#include "net_session.h"
#include "model_queue.h"
#include "net_scheduler.h"
#include "../mpu6050/mpu6050_thread.h"

#define DBG_SECTION_NAME    "imu_wifi"
#define DBG_LEVEL           DBG_INFO
#include <rtdbg.h>

/* 线程控制 */
static rt_thread_t imu_wifi_thread = RT_NULL;
static volatile rt_bool_t imu_wifi_running = RT_FALSE;

/* 发送缓冲区 */
static char send_buf[IMU_WIFI_SEND_BUF_SIZE];

/* 接收缓冲区 */
#define RECV_BUF_SIZE 512
static char recv_buf[RECV_BUF_SIZE];

/* Dedicated line accumulator — MUST NOT share storage with recv_buf.
 * Issue #3 fix: previous code passed recv_buf as both accumulator and
 * chunk, so a partial line like "CMD:STA" followed by "RT\n" in the next
 * recv() overwrote the first chunk before it was processed. */
static char line_acc[NET_IO_MAX_LINE];
static int  rx_acc_len = 0;

/* 帧序号 */
static volatile uint32_t frame_seq = 0;

/* Command stream parse errors: number of times the ROCK accumulator had
 * to drop bytes because it overflowed, or a single line exceeded
 * NET_IO_MAX_LINE. Exposed via net_stat (see net_stats module). */
static rt_uint32_t imu_parse_err = 0;

/* Rock link state. Most of these are local to the worker thread but
 * rock_line_cb() runs from inside net_io_consume_lines() and may want
 * to send a PONG back — so we expose the current sock through a tiny
 * helper rather than threading the variable through every callback. */
static volatile int rock_current_sock = -1;

static int rock_sock_peek(void)
{
    return rock_current_sock;
}

/* Forward decl so we can hand the accumulator our line callback. */
static void rock_line_cb(const char *data, int len, void *user_data);
/* Forward decl: handle_rock_command is defined below the line callback,
 * but rock_line_cb() already calls it. Without this declaration clang
 * would issue an implicit-function-declaration warning and the static
 * definition would then clash with the implicit declaration. */
static void handle_rock_command(const char *data, int len);

/* Model/calibration payloads now live in model_queue (bounded FIFO,
 * 4 messages × NET_MODEL_MSG_MAX bytes). The legacy single-slot
 * model_buf + model_buf_len pair is gone — see model_queue.h. */

/**
 * @brief  获取并递增帧序号（线程安全）
 */
static uint32_t get_and_inc_frame_seq(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    uint32_t seq = frame_seq++;
    rt_hw_interrupt_enable(level);
    return seq;
}

/**
 * @brief  构建CSV格式的IMU数据
 * @param  buf    输出缓冲区
 * @param  size   缓冲区大小
 * @return 写入的字节数
 */
static int build_csv_imu_data(char *buf, int size)
{
    char *p = buf;
    int remain = size - 2;  /* 预留 \n 和 \0 */
    int n;
    mpu_channel_data_t d;

    /* 时间戳和帧序号 */
    uint32_t timestamp_ms = (uint32_t)(rt_tick_get() * 1000u / RT_TICK_PER_SECOND);
    uint32_t seq = get_and_inc_frame_seq();

    /* [DATA]timestamp,hand_type,frame_seq */
    n = rt_snprintf(p, remain, "[DATA]%u,right,%u", timestamp_ms, seq);
    p += n; remain -= n;

    /* ch0-ch9: MPU6050传感器 (6轴，每个6个值) */
    for (int i = 0; i < 10; i++)
    {
        rt_memset(&d, 0, sizeof(d));
        if (mpu_get_channel_raw_data(i, &d) == RT_EOK && d.valid)
        {
            n = rt_snprintf(p, remain, ",%d,%d,%d,%d,%d,%d",
                          d.ax, d.ay, d.az, d.gx, d.gy, d.gz);
        }
        else
        {
            n = rt_snprintf(p, remain, ",0,0,0,0,0,0");
        }
        p += n; remain -= n;
    }

    /* ch10: ICM-20948传感器 (9轴，9个值) */
    rt_memset(&d, 0, sizeof(d));
    if (mpu_get_channel_raw_data(10, &d) == RT_EOK && d.valid)
    {
        n = rt_snprintf(p, remain, ",%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
                      d.ax, d.ay, d.az, d.gx, d.gy, d.gz,
                      d.mx, d.my, d.mz);
    }
    else
    {
        n = rt_snprintf(p, remain, ",0,0,0,0,0,0,0,0,0\n");
    }
    p += n;

    return (int)(p - buf);
}

/**
 * @brief  Per-line callback invoked by net_io_consume_lines() once per
 *         LF-terminated command. data is NOT NUL-terminated; len is the
 *         number of bytes excluding the LF.
 *
 *         Side effect: trailing '\r' is stripped because ROCK sends
 *         CRLF-terminated lines from the Python side.
 */
static void rock_line_cb(const char *data, int len, void *user_data)
{
    int copy_len = len;

    (void)user_data;

    if (data == RT_NULL || len <= 0)
        return;

    /* Strip trailing CR (CRLF). */
    while (copy_len > 0 && (data[copy_len - 1] == '\r' ||
                            data[copy_len - 1] == ' ' ||
                            data[copy_len - 1] == '\t'))
    {
        copy_len--;
    }

    if (copy_len <= 0)
        return;

    /* Heartbeats short-circuit the dispatcher so PING/PONG never reach
     * operation_mode_handle_cmd(). The peeking helper gives us the
     * active ROCK socket so we can reply PONG without threading the
     * variable through the callback's user_data. */
    if (net_keepalive_handle_line(rock_sock_peek(), data, copy_len))
        return;

    handle_rock_command(data, copy_len);
}

/**
 * @brief  处理从Rock接收到的命令
 * @param  data  接收到的数据
 * @param  len   数据长度
 */
static void handle_rock_command(const char *data, int len)
{
    if (len <= 0 || data == RT_NULL)
        return;

    /* 复制到临时缓冲区并添加结束符 */
    char cmd_buf[RECV_BUF_SIZE];
    int copy_len = (len < RECV_BUF_SIZE - 1) ? len : (RECV_BUF_SIZE - 1);
    rt_memcpy(cmd_buf, data, copy_len);
    cmd_buf[copy_len] = '\0';

    /* 去除尾部的\r、\n和空格 */
    while (copy_len > 0 && (cmd_buf[copy_len - 1] == '\r' ||
                            cmd_buf[copy_len - 1] == '\n' ||
                            cmd_buf[copy_len - 1] == ' ' ||
                            cmd_buf[copy_len - 1] == '\t'))
    {
        cmd_buf[--copy_len] = '\0';
    }

    if (copy_len == 0)
        return;

    LOG_I("Rock command: %s", cmd_buf);

    /* 处理CMD:命令 */
    if (rt_strncmp(cmd_buf, "CMD:", 4) == 0)
    {
        LOG_I("Processing CMD: %s", cmd_buf);
        rt_bool_t result = operation_mode_handle_cmd(cmd_buf);
        if (result == RT_TRUE)
        {
            LOG_I("CMD processed successfully, new state: %d", operation_mode_get_state());
        }
        else
        {
            LOG_W("Unknown operation command: %s", cmd_buf);
        }
        return;
    }

    /* 处理MODE:命令 */
    if (rt_strncmp(cmd_buf, "MODE:", 5) == 0)
    {
        LOG_I("Processing MODE: %s", cmd_buf);
        rt_bool_t result = operation_mode_handle_cmd(cmd_buf);
        if (result == RT_TRUE)
        {
            LOG_I("MODE processed successfully, new state: %d", operation_mode_get_state());
        }
        else
        {
            LOG_W("Unknown MODE command: %s", cmd_buf);
        }
        return;
    }

    /* 处理SAY:文本命令（右手不支持语音，仅记录日志） */
    if (rt_strncmp(cmd_buf, "SAY:", 4) == 0 ||
        rt_strncmp(cmd_buf, "say:", 4) == 0)
    {
        /* 提取文本内容（跳过"SAY:"前缀） */
        const char *text = cmd_buf + 4;

        /* 跳过前导空格和分隔符 */
        while (*text == ' ' || *text == ':' || *text == '\t')
            text++;

        if (*text != '\0')
        {
            LOG_I("SAY command received (not supported on right hand): %s", text);
            /* 通知operation_mode收到翻译结果 */
            operation_mode_notify_say_received();
        }
        return;
    }

    LOG_W("Unknown command format: %s", cmd_buf);
}

/**
 * @brief  IMU WiFi双向通信线程入口
 */
static void imu_wifi_sender_thread_entry(void *parameter)
{
    int sock = -1;
    struct sockaddr_in server_addr;
    int ret;
    rt_tick_t last_send_tick = 0;

    LOG_I("IMU WiFi sender thread started");
    LOG_I("Target Rock server: %s:%d", ROCK_SERVER_IP, ROCK_SERVER_PORT);

    while (imu_wifi_running)
    {
        /* Same gating as tcp_client: don't burn CPU on connect() while
         * the link is dead. */
        if (net_manager_wait_ready(5000) != RT_EOK)
        {
            LOG_W("imu_wifi: net not ready, backoff");
            continue;
        }

        /* 创建socket */
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

        /*设置接收超时 10ms（非阻塞接收） */
        struct timeval recv_timeout;
        recv_timeout.tv_sec = 0;
        recv_timeout.tv_usec = 10000;  /* 10ms */
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));

        /* Enable SO_KEEPALIVE on the ROCK socket to detect half-open peers
         * (server crash without FIN/RST). No application-level PING/PONG here. */
        net_keepalive_enable(sock);

        /* 连接Rock服务器 */
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(ROCK_SERVER_PORT);
        server_addr.sin_addr.s_addr = inet_addr(ROCK_SERVER_IP);
        rt_memset(&(server_addr.sin_zero), 0, sizeof(server_addr.sin_zero));

        LOG_I("Connecting to Rock %s:%d ...", ROCK_SERVER_IP, ROCK_SERVER_PORT);
        ret = connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
        if (ret < 0)
        {
            static int consecutive_fail = 0;
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

        LOG_I("Connected to Rock!");
        last_send_tick = rt_tick_get();
        rock_current_sock = sock;

        /* Announce the new session so the ROCK consumer can flush its
         * per-session cache. The seq-reset hook zeros our frame_seq.
         * 'S' is the role identifier for the ROCK (server-side stream)
         * path; passing 'R' would be rejected by the new validator. */
        if (net_session_announce(sock, 'S') != RT_EOK)
        {
            LOG_W("imu_wifi: HELLO failed, tearing down socket");
            closesocket(sock);
            sock = -1;
            rock_current_sock = -1;
            rt_thread_mdelay(3000);
            continue;
        }

        /* No application-level heartbeat on the ROCK socket: the stream
         * is dense enough (11.1 Hz CSV), and neither the ROCK server
         * nor the left-hand board sends PONG, so a watchdog would
         * spuriously disconnect every ~6s. SO_KEEPALIVE still protects
         * against half-open peers. */

        /* Issue 6 fix: drop any partial MODEL state on reconnect so the new
         * socket does NOT resume mid-message. The peer already lost the
         * message prefix when the previous socket died; sending the tail
         * would corrupt the next framing. */
        model_queue_reset_partial_sends();

        /* Reset line accumulator so a half-received command from a
         * previous session can't fire as soon as the new socket goes up. */
        rx_acc_len = 0;

        /* 持续收发数据 */
        while (imu_wifi_running)
        {
            /* 等待到下一个发送时刻 (90ms间隔) */
            rt_tick_t now = rt_tick_get();
            rt_tick_t elapsed = now - last_send_tick;
            rt_tick_t interval_ticks = (IMU_WIFI_SEND_INTERVAL * RT_TICK_PER_SECOND) / 1000;

            if (elapsed < interval_ticks)
            {
                rt_thread_mdelay(interval_ticks - elapsed);
            }
            last_send_tick = rt_tick_get();

            /* 检查操作模式：只有在RUNNING状态才发送数据 */
            if (operation_mode_stream_enabled())
            {
                /* 构建CSV数据 */
                int len = build_csv_imu_data(send_buf, sizeof(send_buf));

                /* 90ms cadence; cap send at 70ms so the recv slot still fires. */
                rt_tick_t deadline = rt_tick_get() + (RT_TICK_PER_SECOND * 70 / 1000);
                int sent = net_io_send_all_deadline(sock, send_buf, len,
                                                    IMU_WIFI_SEND_DEADLINE_MS,
                                                    deadline);
                if (sent < len)
                {
                    if (sent < 0)
                        LOG_W("Send failed, reconnecting...");
                    else
                        LOG_W("Send short write %d/%d, reconnecting...", sent, len);
                    break;
                }
                LOG_D("Sent %d bytes to Rock", sent);
            }
            else
            {
                static rt_tick_t last_log_tick = 0;
                rt_tick_t now_tick = rt_tick_get();
                /* 每5秒打印一次等待状态 */
                if ((now_tick - last_log_tick) > (5000 * RT_TICK_PER_SECOND / 1000))
                {
                    LOG_I("Waiting for CMD:START... (current state: %d)", operation_mode_get_state());
                    last_log_tick = now_tick;
                }
            }

            /* Send a queued MODEL message (calibration thresholds).
             * Short writes are handled by model_queue_advance_head(), which
             * records the sent offset so the next cycle resumes from the
             * correct position instead of re-sending the already-ACK'd prefix. */
            const char *mq_data = RT_NULL;
            int         mq_len  = 0;
            if (model_queue_peek(&mq_data, &mq_len) == RT_EOK)
            {
                /* MODEL frames get 50ms deadline within the 90ms cycle. */
                rt_tick_t model_deadline = rt_tick_get() + (RT_TICK_PER_SECOND * 50 / 1000);
                int sent = net_io_send_all_deadline(sock, mq_data, mq_len,
                                                    IMU_WIFI_SEND_DEADLINE_MS,
                                                    model_deadline);
                if (sent >= mq_len)
                {
                    LOG_I("Sent MODEL data (%d bytes) to Rock", mq_len);
                    model_queue_pop();
                }
                else if (sent < 0)
                {
                    LOG_W("MODEL send failed, reconnecting...");
                    break;
                }
                else
                {
                    /* Partial send: record progress and retry next cycle. */
                    model_queue_advance_head(sent);
                    LOG_W("MODEL short write %d/%d, resuming next cycle", sent, mq_len);
                }
            }

            /* 尝试接收Rock发送的命令（非阻塞，10ms超时）。
 *
 * 历史遗留：原来采用“先 strchr('\n') 找到第一条就清空 buffer”的策略，
 * 导致一次 recv() 里 "CMD:START\nCMD:RESET_SEQ\n" 中的第二条被丢弃；
 * 而无换行时则用 strstr() 提前触发，可能把残包当完整命令处理。
 *
 * 现在的策略：所有收到的字节都进 net_io_consume_lines()，按 '\n'
 * 逐行提取；只有处理完的字节被消费，尾部残包留给下一轮 recv。
 */
            int rx_len = recv(sock, recv_buf, sizeof(recv_buf) - 1, 0);
            if (rx_len > 0)
            {
                LOG_D("Received %d bytes from Rock", rx_len);
                /* No net_keepalive_on_rx here — ROCK has no PING/PONG protocol. */
                net_io_consume_lines(line_acc, &rx_acc_len,
                                     (int)sizeof(line_acc),
                                     recv_buf, rx_len,
                                     rock_line_cb,
                                     RT_NULL,
                                     &imu_parse_err);
            }
            else if (rx_len < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            {
                /* 接收错误（非超时） */
                LOG_W("Recv error, reconnecting...");
                break;
            }

            /* Feed the actual elapsed time since the last send to the
             * shared-link congestion signal. Use elapsed send time rather
             * than tick execution time (fixes prior bug where silence_ms
             * was always ~0 because it measured tick() overhead). */
            {
                rt_tick_t now = rt_tick_get();
                rt_tick_t elapsed_ticks = now - last_send_tick;
                rt_uint32_t elapsed_ms = elapsed_ticks * 1000 / RT_TICK_PER_SECOND;
                net_scheduler_on_tick(elapsed_ms);
            }
        }

        /* No net_keepalive_stop() needed — we never called net_keepalive_start(). */
        rock_current_sock = -1;
        if (sock >= 0)
        {
            closesocket(sock);
            sock = -1;
        }
    }

    LOG_I("IMU WiFi sender thread stopped");
}

/**
 * @brief  启动IMU WiFi发送线程
 */
rt_err_t imu_wifi_sender_start(void)
{
    if (imu_wifi_running)
    {
        LOG_W("IMU WiFi sender already running");
        return -RT_ERROR;
    }

    /* Initialise the model FIFO before the worker launches. */
    model_queue_init();

    imu_wifi_running = RT_TRUE;

    imu_wifi_thread = rt_thread_create("imu_wifi",
                                       imu_wifi_sender_thread_entry,
                                       RT_NULL,
                                       IMU_WIFI_THREAD_STACK_SIZE,
                                       IMU_WIFI_THREAD_PRIORITY,
                                       IMU_WIFI_THREAD_TIMESLICE);

    if (imu_wifi_thread == RT_NULL)
    {
        LOG_E("Failed to create IMU WiFi sender thread");
        imu_wifi_running = RT_FALSE;
        return -RT_ERROR;
    }

    rt_thread_startup(imu_wifi_thread);
    LOG_I("IMU WiFi sender started");

    /* Plug the existing frame_seq reset path into net_session so the
     * HELLO line at the top of each new connection automatically
     * zeroes the frame counter — preventing the new session's seqs
     * from colliding with any cache the peer still holds. */
    net_session_register_seq_reset_rock(imu_wifi_sender_reset_frame_seq);

    return RT_EOK;
}

/**
 * @brief  停止IMU WiFi发送线程
 */
rt_err_t imu_wifi_sender_stop(void)
{
    if (!imu_wifi_running)
    {
        LOG_W("IMU WiFi sender not running");
        return -RT_ERROR;
    }

    imu_wifi_running = RT_FALSE;
    LOG_I("IMU WiFi sender stopping...");

    /* 等待线程退出 */
    rt_thread_mdelay(500);

    return RT_EOK;
}

/**
 * @brief  检查IMU WiFi发送线程是否运行
 */
rt_bool_t imu_wifi_sender_is_running(void)
{
    return imu_wifi_running;
}

/**
 * @brief  原子重置当前活跃 WiFi 流 frame_seq 为 0
 */
void imu_wifi_sender_reset_frame_seq(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    frame_seq = 0;
    rt_hw_interrupt_enable(level);
}

void imu_wifi_sender_send_model(const char *data, int len)
{
    /* Replace the legacy single-slot copy with a bounded FIFO push.
     * The worker thread drains it; on full queue the producer gets
     * -RT_EFULL and we record a dropped count instead of silently
     * overwriting still-unsent data. */
    if (data == RT_NULL || len <= 0)
        return;
    if (len >= NET_MODEL_MSG_MAX)
        return;  /* too big for the queue, refuse silently */

    rt_err_t r = model_queue_push(data, len);
    if (r == -RT_EFULL)
    {
        LOG_W("MODEL queue full, dropping %d-byte message", len);
    }
}


