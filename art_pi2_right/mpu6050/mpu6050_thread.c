/**
 * @file mpu6050_thread.c
 * @brief MPU6050/MPU9250数据采集线程
 * @note  通过双TCA9548A I2C复用模块，读取多路传感器原始数据
 *        I2C1 (PB8/PB9): TCA9548A #1 - 最多8路MPU6050
 *        I2C2 (PE1/PE2): TCA9548A #2 - CH0/CH1: MPU6050, CH2: MPU9250, CH3: OLED
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <stdio.h>
#include "mpu6050_thread.h"
#include "mpu6050.h"
#include "../IIC/i2c2_mutex.h"

/* 传感器数据结构 (统一9轴，6轴传感器的mag字段不使用) */
typedef struct {
    short accel[3];     /* 加速度计数据 */
    short gyro[3];      /* 陀螺仪数据 */
    short mag[3];       /* 磁力计数据 (仅MPU9250使用) */
    rt_bool_t valid;    /* 数据有效标志 */
    rt_bool_t has_mag;  /* 是否有磁力计 (MPU9250为true) */
} mpu_sensor_data_t;

static mpu_sensor_data_t mpu_data_i2c1[MPU6050_I2C1_COUNT];
static mpu_sensor_data_t mpu_data_i2c2[MPU6050_I2C2_COUNT];

/* ========== 串口数据记录 ========== */

#define DATA_LOG_HAND_TYPE  "right"

static volatile rt_bool_t data_log_enabled = RT_FALSE;
static rt_uint32_t data_log_start_tick = 0;

/* ========== TCA9548A通道选择 ========== */

static rt_err_t mpu_tca9548a_select_channel(const char *bus_name, rt_uint8_t channel)
{
    struct rt_i2c_bus_device *i2c_bus;
    struct rt_i2c_msg msg;
    rt_uint8_t data;

    if (channel > 7)
        return -RT_ERROR;

    i2c_bus = (struct rt_i2c_bus_device *)rt_device_find(bus_name);
    if (i2c_bus == RT_NULL)
    {
        rt_kprintf("[MPU-TCA] I2C bus %s not found\n", bus_name);
        return -RT_ERROR;
    }

    data = (1 << channel);
    msg.addr  = MPU_TCA9548A_ADDR;
    msg.flags = RT_I2C_WR;
    msg.buf   = &data;
    msg.len   = 1;

    if (rt_i2c_transfer(i2c_bus, &msg, 1) != 1)
        return -RT_ERROR;

    rt_thread_mdelay(5);
    return RT_EOK;
}

static rt_err_t mpu_tca9548a_disable_all(const char *bus_name)
{
    struct rt_i2c_bus_device *i2c_bus;
    struct rt_i2c_msg msg;
    rt_uint8_t data = 0x00;

    i2c_bus = (struct rt_i2c_bus_device *)rt_device_find(bus_name);
    if (i2c_bus == RT_NULL)
        return -RT_ERROR;

    msg.addr  = MPU_TCA9548A_ADDR;
    msg.flags = RT_I2C_WR;
    msg.buf   = &data;
    msg.len   = 1;

    if (rt_i2c_transfer(i2c_bus, &msg, 1) != 1)
        return -RT_ERROR;

    return RT_EOK;
}

/* ========== 通用I2C读写辅助函数 (用于MPU9250/AK8963直接操作) ========== */

static rt_err_t i2c_write_byte(const char *bus_name, rt_uint8_t dev_addr,
                               rt_uint8_t reg, rt_uint8_t data)
{
    struct rt_i2c_bus_device *i2c_bus;
    struct rt_i2c_msg msgs[1];
    rt_uint8_t buf[2];

    i2c_bus = (struct rt_i2c_bus_device *)rt_device_find(bus_name);
    if (i2c_bus == RT_NULL)
        return -RT_ERROR;

    buf[0] = reg;
    buf[1] = data;
    msgs[0].addr  = dev_addr;
    msgs[0].flags = RT_I2C_WR;
    msgs[0].buf   = buf;
    msgs[0].len   = 2;

    if (rt_i2c_transfer(i2c_bus, msgs, 1) != 1)
        return -RT_ERROR;

    return RT_EOK;
}

