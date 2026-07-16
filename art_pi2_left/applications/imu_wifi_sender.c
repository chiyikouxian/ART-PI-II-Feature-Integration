/**
 * @file imu_wifi_sender.c
 * @brief WiFi双向通信 - 发送IMU数据 + 接收命令 - 左手
 *
 * 发送数据格式（CSV）：
 * [DATA]timestamp_ms,hand_type,ch0-9(60个6轴值),ch10(9个9轴值)\n
 *
 * 接收命令格式：
 * SAY:文本 → VTX316语音播放
 * CMD:命令 → 操作模式控制
 *
 * 稳定性修复记录 (issues 1-8):
 *   - 使用 net_io_send_all_deadline() 防止短写
 *   - 使用独立的 line_acc 缓冲区而非 recv_buf 累积
 *   - MODEL 数据使用 model_queue FIFO + advance_head 记录偏移
 *   - 移除 strstr 解析，改用 net_io_consume_lines 按 '\n' 逐行处理
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
#include "model_queue.h"
#include "../mpu6050/mpu6050_thread.h"
#include "../vtx316/vtx316.h"
#include "tcp_client.h"

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

/* 独立的行累积缓冲区 — 解决 Issue 3: 之前 recv_buf 既用作累积器又
 * 用作原始数据接收区，下一次 recv() 会覆盖未处理的尾部数据。 */
static char line_acc[NET_IO_MAX_LINE];
static int  rx_acc_len = 0;

/* 命令解析错误计数器 */
static rt_uint32_t imu_parse_err = 0;

/* 帧序号 */
static volatile uint32_t frame_seq = 0;

/* ── ROCK 推理模式状态 ────────────────────────────────────────────────────
 * g_infer_mode              : 当前期望的 ROCK 推理模式。Boot 默认
 *                             INFER_MODE_SENTENCE；PE0 每按一次翻转一次。
 * g_mode_generation         : 用户翻转次数；用来标记"是否需要重新发送"。
 * g_mode_generation_sent    : 上一次实际写到 socket 的 generation。
 *                             boot 默认 0，确保首次连接时若用户从未按键，
 *                             generation == generation_sent 不会自动发送。
 * g_mode_has_user_selection : 用户是否至少按过一次 PE0。boot 默认 RT_FALSE。
 *                             仅在用户主动按过键后，TCP 重连才需要重发当前
 *                             模式（否则 ROCK 默认 sentence 与本端默认一致，
 *                             发送是浪费）。
 * 与 IMU WiFi 线程的交互：imu_wifi_sender_request_inference_mode_switch()
 * （按键线程可调用）会原子地翻转 mode+generation，并置 user_selection=TRUE。
 * imu_wifi_sender 线程在每次发 IMU 数据前检查 generation_sent != generation，
 * 若是则推送一行 mode 字面量（"word\n" 或 "sentence\n"），然后才发 IMU 帧。
 */
static volatile inference_mode_t g_infer_mode              = INFER_MODE_SENTENCE;
static volatile uint32_t          g_mode_generation         = 0;
static volatile uint32_t          g_mode_generation_sent    = 0;
static volatile rt_bool_t         g_mode_has_user_selection = RT_FALSE;

static const char *infer_mode_str(inference_mode_t m)
{
    return (m == INFER_MODE_SENTENCE) ? "sentence" : "word";
}

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
    int remain = size - 2;
    int n;
    mpu_channel_data_t d;

    uint32_t timestamp_ms = (uint32_t)(rt_tick_get() * 1000u / RT_TICK_PER_SECOND);
    uint32_t seq = get_and_inc_frame_seq();

    n = rt_snprintf(p, remain, "[DATA]%u,left,%u", timestamp_ms, seq);
    p += n; remain -= n;

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
 * @brief  处理从Rock接收到的完整命令（按 '\n' 分割后调用）
 * @param  data  不含 '\n' 的命令字符串
 * @param  len   长度（不含 '\n'）
 */
