/**
 * @file mpu6050.h
 * @brief MPU6050 6轴传感器驱动头文件
 * @note  通过RT-Thread I2C框架访问，支持动态切换I2C总线
 */

#ifndef MPU6050_H_
#define MPU6050_H_

#include <rtthread.h>
#include <rtdevice.h>
#include <stdint.h>

/* 简化别名 */
#ifndef u8
#define u8  uint8_t
#endif
#ifndef u16
#define u16 uint16_t
#endif
#ifndef u32
#define u32 uint32_t
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define MPU6050_I2C_ADDR        0x68

/* MPU6050寄存器定义 */
#define MPU_SELF_TESTX_REG      0X0D
#define MPU_SELF_TESTY_REG      0X0E
#define MPU_SELF_TESTZ_REG      0X0F
#define MPU_SELF_TESTA_REG      0X10
#define MPU_SAMPLE_RATE_REG     0X19
#define MPU_CFG_REG             0X1A
#define MPU_GYRO_CFG_REG        0X1B
#define MPU_ACCEL_CFG_REG       0X1C
#define MPU_MOTION_DET_REG      0X1F
#define MPU_FIFO_EN_REG         0X23
#define MPU_I2CMST_CTRL_REG     0X24
#define MPU_INTBP_CFG_REG       0X37
#define MPU_INT_EN_REG          0X38
#define MPU_INT_STA_REG         0X3A

#define MPU_ACCEL_XOUTH_REG     0X3B
#define MPU_ACCEL_XOUTL_REG     0X3C
#define MPU_ACCEL_YOUTH_REG     0X3D
#define MPU_ACCEL_YOUTL_REG     0X3E
#define MPU_ACCEL_ZOUTH_REG     0X3F
#define MPU_ACCEL_ZOUTL_REG     0X40

#define MPU_TEMP_OUTH_REG       0X41
#define MPU_TEMP_OUTL_REG       0X42

#define MPU_GYRO_XOUTH_REG      0X43
#define MPU_GYRO_XOUTL_REG      0X44
#define MPU_GYRO_YOUTH_REG      0X45
#define MPU_GYRO_YOUTL_REG      0X46
#define MPU_GYRO_ZOUTH_REG      0X47
#define MPU_GYRO_ZOUTL_REG      0X48

#define MPU_USER_CTRL_REG       0X6A
#define MPU_PWR_MGMT1_REG       0X6B
#define MPU_PWR_MGMT2_REG       0X6C
#define MPU_FIFO_CNTH_REG       0X72
#define MPU_FIFO_CNTL_REG       0X73
#define MPU_FIFO_RW_REG         0X74
#define MPU_DEVICE_ID_REG       0X75

/* 设置使用的I2C总线名 (默认"i2c1") */
void MPU_SetBus(const char *bus_name);

/* 初始化MPU6050 */
u8 mpu6050_init(void);

/* 检测MPU6050并返回设备ID */
rt_err_t mpu6050_getid(const char *i2c_bus_name, rt_uint8_t *who_am_i);

/* 配置函数 */
u8 MPU_Set_Gyro_Fsr(u8 fsr);
u8 MPU_Set_Accel_Fsr(u8 fsr);
u8 MPU_Set_LPF(u16 lpf);
u8 MPU_Set_Rate(u16 rate);

/* 数据读取 */
short MPU_Get_Temperature(void);
u8 MPU_Get_Gyroscope(short *gx, short *gy, short *gz);
u8 MPU_Get_Accelerometer(short *ax, short *ay, short *az);

/* 底层读写 */
uint8_t MPU_Write_Len(uint8_t reg, uint8_t len, uint8_t *buf);
uint8_t MPU_Read_Len(uint8_t reg, uint8_t len, uint8_t *buf);
uint8_t MPU_Write_Byte(uint8_t reg, uint8_t data);
uint8_t MPU_Read_Byte(uint8_t reg);

#ifdef __cplusplus
}
#endif

#endif /* MPU6050_H_ */
