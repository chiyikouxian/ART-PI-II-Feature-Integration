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
 *         The thread sends BLE notifications at NOTIFY_INTERVAL_MS intervals
 *         whenever a BLE client is connected and has enabled notifications.
 *
 *         Must be called after imu_ble_service_init() (so the notify handle
 *         is valid) and after nimble_port_rtthread_init() (so the NimBLE host
 *         stack is running).
 */
rt_err_t imu_notify_thread_start(void);

/**
 * @brief  Update the active BLE connection handle.
 *         Call with a valid handle on BLE_GAP_EVENT_CONNECT,
 *         and with -1 on BLE_GAP_EVENT_DISCONNECT.
 *         Thread-safe; may be called from any context.
 */
void imu_notify_set_conn_handle(int32_t conn_handle);

/**
 * @brief  Tell the notify thread whether the client has subscribed to
 *         notifications (CCCD = 0x0001) or unsubscribed (CCCD = 0x0000).
 *         Call from BLE_GAP_EVENT_SUBSCRIBE with cur_notify != 0 for true,
 *         and from BLE_GAP_EVENT_DISCONNECT with RT_FALSE.
 *         Thread-safe; may be called from any context.
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
