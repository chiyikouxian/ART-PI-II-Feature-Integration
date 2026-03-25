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

    /* 播放开机语音 */
    vtx316_send_frame("\xE7\xB3\xBB\xE7\xBB\x9F\xE5\x90\xAF\xE5\x8A\xA8\xE5\xAE\x8C\xE6\x88\x90");
    rt_kprintf("[VTX316] Startup speech sent\n");

    /* 线程初始化完成后退出 */
    rt_kprintf("[VTX316] Init complete, thread exit\n");
}

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
