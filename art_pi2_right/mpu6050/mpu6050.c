/**
 * @file mpu6050.c
 * @brief MPU6050 6轴传感器驱动
 * @note  通过RT-Thread I2C框架访问，支持动态切换I2C总线
 *        移植自Demo工程，去除DMP依赖
 */

#include <board.h>
#include "mpu6050.h"
#include <math.h>

static const char *s_mpu_bus = "i2c1";

void MPU_SetBus(const char *bus_name)
{
    s_mpu_bus = (bus_name && bus_name[0]) ? bus_name : "i2c1";
}

static struct rt_i2c_bus_device *mpu_bus(void)
{
    return (struct rt_i2c_bus_device *)rt_device_find(s_mpu_bus);
}

uint8_t MPU_Write_Len(uint8_t reg, uint8_t len, uint8_t *buf)
{
    struct rt_i2c_bus_device *bus = mpu_bus();
    if (!bus) return 1;
    uint8_t tmp[1 + 32];
    if (len > 32) len = 32;
    tmp[0] = reg;
    for (uint8_t i = 0; i < len; i++) tmp[1 + i] = buf[i];
    return (rt_i2c_master_send(bus, MPU6050_I2C_ADDR, 0, tmp, 1 + len) == (rt_size_t)(1 + len)) ? 0 : 1;
}

uint8_t MPU_Read_Len(uint8_t reg, uint8_t len, uint8_t *buf)
{
    struct rt_i2c_bus_device *bus = mpu_bus();
    if (!bus) return 1;
    struct rt_i2c_msg msgs[2];
    msgs[0].addr = MPU6050_I2C_ADDR; msgs[0].flags = RT_I2C_WR; msgs[0].buf = &reg; msgs[0].len = 1;
    msgs[1].addr = MPU6050_I2C_ADDR; msgs[1].flags = RT_I2C_RD; msgs[1].buf = buf;  msgs[1].len = len;
    return (rt_i2c_transfer(bus, msgs, 2) == 2) ? 0 : 1;
}

uint8_t MPU_Write_Byte(uint8_t reg, uint8_t data)
{
    return MPU_Write_Len(reg, 1, &data);
}

uint8_t MPU_Read_Byte(uint8_t reg)
{
    uint8_t d = 0;
    MPU_Read_Len(reg, 1, &d);
    return d;
}

u8 MPU_Set_Gyro_Fsr(u8 fsr)
{
    return (MPU_Write_Byte(MPU_GYRO_CFG_REG, fsr << 3) == 0) ? 0 : 1;
}

u8 MPU_Set_Accel_Fsr(u8 fsr)
{
    return (MPU_Write_Byte(MPU_ACCEL_CFG_REG, fsr << 3) == 0) ? 0 : 1;
}

u8 MPU_Set_LPF(u16 lpf)
{
    u8 data = 0;
    if (lpf >= 188) data = 1;
    else if (lpf >= 98) data = 2;
    else if (lpf >= 42) data = 3;
    else if (lpf >= 20) data = 4;
    else if (lpf >= 10) data = 5;
    else data = 6;
    return (MPU_Write_Byte(MPU_CFG_REG, data) == 0) ? 0 : 1;
}

u8 MPU_Set_Rate(u16 rate)
{
    if (rate > 1000) rate = 1000;
    if (rate < 4) rate = 4;
    u8 div = 1000 / rate - 1;
    if (MPU_Write_Byte(MPU_SAMPLE_RATE_REG, div) != 0) return 1;
    return MPU_Set_LPF(rate / 2);
}

short MPU_Get_Temperature(void)
{
    u8 buf[2];
    short raw;
    float temp;
    MPU_Read_Len(MPU_TEMP_OUTH_REG, 2, buf);
    raw = ((u16)buf[0] << 8) | buf[1];
    temp = 36.53f + ((float)raw) / 340.0f;
    return (short)(temp * 100);
}

u8 MPU_Get_Gyroscope(short *gx, short *gy, short *gz)
{
    u8 buf[6];
    if (MPU_Read_Len(MPU_GYRO_XOUTH_REG, 6, buf) != 0) return 1;
    *gx = ((u16)buf[0] << 8) | buf[1];
    *gy = ((u16)buf[2] << 8) | buf[3];
    *gz = ((u16)buf[4] << 8) | buf[5];
    return 0;
}

u8 MPU_Get_Accelerometer(short *ax, short *ay, short *az)
{
    u8 buf[6];
    if (MPU_Read_Len(MPU_ACCEL_XOUTH_REG, 6, buf) != 0) return 1;
    *ax = ((u16)buf[0] << 8) | buf[1];
    *ay = ((u16)buf[2] << 8) | buf[3];
    *az = ((u16)buf[4] << 8) | buf[5];
    return 0;
}

u8 mpu6050_init(void)
{
    u8 id;

    MPU_Write_Byte(MPU_PWR_MGMT1_REG, 0x80);  /* 复位MPU6050 */
    rt_thread_mdelay(100);
    MPU_Write_Byte(MPU_PWR_MGMT1_REG, 0x00);   /* 唤醒MPU6050 */
    MPU_Set_Gyro_Fsr(3);                        /* 陀螺仪 ±2000dps */
    MPU_Set_Accel_Fsr(0);                       /* 加速度计 ±2g */
    MPU_Set_Rate(50);                            /* 采样率50Hz */
    MPU_Write_Byte(MPU_INT_EN_REG, 0x00);       /* 关闭所有中断 */
    MPU_Write_Byte(MPU_USER_CTRL_REG, 0x00);    /* 关闭I2C主模式 */
    MPU_Write_Byte(MPU_FIFO_EN_REG, 0x00);      /* 关闭FIFO */
    MPU_Write_Byte(MPU_INTBP_CFG_REG, 0x80);    /* INT引脚低电平有效 */
    id = MPU_Read_Byte(MPU_DEVICE_ID_REG);
    if (id != MPU6050_I2C_ADDR) return 1;
    MPU_Write_Byte(MPU_PWR_MGMT1_REG, 0x01);   /* CLKSEL: PLL X轴参考 */
    MPU_Write_Byte(MPU_PWR_MGMT2_REG, 0x00);   /* 加速度与陀螺仪都工作 */
    MPU_Set_Rate(50);                            /* 采样率50Hz */
    return 0;
}

rt_err_t mpu6050_getid(const char *i2c_bus_name, rt_uint8_t *who_am_i)
{
    MPU_SetBus(i2c_bus_name);

    if (mpu6050_init() != 0)
    {
        return -RT_ERROR;
    }

    if (who_am_i)
    {
        *who_am_i = MPU_Read_Byte(MPU_DEVICE_ID_REG);
    }

    return RT_EOK;
}