static rt_err_t i2c_read_bytes(const char *bus_name, rt_uint8_t dev_addr,
                               rt_uint8_t reg, rt_uint8_t *buf, rt_uint8_t len)
{
    struct rt_i2c_bus_device *i2c_bus;
    struct rt_i2c_msg msgs[2];

    i2c_bus = (struct rt_i2c_bus_device *)rt_device_find(bus_name);
    if (i2c_bus == RT_NULL)
        return -RT_ERROR;

    msgs[0].addr  = dev_addr;
    msgs[0].flags = RT_I2C_WR;
    msgs[0].buf   = &reg;
    msgs[0].len   = 1;

    msgs[1].addr  = dev_addr;
    msgs[1].flags = RT_I2C_RD;
    msgs[1].buf   = buf;
    msgs[1].len   = len;

    if (rt_i2c_transfer(i2c_bus, msgs, 2) != 2)
        return -RT_ERROR;

    return RT_EOK;
}

/* ========== MPU9250 bypass模式初始化 ========== */

static rt_err_t mpu9250_init_bypass(const char *bus_name)
{
    rt_uint8_t who_am_i;

    if (i2c_read_bytes(bus_name, MPU9250_ADDR, 0x75, &who_am_i, 1) != RT_EOK)
    {
        rt_kprintf("[MPU9250] Read WHO_AM_I failed\n");
        return -RT_ERROR;
    }
    rt_kprintf("[MPU9250] WHO_AM_I=0x%02x\n", who_am_i);

    /* 复位 */
    i2c_write_byte(bus_name, MPU9250_ADDR, MPU_PWR_MGMT1_REG, 0x80);
    rt_thread_mdelay(100);

    /* 唤醒，选择最佳时钟源 */
    if (i2c_write_byte(bus_name, MPU9250_ADDR, MPU_PWR_MGMT1_REG, 0x01) != RT_EOK)
        return -RT_ERROR;
    rt_thread_mdelay(10);

    /* 采样率分频 */
    i2c_write_byte(bus_name, MPU9250_ADDR, MPU_SAMPLE_RATE_REG, 0x04);
    /* DLPF */
    i2c_write_byte(bus_name, MPU9250_ADDR, MPU_CFG_REG, 0x03);
    /* 陀螺仪 ±2000dps */
    i2c_write_byte(bus_name, MPU9250_ADDR, MPU_GYRO_CFG_REG, 0x18);
    /* 加速度计 ±16g */
    i2c_write_byte(bus_name, MPU9250_ADDR, MPU_ACCEL_CFG_REG, 0x18);

    /* 启用I2C bypass模式，使主机可以直接访问AK8963 */
    if (i2c_write_byte(bus_name, MPU9250_ADDR, MPU_INTBP_CFG_REG, 0x02) != RT_EOK)
        return -RT_ERROR;
    rt_thread_mdelay(10);

    return RT_EOK;
}

/* ========== AK8963磁力计 ========== */

static rt_err_t ak8963_init(const char *bus_name)
{
    rt_uint8_t who;

    if (i2c_read_bytes(bus_name, AK8963_ADDR, AK8963_WIA, &who, 1) != RT_EOK)
        return -RT_ERROR;

    if (who != AK8963_WHO_AM_I)
    {
        rt_kprintf("[AK8963] WHO_AM_I mismatch: expected 0x%02x, got 0x%02x\n",
                   AK8963_WHO_AM_I, who);
        return -RT_ERROR;
    }
    rt_kprintf("[AK8963] WHO_AM_I=0x%02x (OK)\n", who);

    /* 软复位 */
    i2c_write_byte(bus_name, AK8963_ADDR, AK8963_CNTL2, 0x01);
    rt_thread_mdelay(10);

    /* 连续测量模式2 (100Hz) + 16位输出 */
    if (i2c_write_byte(bus_name, AK8963_ADDR, AK8963_CNTL1, 0x16) != RT_EOK)
        return -RT_ERROR;
    rt_thread_mdelay(10);

    return RT_EOK;
}

