/**
 * @file    uart_oled_thread.c
 * @brief   串口接收线程 - 从UART1接收文本并显示到OLED
 *
 * @details 本线程实现两个功能:
 *          1. 从UART1串口接收文本数据，按行解析后滚动显示到OLED屏幕
 *          2. 在OLED顶部状态栏实时显示电池电压和电量图标
 *
 *          OLED显示布局 (128×64像素):
 *          ┌────────────────────────────────────────────┐
 *          │ Y=0~7   (8px):  状态栏 [电压 X.XXV XX%][图标] │
 *          │ Y=8~23  (16px): 文本第1行 (8×16字体)         │
 *          │ Y=24~39 (16px): 文本第2行 (8×16字体)         │
 *          │ Y=40~55 (16px): 文本第3行 (8×16字体)         │
 *          │ Y=56~63 (8px):  (未使用)                     │
 *          └────────────────────────────────────────────┘
 *
 *          硬件连接:
 *          - UART1引脚: PF13-TX, PF12-RX (P1排针)
 *          - OLED通过TCA9548A通道3连接到I2C2 (PE1-SCL, PE2-SDA)
 *
 *          电池状态栏每3秒自动刷新一次 (仅局部更新，不影响文本区域)
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <stdio.h>
#include "uart_oled_thread.h"
#include "../IIC/tca9548a.h"
#include "../IIC/OLED/OLED.h"
#include "../IIC/i2c2_mutex.h"
#include "../applications/adc_battery.h"

/*
 * OLED显示参数 (OLED_8X16字体)
 * 状态栏占用顶部8像素，剩余56像素可显示3行16像素高的文本
 */
#define OLED_MAX_COLS       16      /* 128 / 8 = 16 ASCII字符每行 */
#define OLED_MAX_ROWS       3       /* 状态栏占8px，剩余56px / 16 = 3行文本 */
#define OLED_FONT_HEIGHT    16
#define OLED_LINE_BUF_SIZE  48      /* 行缓冲区字节数: 8中文×3字节UTF-8 + 余量 */

/* 接收缓冲区 */
#define UART_RX_BUF_SIZE    256

/* OLED连接在TCA9548A的通道号 */
#define OLED_TCA9548A_CHANNEL   3

/* 电池图标显示位置 (右上角，16×8像素) */
#define BAT_ICON_X          96      /* 128 - 16(icon) - 16(间距) */
#define BAT_ICON_Y          0
#define BAT_ICON_W          16
#define BAT_ICON_H          8

/* 电池电压文本位置 (图标左侧，6×8小字体) */
#define BAT_TEXT_X          48
#define BAT_TEXT_Y          0

/* 显示缓冲区: 3行，每行最多存放 OLED_LINE_BUF_SIZE 字节 (支持UTF-8中文) */
static char oled_lines[OLED_MAX_ROWS][OLED_LINE_BUF_SIZE + 1];
static int oled_line_count = 0;

/**
 * @brief   根据电池百分比获取对应的电池图标
 * @param   percentage  电池电量百分比 0-100
 * @return  指向对应电池图标数据的指针 (16×8像素，14级精细显示)
 *
 * @note    图标数据定义在 OLED_Data.c 中，共14个等级:
 *          Battery100 (100%)  →  Battery0 (0%)
 *          每个等级对应约8%的电量区间
 */
static const uint8_t *battery_get_icon(rt_uint8_t percentage)
{
    if (percentage >= 100)     return Battery100;
    else if (percentage >= 92) return Battery92_99;
    else if (percentage >= 84) return Battery84_91;
    else if (percentage >= 76) return Battery76_83;
    else if (percentage >= 68) return Battery68_75;
    else if (percentage >= 60) return Battery60_67;
    else if (percentage >= 52) return Battery52_59;
    else if (percentage >= 44) return Battery44_51;
    else if (percentage >= 36) return Battery36_43;
    else if (percentage >= 28) return Battery28_35;
    else if (percentage >= 19) return Battery19_27;
    else if (percentage >= 10) return Battery10_18;
    else if (percentage >= 1)  return Battery1_9;
    else                       return Battery0;
}

/**
 * @brief   获取I2C总线并切换到OLED通道
 * @return  RT_EOK 成功, 其他失败
 *
 * @note    由于I2C2总线被OLED和MPU6050传感器共享，每次操作OLED前必须:
 *          1. 获取I2C2互斥锁 (防止与MPU6050线程冲突)
 *          2. 重新初始化I2C GPIO引脚 (MPU6050线程使用RT-Thread I2C框架可能改变GPIO状态)
 *          3. 通过TCA9548A切换到OLED所在的通道3
 */
