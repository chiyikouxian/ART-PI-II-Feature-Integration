/**
 * @file vtx316.c
 * @brief VTX316语音合成芯片驱动
 * @note  通过USART1 (PF13-TX, PF12-RX) 与VTX316通信，115200bps
 *        协议帧: [0xFD] [长度高] [长度低] [0x01] [0x05] [UTF-8文本...]
 *        播报完成: VTX316返回 0x41 0x4F ("AO")
 *
 *        流控机制:
 *        - vtx316_speak() 非阻塞，若busy则跳过
 *        - vtx316_speak_wait() 阻塞等待上一次完成后再发送
 *        - 收到 0x41 0x4F 后释放完成事件
 *
 *        TCP文本转语音:
 *        - TCP接收格式: "SAY:要播报的文本"
 *        - vtx316_tcp_recv_handler() 注册为TCP recv callback
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>
#include "vtx316.h"

/* 串口设备句柄 */
static rt_device_t vtx316_uart = RT_NULL;

/* 播报忙标志 */
static volatile rt_bool_t vtx316_busy = RT_FALSE;

/* 播报完成事件 */
static struct rt_event vtx316_event;
#define VTX316_EVT_DONE  (1 << 0)

/* UART接收状态机: 检测 0x41 0x4F 序列 */
static rt_uint8_t rx_state = 0;   /* 0=idle, 1=got 0x41 */
static volatile rt_bool_t g_vtx_ready = RT_FALSE;
static volatile rt_bool_t g_boot_success_spoken = RT_FALSE;
static volatile rt_bool_t g_boot_success_pending = RT_FALSE;
static rt_uint32_t g_pending_error_mask = 0;

static const char *vtx316_boot_error_texts[VTX316_BOOT_ERROR_MAX] = {
    "左手传感器异常",
    "左手WLAN设备未就绪",
    "左手WiFi连接调用失败",
    "左手WiFi连接超时",
    "左手数据发送失败",
    "左手语音模块异常",
    "左手蓝牙异常"
};

static void vtx316_flush_pending_boot_errors(void)
{
    int i;

    if (!g_vtx_ready)
        return;

    for (i = 0; i < VTX316_BOOT_ERROR_MAX; i++)
    {
        rt_uint32_t bit = 1U << i;

        if ((g_pending_error_mask & bit) == 0)
            continue;

        g_pending_error_mask &= ~bit;
        vtx316_speak_wait(vtx316_boot_error_texts[i]);
    }
}

/**
 * @brief  UART接收回调 (中断上下文)
 * @note   检测VTX316播报完成回复 0x41 0x4F
 */
static rt_err_t vtx316_uart_rx_callback(rt_device_t dev, rt_size_t size)
{
    rt_uint8_t ch;

    while (rt_device_read(dev, -1, &ch, 1) == 1)
    {
        switch (rx_state)
        {
        case 0:
            if (ch == VTX316_REPLY_DONE_0)  /* 0x41 */
                rx_state = 1;
            break;
        case 1:
            if (ch == VTX316_REPLY_DONE_1)  /* 0x4F */
            {
                /* 收到完整的 41 4F -> 播报完成 */
                vtx316_busy = RT_FALSE;
                rt_event_send(&vtx316_event, VTX316_EVT_DONE);
            }
            rx_state = 0;
            break;
        default:
            rx_state = 0;
            break;
        }
    }

    return RT_EOK;
}

/**
 * @brief  通过串口发送单个字节
 */
static void vtx316_send_byte(rt_uint8_t byte)
{
    if (vtx316_uart != RT_NULL)
    {
        rt_device_write(vtx316_uart, 0, &byte, 1);
    }
}

/**
 * @brief  内部: 发送VTX316语音帧 (不检查busy)
 */
static void vtx316_send_frame(const char *text)
{
    rt_uint16_t len;
    rt_uint16_t data_len;

    if (vtx316_uart == RT_NULL || text == RT_NULL)
        return;

    len = strlen(text);
    data_len = len + 2;  /* 命令字(1) + 编码类型(1) + 文本数据 */

    /* 标记忙 */
    vtx316_busy = RT_TRUE;

    /* 清除之前可能残留的完成事件 */
    rt_event_recv(&vtx316_event, VTX316_EVT_DONE,
                  RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                  0, RT_NULL);

    /* 发送帧头 */
    vtx316_send_byte(VTX316_FRAME_HEADER);
    /* 发送数据区总长度 (高字节在前) */
    vtx316_send_byte((rt_uint8_t)(data_len >> 8));
    vtx316_send_byte((rt_uint8_t)(data_len & 0xFF));
    /* 发送命令字: 0x01 = 合成播报 */
    vtx316_send_byte(VTX316_CMD_SPEAK);
    /* 发送编码类型: 0x05 = UTF-8 */
    vtx316_send_byte(VTX316_ENCODING_UTF8);
    /* 发送UTF-8文本数据 */
    while (*text)
    {
        vtx316_send_byte((rt_uint8_t)*text++);
    }
}

