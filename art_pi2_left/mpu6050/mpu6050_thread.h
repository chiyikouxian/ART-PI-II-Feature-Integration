/**
 * @file mpu6050_thread.h
 * @brief MPU6050数据采集线程头文件
 * @note  支持双TCA9548A I2C复用模块，读取多路MPU6050原始数据
 *        I2C1 (PB8/PB9): TCA9548A #1
 *        I2C2 (PE1/PE2): TCA9548A #2 (与OLED共享)
 */

#ifndef __MPU6050_THREAD_H
#define __MPU6050_THREAD_H

#include <rtthread.h>

/* 线程配置 */
#define MPU6050_THREAD_PRIORITY     16
#define MPU6050_THREAD_STACK_SIZE   4096
#define MPU6050_THREAD_TIMESLICE    10

/* I2C总线配置 */
#define MPU6050_I2C1_BUS_NAME    "i2c1"    /* PB8-SCL, PB9-SDA */
#define MPU6050_I2C2_BUS_NAME    "i2c2"    /* PE1-SCL, PE2-SDA */

/* TCA9548A I2C多路复用器地址 */
#define MPU_TCA9548A_ADDR        0x70

/* MPU6050传感器数量配置 */
#define MPU6050_I2C1_COUNT       8    /* I2C1上最大MPU6050数量 */
#define MPU6050_I2C2_COUNT       3    /* I2C2上最大传感器数量 */

/* I2C2通道分配:
 *   CH0, CH1: MPU6050 (WHO_AM_I=0x68)
 *   CH2:      MPU9250 (WHO_AM_I=0x71), 含AK8963磁力计
 *   CH3:      OLED (SH1106)
 */
#define MPU9250_I2C2_CHANNEL     2
#define MPU9250_ADDR             0x68
#define MPU9250_WHO_AM_I         0x71

/* AK8963磁力计 */
#define AK8963_ADDR              0x0C
#define AK8963_WHO_AM_I          0x48
#define AK8963_WIA               0x00
#define AK8963_ST1               0x02
#define AK8963_HXL               0x03
#define AK8963_ST2               0x09
#define AK8963_CNTL1             0x0A
#define AK8963_CNTL2             0x0B

/* ========== 对外数据接口 (供tcp_client等模块读取) ========== */

/* 总通道数: I2C1(8) + I2C2(3) = 11 */
#define MPU_TOTAL_CHANNELS   (MPU6050_I2C1_COUNT + MPU6050_I2C2_COUNT)

/* 单通道传感器数据 */
typedef struct {
    short ax, ay, az;       /* 加速度计原始值 */
    short gx, gy, gz;       /* 陀螺仪原始值 */
    short mx, my, mz;       /* 磁力计原始值 (仅ch10有效) */
    rt_bool_t valid;        /* 传感器是否检测到 */
    rt_bool_t has_mag;      /* 是否有磁力计 */
} mpu_channel_data_t;

/**
 * @brief  获取指定通道的传感器数据
 * @param  ch 通道号 0~10
 *         ch0~ch7:  I2C1 TCA CH0~CH7 (MPU6050, 6-DOF)
 *         ch8~ch9:  I2C2 TCA CH0~CH1 (MPU6050, 6-DOF)
 *         ch10:     I2C2 TCA CH2     (MPU9250, 9-DOF)
 * @param  out 输出数据
 * @return RT_EOK 成功, -RT_ERROR 通道号无效
 */
rt_err_t mpu_get_channel_data(int ch, mpu_channel_data_t *out);

/* 函数声明 */
void mpu6050_thread_entry(void *parameter);

#endif /* __MPU6050_THREAD_H */