static rt_err_t ak8963_read_mag(const char *bus_name, short *mx, short *my, short *mz)
{
    rt_uint8_t buf[7];
    rt_uint8_t st1;

    /* 检查数据是否准备好 */
    if (i2c_read_bytes(bus_name, AK8963_ADDR, AK8963_ST1, &st1, 1) != RT_EOK)
        return -RT_ERROR;

    if (!(st1 & 0x01))
        return -RT_ERROR;  /* 数据未准备好 */

    /* 读取6字节磁力计数据 + ST2 (必须读ST2以完成读取周期) */
    if (i2c_read_bytes(bus_name, AK8963_ADDR, AK8963_HXL, buf, 7) != RT_EOK)
        return -RT_ERROR;

    /* 检查磁场溢出 */
    if (buf[6] & 0x08)
        return -RT_ERROR;

    /* 组合数据 (小端模式) */
    *mx = (short)((buf[1] << 8) | buf[0]);
    *my = (short)((buf[3] << 8) | buf[2]);
    *mz = (short)((buf[5] << 8) | buf[4]);

    return RT_EOK;
}

/* ========== 线程入口 ========== */

void mpu6050_thread_entry(void *parameter)
{
    rt_uint8_t who;
    rt_uint8_t i2c1_active_count = 0;
    rt_uint8_t i2c2_active_count = 0;

    /* 等待IIC线程完成OLED初始化，避免I2C2总线冲突 */
    rt_thread_mdelay(2000);

    rt_kprintf("\n========== Multi-Sensor Initialization ==========\n");
    rt_kprintf("Target: I2C1=%d sensors, I2C2=%d sensors\n",
               MPU6050_I2C1_COUNT, MPU6050_I2C2_COUNT);

    /* ========== 初始化 I2C1 上的 MPU6050 ========== */
    rt_kprintf("\n[I2C1] Scanning TCA9548A channels for MPU6050...\n");
    MPU_SetBus(MPU6050_I2C1_BUS_NAME);
    rt_thread_mdelay(50);

    for (int ch = 0; ch < MPU6050_I2C1_COUNT; ch++)
    {
        mpu_data_i2c1[ch].valid = RT_FALSE;
        mpu_data_i2c1[ch].has_mag = RT_FALSE;

        if (mpu_tca9548a_select_channel(MPU6050_I2C1_BUS_NAME, ch) != RT_EOK)
        {
            rt_kprintf("  [I2C1-CH%d] Failed to select channel\n", ch);
            continue;
        }

        if (mpu6050_getid(MPU6050_I2C1_BUS_NAME, &who) == RT_EOK && who == 0x68)
        {
            rt_kprintf("  [I2C1-CH%d] MPU6050 found! WHO_AM_I=0x%02x\n", ch, who);
            mpu_data_i2c1[ch].valid = RT_TRUE;
            i2c1_active_count++;
        }
        else
        {
            rt_kprintf("  [I2C1-CH%d] No MPU6050 detected\n", ch);
        }
    }
    mpu_tca9548a_disable_all(MPU6050_I2C1_BUS_NAME);

    /* ========== 初始化 I2C2 上的传感器 (需要互斥锁保护) ========== */
    rt_kprintf("\n[I2C2] Scanning TCA9548A channels for sensors...\n");

    i2c2_mutex_take(5000);
    MPU_SetBus(MPU6050_I2C2_BUS_NAME);
    rt_thread_mdelay(50);

    for (int ch = 0; ch < MPU6050_I2C2_COUNT; ch++)
    {
        mpu_data_i2c2[ch].valid = RT_FALSE;
        mpu_data_i2c2[ch].has_mag = RT_FALSE;

        if (mpu_tca9548a_select_channel(MPU6050_I2C2_BUS_NAME, ch) != RT_EOK)
        {
            rt_kprintf("  [I2C2-CH%d] Failed to select channel\n", ch);
            continue;
        }

        if (ch == MPU9250_I2C2_CHANNEL)
        {
            /* 通道2是MPU9250，使用直接I2C读取检测 */
            rt_uint8_t who_am_i;
            if (i2c_read_bytes(MPU6050_I2C2_BUS_NAME, MPU9250_ADDR, 0x75,
                               &who_am_i, 1) == RT_EOK)
            {
                rt_kprintf("  [I2C2-CH%d] Device found! WHO_AM_I=0x%02x", ch, who_am_i);
                if (who_am_i == 0x71)
                    rt_kprintf(" (MPU9250)\n");
                else if (who_am_i == 0x70)
                    rt_kprintf(" (MPU6500 - NO magnetometer!)\n");
                else if (who_am_i == 0x68)
                    rt_kprintf(" (MPU6050)\n");
                else
                    rt_kprintf(" (unknown)\n");

                if (mpu9250_init_bypass(MPU6050_I2C2_BUS_NAME) == RT_EOK)
                {
                    rt_kprintf("  [I2C2-CH%d] Bypass mode enabled, scanning AK8963...\n", ch);

                    /* 尝试读取AK8963的WHO_AM_I来确认磁力计是否存在 */
                    rt_uint8_t ak_who = 0;
                    rt_err_t ak_ret = i2c_read_bytes(MPU6050_I2C2_BUS_NAME,
                                                      AK8963_ADDR, AK8963_WIA,
                                                      &ak_who, 1);
                    rt_kprintf("  [I2C2-CH%d] AK8963 scan: ret=%d, WIA=0x%02x (expect 0x48)\n",
                               ch, ak_ret, ak_who);

                    if (ak_ret == RT_EOK && ak_who == AK8963_WHO_AM_I)
                    {
                        if (ak8963_init(MPU6050_I2C2_BUS_NAME) == RT_EOK)
                        {
                            rt_kprintf("  [I2C2-CH%d] AK8963 magnetometer initialized\n", ch);
                            mpu_data_i2c2[ch].valid = RT_TRUE;
                            mpu_data_i2c2[ch].has_mag = RT_TRUE;
                            i2c2_active_count++;
                        }
                        else
                        {
                            rt_kprintf("  [I2C2-CH%d] AK8963 init failed, using 6-axis only\n", ch);
                            mpu_data_i2c2[ch].valid = RT_TRUE;
                            mpu_data_i2c2[ch].has_mag = RT_FALSE;
                            i2c2_active_count++;
                        }
                    }
                    else
                    {
                        rt_kprintf("  [I2C2-CH%d] AK8963 not found! Chip is likely MPU6500, not MPU9250\n", ch);
                        rt_kprintf("  [I2C2-CH%d] Using 6-axis mode (no magnetometer)\n", ch);
                        mpu_data_i2c2[ch].valid = RT_TRUE;
                        mpu_data_i2c2[ch].has_mag = RT_FALSE;
                        i2c2_active_count++;
                    }
                }
                else
                {
                    rt_kprintf("  [I2C2-CH%d] MPU init failed\n", ch);
                }
            }
            else
            {
                rt_kprintf("  [I2C2-CH%d] No device detected\n", ch);
            }
        }
        else
        {
            /* 其他通道使用mpu6050_getid检测 */
            if (mpu6050_getid(MPU6050_I2C2_BUS_NAME, &who) == RT_EOK && who == 0x68)
            {
                rt_kprintf("  [I2C2-CH%d] MPU6050 found! WHO_AM_I=0x%02x\n", ch, who);
                mpu_data_i2c2[ch].valid = RT_TRUE;
                i2c2_active_count++;
            }
            else
            {
                rt_kprintf("  [I2C2-CH%d] No MPU6050 detected\n", ch);
            }
        }
    }
    mpu_tca9548a_disable_all(MPU6050_I2C2_BUS_NAME);
    i2c2_mutex_release();

    /* ========== 初始化总结 ========== */
    rt_kprintf("\n========== Sensor Init Summary ==========\n");
    rt_kprintf("I2C1: %d/%d sensors active\n", i2c1_active_count, MPU6050_I2C1_COUNT);
    rt_kprintf("I2C2: %d/%d sensors active\n", i2c2_active_count, MPU6050_I2C2_COUNT);
    rt_kprintf("Total: %d/%d sensors active\n",
               i2c1_active_count + i2c2_active_count,
               MPU6050_I2C1_COUNT + MPU6050_I2C2_COUNT);

    if (i2c1_active_count == 0 && i2c2_active_count == 0)
    {
        rt_kprintf("\n[SENSOR] No sensors found! Thread exiting...\n");
        return;
    }

    rt_kprintf("\nWaiting for sensors to stabilize...\n");
    rt_thread_mdelay(1000);

    /* ========== 数据采集循环 ========== */
    rt_kprintf("\n========== Starting Data Acquisition ==========\n\n");

    const rt_uint32_t sample_interval = 20;  /* 20ms = 50Hz */

    while (1)
    {
        rt_uint32_t start_tick = rt_tick_get();

        /* 读取I2C1上的所有MPU6050 */
        MPU_SetBus(MPU6050_I2C1_BUS_NAME);
        for (int ch = 0; ch < MPU6050_I2C1_COUNT; ch++)
        {
            if (!mpu_data_i2c1[ch].valid)
                continue;

            if (mpu_tca9548a_select_channel(MPU6050_I2C1_BUS_NAME, ch) != RT_EOK)
                continue;

            MPU_Get_Accelerometer(&mpu_data_i2c1[ch].accel[0],
                                  &mpu_data_i2c1[ch].accel[1],
                                  &mpu_data_i2c1[ch].accel[2]);
            MPU_Get_Gyroscope(&mpu_data_i2c1[ch].gyro[0],
                              &mpu_data_i2c1[ch].gyro[1],
                              &mpu_data_i2c1[ch].gyro[2]);
        }

        /* 读取I2C2上的所有传感器 (需要互斥锁保护，与OLED共享总线) */
        if (i2c2_mutex_take(50) == RT_EOK)
        {
            MPU_SetBus(MPU6050_I2C2_BUS_NAME);
            for (int ch = 0; ch < MPU6050_I2C2_COUNT; ch++)
            {
                if (!mpu_data_i2c2[ch].valid)
                    continue;

                if (mpu_tca9548a_select_channel(MPU6050_I2C2_BUS_NAME, ch) != RT_EOK)
                    continue;

                /* 读取加速度计和陀螺仪 (MPU6050和MPU9250寄存器兼容) */
                MPU_Get_Accelerometer(&mpu_data_i2c2[ch].accel[0],
                                      &mpu_data_i2c2[ch].accel[1],
                                      &mpu_data_i2c2[ch].accel[2]);
                MPU_Get_Gyroscope(&mpu_data_i2c2[ch].gyro[0],
                                  &mpu_data_i2c2[ch].gyro[1],
                                  &mpu_data_i2c2[ch].gyro[2]);

                /* MPU9250: 额外读取磁力计 */
                if (mpu_data_i2c2[ch].has_mag)
                {
                    ak8963_read_mag(MPU6050_I2C2_BUS_NAME,
                                    &mpu_data_i2c2[ch].mag[0],
                                    &mpu_data_i2c2[ch].mag[1],
                                    &mpu_data_i2c2[ch].mag[2]);
                }
            }
            i2c2_mutex_release();
        }

        /* 串口数据记录输出 */
        if (data_log_enabled)
        {
            rt_uint32_t ts_ms = (rt_tick_get() - data_log_start_tick) * 1000 / RT_TICK_PER_SECOND;
            rt_kprintf("[DATA]%u,%s", ts_ms, DATA_LOG_HAND_TYPE);

            /* ch0~ch7: I2C1 MPU6050 (f_0_s1 ~ f_3_s2) */
            for (int i = 0; i < MPU6050_I2C1_COUNT; i++)
            {
                rt_kprintf(",%d,%d,%d,%d,%d,%d",
                    mpu_data_i2c1[i].accel[0], mpu_data_i2c1[i].accel[1], mpu_data_i2c1[i].accel[2],
                    mpu_data_i2c1[i].gyro[0], mpu_data_i2c1[i].gyro[1], mpu_data_i2c1[i].gyro[2]);
            }

            /* ch8~ch9: I2C2 MPU6050 (f_4_s1, f_4_s2) */
            for (int i = 0; i < MPU6050_I2C2_COUNT - 1; i++)
            {
                rt_kprintf(",%d,%d,%d,%d,%d,%d",
                    mpu_data_i2c2[i].accel[0], mpu_data_i2c2[i].accel[1], mpu_data_i2c2[i].accel[2],
                    mpu_data_i2c2[i].gyro[0], mpu_data_i2c2[i].gyro[1], mpu_data_i2c2[i].gyro[2]);
            }

            /* ch10: dorsal (MPU9250/MPU6500, 含mag) */
            {
                int d = MPU9250_I2C2_CHANNEL;
                rt_kprintf(",%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
                    mpu_data_i2c2[d].accel[0], mpu_data_i2c2[d].accel[1], mpu_data_i2c2[d].accel[2],
                    mpu_data_i2c2[d].gyro[0], mpu_data_i2c2[d].gyro[1], mpu_data_i2c2[d].gyro[2],
                    mpu_data_i2c2[d].mag[0], mpu_data_i2c2[d].mag[1], mpu_data_i2c2[d].mag[2]);
            }
        }

        /* 补偿延迟以维持10Hz采样率 */
        rt_uint32_t elapsed = rt_tick_get() - start_tick;
        if (elapsed < sample_interval)
        {
            rt_thread_mdelay(sample_interval - elapsed);
        }
    }
}