/**
 * @brief  VTX316语音播报 (非阻塞)
 * @note   若当前正在播报则跳过本次请求
 */
void vtx316_speak(const char *text)
{
    if (vtx316_busy)
        return;

    vtx316_send_frame(text);
}

/**
 * @brief  VTX316语音播报 (阻塞等待上一次完成)
 * @return RT_EOK 成功, -RT_ETIMEOUT 超时
 */
rt_err_t vtx316_speak_wait(const char *text)
{
    rt_uint32_t evt;

    if (vtx316_uart == RT_NULL || text == RT_NULL)
        return -RT_ERROR;

    /* 等待上一次播报完成 */
    if (vtx316_busy)
    {
        rt_err_t ret = rt_event_recv(&vtx316_event, VTX316_EVT_DONE,
                                     RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                                     rt_tick_from_millisecond(VTX316_SPEAK_TIMEOUT_MS),
                                     &evt);
        if (ret != RT_EOK)
        {
            rt_kprintf("[VTX316] Wait timeout, force send\n");
            vtx316_busy = RT_FALSE;
        }
    }

    /* 发送新的语音帧 */
    vtx316_send_frame(text);

    return RT_EOK;
}

/**
 * @brief  查询VTX316是否正在播报
 */
rt_bool_t vtx316_is_busy(void)
{
    return vtx316_busy;
}

rt_bool_t vtx316_is_ready(void)
{
    return g_vtx_ready;
}

void vtx316_report_boot_error(vtx316_boot_error_t error)
{
    rt_uint32_t bit;

    if (error < 0 || error >= VTX316_BOOT_ERROR_MAX)
        return;

    bit = 1U << error;

    if (!g_vtx_ready)
    {
        g_pending_error_mask |= bit;
        return;
    }

    if ((g_pending_error_mask & bit) != 0)
        g_pending_error_mask &= ~bit;

    vtx316_speak_wait(vtx316_boot_error_texts[error]);
}

void vtx316_report_boot_success(void)
{
    if (g_boot_success_spoken)
        return;

    if (!g_vtx_ready)
    {
        g_boot_success_pending = RT_TRUE;
        return;
    }

    if (g_pending_error_mask != 0)
    {
        vtx316_flush_pending_boot_errors();
        if (g_pending_error_mask != 0)
            return;
    }

    g_boot_success_pending = RT_FALSE;
    g_boot_success_spoken = RT_TRUE;
    vtx316_speak_wait("系统初始化成功");
}

/**
 * @brief  TCP接收回调: 解析文本并转发到VTX316
 * @param  data  接收数据 (格式: "SAY:要播报的文本")
 * @param  len   数据长度
 */
void vtx316_tcp_recv_handler(const char *data, int len)
{
    /* 检查 "SAY:" 前缀 (不区分大小写) */
    if (len > 4 &&
        (data[0] == 'S' || data[0] == 's') &&
        (data[1] == 'A' || data[1] == 'a') &&
        (data[2] == 'Y' || data[2] == 'y') &&
        data[3] == ':')
    {
        /* 提取文本内容 (去掉 "SAY:" 前缀) */
        const char *text = data + 4;
        int text_len = len - 4;

        /* 去掉末尾换行符 */
        while (text_len > 0 && (text[text_len - 1] == '\n' || text[text_len - 1] == '\r'))
            text_len--;

        if (text_len > 0)
        {
            /* 复制到临时缓冲区确保零终止 */
            char speak_buf[256];
            if (text_len > (int)sizeof(speak_buf) - 1)
                text_len = sizeof(speak_buf) - 1;

            rt_memcpy(speak_buf, text, text_len);
            speak_buf[text_len] = '\0';

            rt_kprintf("[VTX316] TCP speech: %s\n", speak_buf);
            vtx316_speak_wait(speak_buf);
        }
    }
    else
    {
        /* 非SAY命令, 打印接收内容 */
        rt_kprintf("[TCP] Recv[%d]: %.*s\n", len, len, data);
    }
}

/**
 * @brief  VTX316线程入口函数
 */
/* ========== 调试模式: Console镜像输出到UART1 ========== */
/* 调试完成后将 CONSOLE_MIRROR_DEBUG 改为 0 即可恢复VTX316语音功能 */
#define CONSOLE_MIRROR_DEBUG  0

#if CONSOLE_MIRROR_DEBUG

#include <stdarg.h>

static rt_device_t mirror_uart = RT_NULL;

/**
 * @brief  重写rt_kprintf (原函数为rt_weak)
 *         在原有console输出的基础上，同时镜像到uart1
 */