static rt_err_t oled_acquire_bus(void)
{
    /* 获取I2C2总线互斥锁，超时1000ms */
    if (i2c2_mutex_take(1000) != RT_EOK)
    {
        rt_kprintf("[UART_OLED] Failed to acquire I2C2 mutex\n");
        return -RT_ERROR;
    }

    /*
     * 重新初始化I2C GPIO引脚，确保bit-bang正常工作
     * (RT-Thread I2C框架操作PE1/PE2后可能影响GPIO状态)
     */
    OLED_I2C_Init();

    /* 有TCA9548A时切换到OLED通道 */
    if (tca9548a_is_present())
    {
        /*
         * MPU6050线程通过RT-Thread I2C框架操作TCA9548A后，
         * tca9548a.c的current_channel缓存已过期，
         * 先禁用所有通道使缓存失效，再重新选择OLED通道
         */
        tca9548a_disable_all_channels();
        tca9548a_select_channel(OLED_TCA9548A_CHANNEL);
    }

    return RT_EOK;
}

/**
 * @brief   释放I2C总线
 */
static void oled_release_bus(void)
{
    i2c2_mutex_release();
}

/**
 * @brief   在OLED顶部状态栏显示电池图标和电压
 * @note    仅更新第0行区域 (Y=0, 高8像素)，不清除文本区域
 *
 *          状态栏布局:
 *          ┌──────────┬───────────────────────┬──────────┐
 *          │ X=0~47   │ X=48~111              │ X=112~127│
 *          │ (空白)   │ "X.XXV XX%" (6x8字体)  │ [电池图标]│
 *          └──────────┴───────────────────────┴──────────┘
 */
static void oled_update_battery_bar(void)
{
    /* 从电池ADC采集线程获取当前电压和电量 */
    rt_uint32_t voltage_mv = battery_get_voltage();
    rt_uint8_t  percentage = battery_get_percentage();

    /* 根据电量百分比选择对应的14级电池图标 */
    const uint8_t *icon = battery_get_icon(percentage);

    /* 格式化电压和电量字符串: "X.XXV XX%" */
    char bat_str[12];
    rt_snprintf(bat_str, sizeof(bat_str), "%d.%02dV%3d%%",
                voltage_mv / 1000,
                (voltage_mv % 1000) / 10,
                percentage);

    /* 清除状态栏区域 (从BAT_TEXT_X开始到屏幕右端, Y=0, 高8像素) */
    OLED_ClearArea(BAT_TEXT_X, BAT_TEXT_Y, 128 - BAT_TEXT_X, BAT_ICON_H);

    /* 显示电压文本 (使用6x8小字体，节省状态栏空间) */
    OLED_ShowString(BAT_TEXT_X, BAT_TEXT_Y, bat_str, OLED_6X8);

    /* 在最右侧显示电池图标 (16x8像素) */
    OLED_ShowImage(128 - BAT_ICON_W, BAT_ICON_Y, BAT_ICON_W, BAT_ICON_H, icon);
}

/**
 * @brief   将接收到的文本更新到OLED显示 (全屏刷新)
 * @note    刷新流程:
 *          1. 获取I2C总线 → 2. 清空显存 → 3. 绘制状态栏
 *          → 4. 绘制文本行 → 5. 发送到OLED硬件 → 6. 释放总线
 */
static void oled_display_text(void)
{
    /* 获取I2C总线并切换到OLED通道 */
    if (oled_acquire_bus() != RT_EOK)
        return;

    /* 清空整个OLED显存 */
    OLED_Clear();

    /* 顶部状态栏: 电池图标 + 电压 */
    oled_update_battery_bar();

    /* 文本区域从Y=8开始 (状态栏下方)，最多显示3行文本 */
    for (int i = 0; i < oled_line_count && i < OLED_MAX_ROWS; i++)
    {
        OLED_ShowString(0, 8 + i * OLED_FONT_HEIGHT, oled_lines[i], OLED_8X16);
    }

    /* 将显存数据发送到OLED硬件 */
    OLED_Update();

    /* 释放I2C总线 */
    oled_release_bus();
}

/**
 * @brief   添加一行文本到显示缓冲区
 * @param   line 要显示的文本行
 *
 * @note    显示缓冲区最多容纳3行文本 (OLED_MAX_ROWS)。
 *          当缓冲区已满时，自动向上滚动:
 *          - 第1行被丢弃
 *          - 第2行移到第1行位置
 *          - 第3行移到第2行位置
 *          - 新文本写入第3行
 *          超过16字符的文本会被截断。
 */