static void handle_rock_command(const char *data, int len)
{
    if (data == RT_NULL || len <= 0)
        return;

    char cmd_buf[NET_IO_MAX_LINE];
    int copy_len = (len < (int)sizeof(cmd_buf) - 1) ? len : ((int)sizeof(cmd_buf) - 1);
    rt_memcpy(cmd_buf, data, copy_len);
    cmd_buf[copy_len] = '\0';

    while (copy_len > 0 &&
           (cmd_buf[copy_len - 1] == '\r' || cmd_buf[copy_len - 1] == '\n' ||
            cmd_buf[copy_len - 1] == ' ' || cmd_buf[copy_len - 1] == '\t'))
    {
        cmd_buf[--copy_len] = '\0';
    }

    if (copy_len == 0)
        return;

    LOG_I("Rock command: %s", cmd_buf);

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

    if (rt_strncmp(cmd_buf, "SAY:", 4) == 0 ||
        rt_strncmp(cmd_buf, "say:", 4) == 0)
    {
        const char *text = cmd_buf + 4;
        while (*text == ' ' || *text == ':' || *text == '\t')
            text++;

        if (*text != '\0')
        {
            LOG_I("VTX316 speak: %s", text);
            vtx316_speak_wait(text);
            tcp_set_translated_text(text, rt_strlen(text));
            operation_mode_notify_say_received();
        }
        return;
    }

    LOG_W("Unknown command format: %s", cmd_buf);
}

/**
 * @brief  net_io_consume_lines 回调：每有一条 '\n' 结尾的完整命令时调用
 */
static void rock_line_cb(const char *data, int len, void *user_data)
{
    (void)user_data;
    handle_rock_command(data, len);
}

/**
 * @brief  IMU WiFi双向通信线程入口
 */