/* ========== 对外数据接口 ========== */

rt_err_t mpu_get_channel_data(int ch, mpu_channel_data_t *out)
{
    if (out == RT_NULL || ch < 0 || ch >= MPU_TOTAL_CHANNELS)
        return -RT_ERROR;

    rt_memset(out, 0, sizeof(mpu_channel_data_t));

    const mpu_sensor_data_t *src;

    if (ch < MPU6050_I2C1_COUNT)
    {
        /* ch0~ch7 → I2C1 数组 */
        src = &mpu_data_i2c1[ch];
    }
    else
    {
        /* ch8~ch10 → I2C2 数组 (索引 0~2) */
        src = &mpu_data_i2c2[ch - MPU6050_I2C1_COUNT];
    }

    out->valid   = src->valid;
    out->has_mag = src->has_mag;

    if (src->valid)
    {
        out->ax = src->accel[0];
        out->ay = src->accel[1];
        out->az = src->accel[2];
        out->gx = src->gyro[0];
        out->gy = src->gyro[1];
        out->gz = src->gyro[2];

        if (src->has_mag)
        {
            out->mx = src->mag[0];
            out->my = src->mag[1];
            out->mz = src->mag[2];
        }
    }

    return RT_EOK;
}

/* ========== 串口数据记录 MSH 命令 ========== */

