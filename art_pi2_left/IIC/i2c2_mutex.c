/**
 * @file i2c2_mutex.c
 * @brief I2C2总线共享互斥锁实现
 */

#include <rtthread.h>
#include "i2c2_mutex.h"

static struct rt_mutex i2c2_bus_mutex;

void i2c2_mutex_init(void)
{
    rt_mutex_init(&i2c2_bus_mutex, "i2c2_mtx", RT_IPC_FLAG_PRIO);
}

rt_err_t i2c2_mutex_take(rt_int32_t timeout_ms)
{
    return rt_mutex_take(&i2c2_bus_mutex, rt_tick_from_millisecond(timeout_ms));
}

rt_err_t i2c2_mutex_release(void)
{
    return rt_mutex_release(&i2c2_bus_mutex);
}