static void oled_add_line(const char *line)
{
    /* 如果已满3行，向上滚动: 将每行内容复制到上一行 */
    if (oled_line_count >= OLED_MAX_ROWS)
    {
        for (int i = 0; i < OLED_MAX_ROWS - 1; i++)
        {
            rt_memcpy(oled_lines[i], oled_lines[i + 1], OLED_LINE_BUF_SIZE + 1);
        }
        oled_line_count = OLED_MAX_ROWS - 1;
    }

    /* 复制新行到缓冲区末尾，截断超出缓冲区的部分 */
    rt_strncpy(oled_lines[oled_line_count], line, OLED_LINE_BUF_SIZE);
    oled_lines[oled_line_count][OLED_LINE_BUF_SIZE] = '\0';
    oled_line_count++;

    /* 立即刷新OLED显示 */
    oled_display_text();
}

/**
 * @brief   将STT识别结果显示到OLED (支持中文, 自动分行)
 * @param   text  UTF-8编码的文本
 *
 * @note    中文字符占16像素宽, 每行128像素最多8个中文字符。
 *          按字符宽度自动换行, 通过oled_add_line逐行显示并滚动。
 *          可从任意线程安全调用。
 */
void oled_show_stt_result(const char *text)
{
    if (text == RT_NULL || text[0] == '\0')
        return;

    char line_buf[OLED_LINE_BUF_SIZE + 1];
    int line_pos = 0;       /* 当前行已占像素宽度 */
    int buf_idx = 0;        /* line_buf 写入位置 */
    int i = 0;

    while (text[i] != '\0')
    {
        uint8_t c = (uint8_t)text[i];
        int char_bytes = 0;     /* 当前字符的 UTF-8 字节数 */
        int char_width = 0;     /* 当前字符的像素宽度 */

        if ((c & 0x80) == 0x00)
        {
            char_bytes = 1;
            char_width = 8;     /* ASCII: 8 像素宽 */
        }
        else if ((c & 0xE0) == 0xC0)
        {
            char_bytes = 2;
            char_width = 16;    /* 多字节字符: 16 像素宽 */
        }
        else if ((c & 0xF0) == 0xE0)
        {
            char_bytes = 3;
            char_width = 16;
        }
        else if ((c & 0xF8) == 0xF0)
        {
            char_bytes = 4;
            char_width = 16;
        }
        else
        {
            i++;
            continue;   /* 跳过无效字节 */
        }

        /* 检查后续字节是否完整 */
        int valid = 1;
        for (int j = 1; j < char_bytes; j++)
        {
            if (text[i + j] == '\0') { valid = 0; break; }
        }
        if (!valid) break;

        /* 换行符处理 */
        if (char_bytes == 1 && (text[i] == '\n' || text[i] == '\r'))
        {
            if (buf_idx > 0)
            {
                line_buf[buf_idx] = '\0';
                oled_add_line(line_buf);
                buf_idx = 0;
                line_pos = 0;
            }
            i++;
            continue;
        }

        /* 当前行放不下这个字符, 先输出当前行 */
        if (line_pos + char_width > 128)
        {
            line_buf[buf_idx] = '\0';
            oled_add_line(line_buf);
            buf_idx = 0;
            line_pos = 0;
        }

        /* 缓冲区溢出保护 */
        if (buf_idx + char_bytes >= OLED_LINE_BUF_SIZE)
        {
            line_buf[buf_idx] = '\0';
            oled_add_line(line_buf);
            buf_idx = 0;
            line_pos = 0;
        }

        /* 将字符字节复制到行缓冲区 */
        for (int j = 0; j < char_bytes; j++)
        {
            line_buf[buf_idx++] = text[i + j];
        }
        line_pos += char_width;
        i += char_bytes;
    }

    /* 输出最后一行 */
    if (buf_idx > 0)
    {
        line_buf[buf_idx] = '\0';
        oled_add_line(line_buf);
    }
}

/* 串口接收回调信号量: 串口收到数据时释放此信号量唤醒线程 */
static struct rt_semaphore rx_sem;

/**
 * @brief   串口接收回调函数
 * @note    在中断上下文中被调用，仅释放信号量通知线程，不做耗时操作
 */
static rt_err_t uart_rx_callback(rt_device_t dev, rt_size_t size)
{
    rt_sem_release(&rx_sem);
    return RT_EOK;
}