int rt_kprintf(const char *fmt, ...)
{
    va_list args;
    rt_size_t length = 0;
    static char mirror_log_buf[256];

    va_start(args, fmt);
    length = rt_vsnprintf(mirror_log_buf, sizeof(mirror_log_buf) - 1, fmt, args);
    if (length > sizeof(mirror_log_buf) - 1)
        length = sizeof(mirror_log_buf) - 1;
    va_end(args);

    /* 原始console输出 */
    rt_device_t console = rt_console_get_device();
    if (console != RT_NULL)
        rt_device_write(console, 0, mirror_log_buf, length);

    /* 镜像到uart1 */
    if (mirror_uart != RT_NULL)
        rt_device_write(mirror_uart, 0, mirror_log_buf, length);

    return length;
}

void vtx316_thread_entry(void *parameter)
{
    struct serial_configure config = RT_SERIAL_CONFIG_DEFAULT;

    rt_kprintf("[MIRROR] Console mirror mode (UART1)\n");

    /* 查找串口设备 */
    mirror_uart = rt_device_find(VTX316_UART_NAME);
    if (mirror_uart == RT_NULL)
    {
        rt_kprintf("[MIRROR] ERROR: %s not found!\n", VTX316_UART_NAME);
        return;
    }

    /* 配置串口参数: 115200bps, 8N1 */
    config.baud_rate = VTX316_UART_BAUDRATE;
    config.data_bits = DATA_BITS_8;
    config.stop_bits = STOP_BITS_1;
    config.parity    = PARITY_NONE;
    rt_device_control(mirror_uart, RT_DEVICE_CTRL_CONFIG, &config);

    /* 以写模式打开串口 */
    if (rt_device_open(mirror_uart, RT_DEVICE_OFLAG_WRONLY) != RT_EOK)
    {
        rt_kprintf("[MIRROR] ERROR: Failed to open %s\n", VTX316_UART_NAME);
        mirror_uart = RT_NULL;
        return;
    }

    rt_kprintf("[MIRROR] %s opened, console output mirrored\n", VTX316_UART_NAME);
}

#else /* !CONSOLE_MIRROR_DEBUG */

void vtx316_thread_entry(void *parameter)
{
    struct serial_configure config = RT_SERIAL_CONFIG_DEFAULT;

    rt_kprintf("[VTX316] Thread started\n");

    /* 初始化完成事件 */
    rt_event_init(&vtx316_event, "vtx_evt", RT_IPC_FLAG_FIFO);

    /* 查找串口设备 */
    vtx316_uart = rt_device_find(VTX316_UART_NAME);
    if (vtx316_uart == RT_NULL)
    {
        rt_kprintf("[VTX316] ERROR: %s not found!\n", VTX316_UART_NAME);
        return;
    }

    /* 配置串口参数: 115200bps, 8N1 */
    config.baud_rate = VTX316_UART_BAUDRATE;
    config.data_bits = DATA_BITS_8;
    config.stop_bits = STOP_BITS_1;
    config.parity    = PARITY_NONE;
    rt_device_control(vtx316_uart, RT_DEVICE_CTRL_CONFIG, &config);

    /* 以读写模式打开串口, 接收使用中断模式 */
    if (rt_device_open(vtx316_uart, RT_DEVICE_OFLAG_RDWR | RT_DEVICE_FLAG_INT_RX) != RT_EOK)
    {
        rt_kprintf("[VTX316] ERROR: Failed to open %s\n", VTX316_UART_NAME);
        vtx316_uart = RT_NULL;
        return;
    }

    /* 设置接收回调 */
    rt_device_set_rx_indicate(vtx316_uart, vtx316_uart_rx_callback);

    rt_kprintf("[VTX316] %s opened (115200, 8N1, RX interrupt)\n", VTX316_UART_NAME);

    /* 等待VTX316芯片初始化完成 */
    rt_thread_mdelay(500);

    g_vtx_ready = RT_TRUE;
    vtx316_flush_pending_boot_errors();
    if (g_boot_success_pending)
        vtx316_report_boot_success();

    /* 线程初始化完成后退出 */
    rt_kprintf("[VTX316] Init complete, thread exit\n");
}

#endif /* CONSOLE_MIRROR_DEBUG */

/* MSH命令: vtx316_say <text> */
static void vtx316_say(int argc, char **argv)
{
    if (argc < 2)
    {
        rt_kprintf("Usage: vtx316_say <text>\n");
        return;
    }

    if (vtx316_uart == RT_NULL)
    {
        rt_kprintf("[VTX316] Not initialized!\n");
        return;
    }

    vtx316_speak_wait(argv[1]);
    rt_kprintf("[VTX316] Sent: %s\n", argv[1]);
}
MSH_CMD_EXPORT(vtx316_say, VTX316 speech synthesis: vtx316_say <text>);