static void data_log_print_header(void)
{
    static const char *ch_names[] = {
        "f_0_s1", "f_0_s2", "f_1_s1", "f_1_s2", "f_2_s1",
        "f_2_s2", "f_3_s1", "f_3_s2", "f_4_s1", "f_4_s2"
    };

    rt_kprintf("[DATA_HEAD]timestamp_ms,hand_type");
    for (int i = 0; i < 10; i++)
    {
        rt_kprintf(",%s_acc_x,%s_acc_y,%s_acc_z,%s_gyro_x,%s_gyro_y,%s_gyro_z",
            ch_names[i], ch_names[i], ch_names[i],
            ch_names[i], ch_names[i], ch_names[i]);
    }
    rt_kprintf(",dorsal_acc_x,dorsal_acc_y,dorsal_acc_z");
    rt_kprintf(",dorsal_gyro_x,dorsal_gyro_y,dorsal_gyro_z");
    rt_kprintf(",dorsal_mag_x,dorsal_mag_y,dorsal_mag_z\n");
}

static int data_start(int argc, char **argv)
{
    data_log_start_tick = rt_tick_get();
    data_log_print_header();
    data_log_enabled = RT_TRUE;
    rt_kprintf("[DATA_LOG] Started (%s hand, 50Hz)\n", DATA_LOG_HAND_TYPE);
    return 0;
}
MSH_CMD_EXPORT(data_start, Start serial data logging);

static int data_stop(int argc, char **argv)
{
    data_log_enabled = RT_FALSE;
    rt_kprintf("[DATA_LOG] Stopped\n");
    return 0;
}
MSH_CMD_EXPORT(data_stop, Stop serial data logging);