/**
 * @brief   串口接收/OLED显示线程入口函数
 *
 * @details 线程工作流程:
 *          1. 初始化信号量，打开UART1设备 (中断接收模式)
 *          2. 等待IIC线程完成OLED初始化 (延时2000ms)
 *          3. 显示初始提示信息 "UART1 Ready"
 *          4. 进入主循环:
 *             - 等待串口接收信号量 (超时500ms)
 *             - 收到数据: 按行解析，显示到OLED
 *             - 超时: 检查是否有未完成的行数据
 *             - 每3秒 (500ms×6次) 刷新一次电池状态栏
 */
void uart_oled_thread_entry(void *parameter)
{
    rt_device_t uart_dev = RT_NULL;
    char line_buf[UART_RX_BUF_SIZE];
    int line_pos = 0;
    char ch;

    rt_kprintf("[UART_OLED] Thread started\n");

    /* 初始化信号量: 初始值0，FIFO模式 */
    rt_sem_init(&rx_sem, "uart_rx", 0, RT_IPC_FLAG_FIFO);

    /* 查找串口设备 */
    uart_dev = rt_device_find(UART_OLED_DEVICE_NAME);
    if (uart_dev == RT_NULL)
    {
        rt_kprintf("[UART_OLED] Cannot find device: %s\n", UART_OLED_DEVICE_NAME);
        return;
    }

    /* 以中断接收模式打开串口 (非阻塞接收 + 阻塞发送) */
    rt_err_t ret = rt_device_open(uart_dev, RT_DEVICE_FLAG_RX_NON_BLOCKING | RT_DEVICE_FLAG_TX_BLOCKING);
    if (ret != RT_EOK)
    {
        rt_kprintf("[UART_OLED] Failed to open %s, err=%d\n", UART_OLED_DEVICE_NAME, ret);
        return;
    }

    /* 设置接收回调: 串口收到数据时调用uart_rx_callback释放信号量 */
    rt_device_set_rx_indicate(uart_dev, uart_rx_callback);

    rt_kprintf("[UART_OLED] Device %s opened, waiting for input...\n", UART_OLED_DEVICE_NAME);

    /* 等待IIC线程完成OLED初始化 (iic_thread初始化OLED需要约1秒) */
    rt_thread_mdelay(2000);

    /* 初始显示提示信息 */
    oled_add_line("UART1 Ready");
    oled_add_line("Send text...");

    /*
     * 电池状态栏刷新计数器
     * 每次循环超时500ms，6次超时 = 3秒刷新一次电池状态
     * 仅做局部更新 (OLED_UpdateArea)，不影响文本区域显示
     */
    rt_uint8_t bat_refresh_cnt = 0;
    #define BAT_REFRESH_INTERVAL  6     /* 500ms × 6 = 3秒 */

    while (1)
    {
        /* 等待接收信号量，超时500ms */
        rt_err_t sem_ret = rt_sem_take(&rx_sem, rt_tick_from_millisecond(500));
        if (sem_ret == RT_EOK)
        {
            /* 收到串口数据，读取所有可用字符 */
            while (rt_device_read(uart_dev, -1, &ch, 1) == 1)
            {
                /* 遇到换行符，将已积累的字符作为一行显示 */
                if (ch == '\r' || ch == '\n')
                {
                    if (line_pos > 0)
                    {
                        line_buf[line_pos] = '\0';
                        rt_kprintf("[UART_OLED] Recv: %s\n", line_buf);
                        oled_add_line(line_buf);
                        line_pos = 0;
                    }
                }
                else
                {
                    /* 普通字符，追加到行缓冲区 */
                    if (line_pos < (int)(sizeof(line_buf) - 1))
                    {
                        line_buf[line_pos++] = ch;
                    }
                }
            }
        }
        else
        {
            /* 超时：如果缓冲区有数据但没收到换行符，也显示出来 */
            if (line_pos > 0)
            {
                line_buf[line_pos] = '\0';
                rt_kprintf("[UART_OLED] Recv(timeout): %s\n", line_buf);
                oled_add_line(line_buf);
                line_pos = 0;
            }
        }

        /*
         * 定时刷新电池状态栏 (仅局部更新，不影响文本区域)
         * 使用 OLED_UpdateArea 只刷新状态栏所在的像素区域，
         * 避免全屏刷新导致的闪烁和不必要的I2C传输
         */
        if (++bat_refresh_cnt >= BAT_REFRESH_INTERVAL)
        {
            bat_refresh_cnt = 0;
            if (oled_acquire_bus() == RT_EOK)
            {
                oled_update_battery_bar();
                /* 仅更新状态栏区域 (从BAT_TEXT_X到屏幕右端, Y=0, 高8像素) */
                OLED_UpdateArea(BAT_TEXT_X, BAT_ICON_Y, 128 - BAT_TEXT_X, BAT_ICON_H);
                oled_release_bus();
            }
        }
    }
}
