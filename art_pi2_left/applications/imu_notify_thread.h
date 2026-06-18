/*
 * imu_notify_thread.h — periodic BLE notification sender for IMU data
 */

#ifndef IMU_NOTIFY_THREAD_H
#define IMU_NOTIFY_THREAD_H

#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Start the IMU notification thread.
 *         Thread sends 145-byte imu_full_payload_t BLE notifications at 10 Hz.
 */
rt_err_t imu_notify_thread_start(void);

/**
 * @brief  Update the active BLE connection handle.
 *         Pass a valid handle on connect, -1 on disconnect.
 */
void imu_notify_set_conn_handle(int32_t conn_handle);

/**
 * @brief  Update the CCCD notification subscription state.
 *         Called from GAP event handler when client subscribes/unsubscribes.
 */
void imu_notify_set_subscribed(rt_bool_t subscribed);

/**
 * @brief  Atomically reset the BLE frame_seq counter to 0.
 *         Called by operation_mode_handle_cmd() on CMD:RESET_SEQ / CMD:START / CMD:STOP.
 */
void imu_notify_reset_frame_seq(void);

#ifdef __cplusplus
}
#endif

#endif /* IMU_NOTIFY_THREAD_H */