static void imu_wifi_sender_thread_entry(void *parameter)
{
    int sock = -1;
    struct sockaddr_in server_addr;
    rt_err_t ret;
    rt_tick_t last_send_tick = 0;

    LOG_I("IMU WiFi sender thread started");
    LOG_I("Target Rock server: %s:%d", ROCK_SERVER_IP, ROCK_SERVER_PORT);

    /* 初始化 MODEL 队列 */
    model_queue_init();

    while (imu_wifi_running)
    {
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

        {
            struct timeval recv_timeout;
            recv_timeout.tv_sec = 0;
            recv_timeout.tv_usec = 10000;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));
        }

        /* Issue 7 fix: enable kernel-level SO_KEEPALIVE on the ROCK
         * socket. Without this, a half-open peer (e.g. ROCK rebooted
         * without closing the TCP connection) would only be detected on
         * the next send() failure. SO_KEEPALIVE adds idle + interval
         * probes so the stack drops the dead socket within a few tens
         * of seconds even if we have nothing to send. We keep the
         * defaults (tcp_keepalive_time / tcp_keepalive_intvl) since the
         * default values are reasonable for an 11Hz IMU stream. */
        {
            int keepalive = 1;
            setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive,
                       sizeof(keepalive));
        }

        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(ROCK_SERVER_PORT);
        server_addr.sin_addr.s_addr = inet_addr(ROCK_SERVER_IP);
        rt_memset(&(server_addr.sin_zero), 0, sizeof(server_addr.sin_zero));

        LOG_I("Connecting to Rock %s:%d ...", ROCK_SERVER_IP, ROCK_SERVER_PORT);
        ret = connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
        if (ret < 0)
        {
            LOG_W("Connect failed, retry in 3s...");
            closesocket(sock);
            sock = -1;
            rt_thread_mdelay(3000);
            continue;
        }

        LOG_I("Connected to Rock!");
        last_send_tick = rt_tick_get();

        /* Issue 6: drop any partial MODEL state on reconnect so the new
         * socket does NOT resume mid-message. */
        model_queue_reset_partial_sends();

        /* Reset line accumulator so a half-received command from a
         * previous session can't fire as soon as the new socket goes up. */
        rx_acc_len = 0;

        /* On a fresh ROCK connection, decide whether the inference mode
         * must be re-published:
         *   - If the user has never pressed PE0, both ART-Pi and ROCK boot
         *     in sentence mode. No mode command is needed on this socket;
         *     we align g_mode_generation_sent = cur_gen so the per-frame
         *     check below treats the connection as already-up-to-date.
         *   - If the user has flipped the mode at least once, ROCK may have
         *     been rebooted and lost that state. Force a re-publish on
         *     this socket by setting g_mode_generation_sent = cur_gen - 1. */
        {
            uint32_t cur_gen;
            inference_mode_t mode;
            rt_bool_t        has_user_selection;
            rt_base_t level = rt_hw_interrupt_disable();
            cur_gen            = g_mode_generation;
            mode               = g_infer_mode;
            has_user_selection = g_mode_has_user_selection;
            if (has_user_selection)
            {
                g_mode_generation_sent = (uint32_t)(cur_gen - 1u);
            }
            else
            {
                g_mode_generation_sent = cur_gen;
            }
            rt_hw_interrupt_enable(level);

            if (has_user_selection)
            {
                LOG_I("Inference mode will be re-sent on this socket: %s (gen=%u)",
                      infer_mode_str(mode), (unsigned)cur_gen);
            }
            else
            {
                LOG_I("Inference mode defaults to sentence; no initial command required");
            }
        }

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

            /* Publish the current inference mode BEFORE the first IMU
             * frame on this socket (and on every later mode switch).
             * This is independent of operation_mode_stream_enabled() —
             * even if we are in AUTO_STANDBY / MANUAL_SLEEP / WAITING_STOP
             * the mode line must still go out so ROCK knows which
             * sliding-window family is active.
             *
             * Wire format: just "word\n" or "sentence\n" — no protocol
             * prefix. ROCK splits on '\n' and dispatches the literal to
             * the inference engine. */
            {
                uint32_t cur_gen, sent_gen;
                inference_mode_t mode;
                rt_base_t level = rt_hw_interrupt_disable();
                cur_gen  = g_mode_generation;
                sent_gen = g_mode_generation_sent;
                mode     = g_infer_mode;
                rt_hw_interrupt_enable(level);

                if (cur_gen != sent_gen)
                {
                    char mode_line[16];
                    int  mode_len = rt_snprintf(mode_line,
                                                sizeof(mode_line),
                                                "%s\n",
                                                infer_mode_str(mode));
                    rt_tick_t mode_deadline = rt_tick_get() +
                                              (RT_TICK_PER_SECOND * 50 / 1000);
                    int mode_sent = net_io_send_all_deadline(
                        sock, mode_line, mode_len,
                        IMU_WIFI_SEND_DEADLINE_MS, mode_deadline);
                    if (mode_sent >= mode_len)
                    {
                        g_mode_generation_sent = cur_gen;
                        LOG_I("Inference mode sent to Rock: %s (gen=%u)",
                              infer_mode_str(mode),
                              (unsigned)cur_gen);
                    }
                    else
                    {
                        LOG_W("Inference mode send failed: mode=%s sent=%d/%d",
                              infer_mode_str(mode), mode_sent, mode_len);
                        break;
                    }
                }
            }

            /* 检查操作模式：只有在RUNNING状态才发送数据 */
            if (operation_mode_stream_enabled())
            {
                int len = build_csv_imu_data(send_buf, sizeof(send_buf));

                /* 使用 70ms 截止时间，留下 20ms 给 recv 和模型发送 */
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
                if ((now_tick - last_log_tick) > (5000 * RT_TICK_PER_SECOND / 1000))
                {
                    LOG_I("Waiting for CMD:START... (current state: %d)", operation_mode_get_state());
                    last_log_tick = now_tick;
                }
            }

            /* 发送待发送的模型数据（校准阈值）— 使用 FIFO + advance_head 修复短写问题 */
            {
                const char *mq_data = RT_NULL;
                int         mq_len  = 0;
                if (model_queue_peek(&mq_data, &mq_len) == RT_EOK)
                {
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
                        /* 短写：记录已发送偏移，下次从断点继续 */
                        model_queue_advance_head(sent);
                    }
                }
            }

            /* 接收 Rock 命令 — 使用独立 line_acc 缓冲区（修复 Issue 3）*/
            int rx_len = recv(sock, recv_buf, sizeof(recv_buf) - 1, 0);
            if (rx_len > 0)
            {
                LOG_D("Received %d bytes from Rock", rx_len);
                net_io_consume_lines(line_acc, &rx_acc_len,
                                    (int)sizeof(line_acc),
                                    recv_buf, rx_len,
                                    rock_line_cb,
                                    RT_NULL,
                                    &imu_parse_err);
            }
            else if (rx_len < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            {
                LOG_W("Recv error, reconnecting...");
                break;
            }
        }

        /* 关闭socket */
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
    if (data == RT_NULL || len <= 0)
        return;
    if (len >= NET_MODEL_MSG_MAX)
        return;

    rt_err_t r = model_queue_push(data, len);
    if (r == -RT_EFULL)
    {
        LOG_W("MODEL queue full, dropping %d-byte message", len);
    }
}

inference_mode_t imu_wifi_sender_get_inference_mode(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    inference_mode_t cur = g_infer_mode;
    rt_hw_interrupt_enable(level);
    return cur;
}

void imu_wifi_sender_request_inference_mode_switch(void)
{
    inference_mode_t next;
    uint32_t new_gen;

    rt_base_t level = rt_hw_interrupt_disable();
    next = (g_infer_mode == INFER_MODE_WORD) ? INFER_MODE_SENTENCE
                                              : INFER_MODE_WORD;
    g_infer_mode = next;
    g_mode_generation++;
    g_mode_has_user_selection = RT_TRUE;
    new_gen = g_mode_generation;
    rt_hw_interrupt_enable(level);

    LOG_I("ROCK inference mode requested: %s (gen=%u)",
          infer_mode_str(next), (unsigned)new_gen);
}
