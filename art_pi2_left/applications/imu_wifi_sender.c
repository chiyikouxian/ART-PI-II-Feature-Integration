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

/* 帧序号 */
static volatile uint32_t frame_seq = 0;

/* 待发送的模型数据（校准阈值） */
#define MODEL_BUF_SIZE 512
static char model_buf[MODEL_BUF_SIZE];
static volatile int model_buf_len = 0;

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
    n = rt_snprintf(p, remain, "[DATA]%u,left,%u", timestamp_ms, seq);
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

    /* 处理SAY:文本命令 */
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
            LOG_I("VTX316 speak: %s", text);
            vtx316_speak_wait(text);

            /* 同时通过TCP发送翻译后的文本（与BLE行为一致） */
            tcp_set_translated_text(text, rt_strlen(text));

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

        /* 设置接收超时 10ms（非阻塞接收） */
        struct timeval recv_timeout;
        recv_timeout.tv_sec = 0;
        recv_timeout.tv_usec = 10000;  /* 10ms */
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));

        /* 连接Rock服务器 */
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

        /* 接收缓冲区状态 */
        int recv_buf_pos = 0;

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

                /* 发送数据 */
                ret = send(sock, send_buf, len, 0);
                if (ret <= 0)
                {
                    LOG_W("Send failed, reconnecting...");
                    break;
                }
                else
                {
                    LOG_D("Sent %d bytes to Rock", ret);
                }
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

            /* 发送待发送的模型数据（校准阈值） */
            if (model_buf_len > 0)
            {
                int mlen = model_buf_len;
                ret = send(sock, model_buf, mlen, 0);
                if (ret > 0) {
                    LOG_I("Sent MODEL data (%d bytes) to Rock", mlen);
                }
                model_buf_len = 0;
            }

            /* 尝试接收Rock发送的命令（非阻塞，10ms超时） */
            ret = recv(sock, recv_buf + recv_buf_pos, sizeof(recv_buf) - recv_buf_pos - 1, 0);
            if (ret > 0)
            {
                LOG_D("Received %d bytes from Rock", ret);
                recv_buf_pos += ret;
                recv_buf[recv_buf_pos] = '\0';

                int processed = 0;

                /* 方案1：优先检查是否有换行符（支持 \n 和 \r\n） */
                char *newline = strchr(recv_buf, '\n');
                if (newline != RT_NULL)
                {
                    /* 提取命令（去除换行符） */
                    int cmd_len = newline - recv_buf;
                    char cmd_buf[RECV_BUF_SIZE];

                    if (cmd_len > 0 && cmd_len < RECV_BUF_SIZE)
                    {
                        rt_memcpy(cmd_buf, recv_buf, cmd_len);
                        cmd_buf[cmd_len] = '\0';

                        /* 去除尾部的 \r 和空格 */
                        while (cmd_len > 0 && (cmd_buf[cmd_len - 1] == '\r' ||
                                               cmd_buf[cmd_len - 1] == ' ' ||
                                               cmd_buf[cmd_len - 1] == '\t'))
                        {
                            cmd_buf[--cmd_len] = '\0';
                        }

                        /* 处理命令 */
                        if (cmd_len > 0)
                        {
                            handle_rock_command(cmd_buf, cmd_len);
                            processed = 1;
                        }
                    }

                    /* 清空缓冲区（包括换行符后的内容） */
                    recv_buf_pos = 0;
                    recv_buf[0] = '\0';
                }
                /* 方案2：没有换行符，检查是否包含完整的已知命令 */
                else
                {
                    /* 检查 CMD:RESET_SEQ */
                    if (rt_strstr(recv_buf, "CMD:RESET_SEQ") != RT_NULL)
                    {
                        handle_rock_command("CMD:RESET_SEQ", 14);
                        processed = 1;
                    }
                    /* 检查 CMD:START */
                    else if (rt_strstr(recv_buf, "CMD:START") != RT_NULL)
                    {
                        handle_rock_command("CMD:START", 9);
                        processed = 1;
                    }
                    /* 检查 CMD:STOP */
                    else if (rt_strstr(recv_buf, "CMD:STOP") != RT_NULL)
                    {
                        handle_rock_command("CMD:STOP", 8);
                        processed = 1;
                    }
                    /* 检查 MODE:MANUAL */
                    else if (rt_strstr(recv_buf, "MODE:MANUAL") != RT_NULL)
                    {
                        handle_rock_command("MODE:MANUAL", 11);
                        processed = 1;
                    }
                    /* 检查 MODE:AUTO */
                    else if (rt_strstr(recv_buf, "MODE:AUTO") != RT_NULL)
                    {
                        handle_rock_command("MODE:AUTO", 9);
                        processed = 1;
                    }
                    /* 检查 SAY: 命令 */
                    else if (rt_strstr(recv_buf, "SAY:") != RT_NULL || rt_strstr(recv_buf, "say:") != RT_NULL)
                    {
                        handle_rock_command(recv_buf, recv_buf_pos);
                        processed = 1;
                    }

                    /* 如果处理了命令，清空缓冲区 */
                    if (processed)
                    {
                        recv_buf_pos = 0;
                        recv_buf[0] = '\0';
                    }
                    /* 如果缓冲区满了但没有识别到命令，清空避免溢出 */
                    else if (recv_buf_pos >= RECV_BUF_SIZE - 10)
                    {
                        LOG_W("Recv buffer full without valid command, clearing");
                        recv_buf_pos = 0;
                        recv_buf[0] = '\0';
                    }
                }
            }
            else if (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            {
                /* 接收错误（非超时） */
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
    if (data == RT_NULL || len <= 0 || len >= MODEL_BUF_SIZE)
        return;
    rt_base_t level = rt_hw_interrupt_disable();
    rt_memcpy(model_buf, data, len);
    model_buf_len = len;
    rt_hw_interrupt_enable(level);
}


