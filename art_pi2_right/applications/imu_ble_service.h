/*
 * imu_ble_service.h — BLE GATT service for IMU sensor data (Right Hand)
 *
 * Custom service UUID: A74D0001-B4E7-4C5F-9D2A-F163E80ACB00
 *
 * Characteristics:
 *   A74D0002 (Read + Notify): CSV text format IMU data (all 11 channels)
 *   A74D0003 (Read + Write) : 1-byte channel select (reserved, legacy)
 *   A74D0004 (Write)        : UTF-8 text receive (SAY:text → VTX316)
 */

#ifndef IMU_BLE_SERVICE_H
#define IMU_BLE_SERVICE_H

#include <rtthread.h>
#include "host/ble_hs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  CSV text format for IMU notification — matches data_start serial format.
 *
 * Format: [DATA]<timestamp_ms>,<hand_type>,<ch0-9 6-axis data>,<ch10 9-axis data>
 *
 * Example:
 *   [DATA]12345,right,1024,-512,16384,10,-5,2,...,-100,50,200
 *
 * Fields:
 *   - timestamp_ms: milliseconds since boot
 *   - hand_type: "right" or "left"
 *   - ch0-ch9: 10 channels × 6 values (ax,ay,az,gx,gy,gz) = 60 values
 *   - ch10: 1 channel × 9 values (ax,ay,az,gx,gy,gz,mx,my,mz) = 9 values
 *   - Total: 71 comma-separated values per line
 *
 * Typical size: ~200-250 bytes per notification (text format)
 * No special MTU requirement - standard BLE MTU (23+ bytes) is sufficient
 * as notifications can be fragmented automatically by the BLE stack.
 */
typedef struct __attribute__((packed)) {
    uint32_t timestamp_ms;     /* ms since boot                          */
    uint8_t  hand_type;        /* 0x01 = right hand                      */
    int16_t  sensor[10][6];    /* ch0-ch9: [ax,ay,az,gx,gy,gz]          */
    int16_t  dorsal[9];        /* ch10:    [ax,ay,az,gx,gy,gz,mx,my,mz] */
    uint16_t valid_mask;       /* bit N = channel N valid (bits 0–10)    */
} imu_full_payload_t;          /* DEPRECATED: kept for compatibility     */

/**
 * @brief  Text receive callback type.
 * @param  data  received data (e.g. "SAY:hello")
 * @param  len   data length
 */
typedef void (*imu_ble_text_cb_t)(const char *data, int len);

/**
 * @brief  Register the IMU GATT service with NimBLE.
 *         Must be called after ble_svc_gatt_init() and before
 *         nimble_port_run() fires the sync callback.
 */
rt_err_t imu_ble_service_init(void);

/**
 * @brief  Return the GATT attribute value handle for the notify characteristic.
 *         Valid only after imu_ble_service_init() has been called.
 *         Used by imu_notify_thread to send notifications.
 */
uint16_t imu_ble_notify_handle(void);

/**
 * @brief  Update the active BLE connection handle.
 *         Pass a valid handle on connect, -1 on disconnect.
 */
void imu_ble_service_set_conn_handle(int32_t conn_handle);

/**
 * @brief  Return the currently selected IMU channel (0–10).
 *         Updated when the phone writes to the channel-select characteristic.
 */
int imu_ble_service_get_channel(void);

/**
 * @brief  Register callback for text received via BLE (A74D0004 characteristic).
 */
void imu_ble_text_set_callback(imu_ble_text_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* IMU_BLE_SERVICE_H */
