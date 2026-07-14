/*
 * imu_ble_service.c — BLE GATT service for IMU sensor data (Right Hand)
 *
 * Service UUID:    A74D0001-B4E7-4C5F-9D2A-F163E80ACB00
 * Notify chr UUID: A74D0002-...  (Read + Notify)  — CSV text format IMU data
 * Channel chr UUID:A74D0003-...  (Read + Write)   — 1-byte channel select
 * Text chr UUID:   A74D0004-...  (Write)          — UTF-8 text (SAY:text → VTX316)
 */

#include <rtthread.h>
#include <string.h>
#include "imu_ble_service.h"
#include "operation_mode.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "os/os_mbuf.h"

#include "../mpu6050/mpu6050_thread.h"

#define DBG_SECTION_NAME  "IMU_SVC"
#define DBG_LEVEL          DBG_INFO
#include <rtdbg.h>

/* ── 128-bit UUIDs (LSB first) ────────────────────────────────────────────── */

static const ble_uuid128_t _svc_uuid =
    BLE_UUID128_INIT(0x00, 0xCB, 0x0A, 0xE8, 0x63, 0xF1, 0x2A, 0x9D,
                     0x5F, 0x4C, 0xE7, 0xB4, 0x01, 0x00, 0x4D, 0xA7);

static const ble_uuid128_t _notify_chr_uuid =
    BLE_UUID128_INIT(0x00, 0xCB, 0x0A, 0xE8, 0x63, 0xF1, 0x2A, 0x9D,
                     0x5F, 0x4C, 0xE7, 0xB4, 0x02, 0x00, 0x4D, 0xA7);

static const ble_uuid128_t _channel_chr_uuid =
    BLE_UUID128_INIT(0x00, 0xCB, 0x0A, 0xE8, 0x63, 0xF1, 0x2A, 0x9D,
                     0x5F, 0x4C, 0xE7, 0xB4, 0x03, 0x00, 0x4D, 0xA7);

static const ble_uuid128_t _text_chr_uuid =
    BLE_UUID128_INIT(0x00, 0xCB, 0x0A, 0xE8, 0x63, 0xF1, 0x2A, 0x9D,
                     0x5F, 0x4C, 0xE7, 0xB4, 0x04, 0x00, 0x4D, 0xA7);

/* ── State ────────────────────────────────────────────────────────────────── */
static uint16_t        _notify_handle = 0;
static volatile int    _channel       = 0;
static imu_ble_text_cb_t _text_cb     = RT_NULL;

/* ── Characteristic access callbacks ─────────────────────────────────────── */

static int _notify_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR)
        return BLE_ATT_ERR_UNLIKELY;

    /* Build CSV text format: [DATA]timestamp,hand_type,ch0-9(6-axis),ch10(9-axis) */
    char csv_buf[600];
    uint32_t timestamp_ms = (uint32_t)(rt_tick_get() * 1000u / RT_TICK_PER_SECOND);
    int pos = 0;

    /* Start with [DATA] prefix and timestamp */
    pos += rt_snprintf(csv_buf + pos, sizeof(csv_buf) - pos,
                      "[DATA]%u,right,0", timestamp_ms);

    /* ch0–ch9: MPU6050 sensors (6-axis each) */
    for (int i = 0; i < 10; i++) {
        mpu_channel_data_t d;
        if (mpu_get_channel_raw_data(i, &d) == RT_EOK && d.valid) {
            pos += rt_snprintf(csv_buf + pos, sizeof(csv_buf) - pos,
                              ",%d,%d,%d,%d,%d,%d",
                              d.ax, d.ay, d.az, d.gx, d.gy, d.gz);
        } else {
            pos += rt_snprintf(csv_buf + pos, sizeof(csv_buf) - pos,
                              ",0,0,0,0,0,0");
        }
    }

    /* ch10: dorsal ICM-20948 (9-axis with magnetometer) */
    {
        mpu_channel_data_t d;
        if (mpu_get_channel_raw_data(10, &d) == RT_EOK && d.valid) {
            pos += rt_snprintf(csv_buf + pos, sizeof(csv_buf) - pos,
                              ",%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
                              d.ax, d.ay, d.az, d.gx, d.gy, d.gz,
                              d.mx, d.my, d.mz);
        } else {
            pos += rt_snprintf(csv_buf + pos, sizeof(csv_buf) - pos,
                              ",0,0,0,0,0,0,0,0,0\n");
        }
    }

    return os_mbuf_append(ctxt->om, csv_buf, pos);
}

static int _channel_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (OS_MBUF_PKTLEN(ctxt->om) != 1)
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        uint8_t ch = 0;
        os_mbuf_copydata(ctxt->om, 0, 1, &ch);
        if (ch > 10)
            return 0x13;
        _channel = (int)ch;
        LOG_I("channel set to %d", _channel);
        return 0;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t ch = (uint8_t)_channel;
        return os_mbuf_append(ctxt->om, &ch, 1);
    }

    return BLE_ATT_ERR_UNLIKELY;
}

/*
 * Text receive characteristic — accepts UTF-8 text up to 256 bytes.
 * Forwards to registered callback (typically vtx316_tcp_recv_handler).
 */
static int _text_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
        return BLE_ATT_ERR_UNLIKELY;

    uint16_t pkt_len = OS_MBUF_PKTLEN(ctxt->om);
    if (pkt_len == 0 || pkt_len > 256)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    char buf[257];
    os_mbuf_copydata(ctxt->om, 0, pkt_len, buf);
    buf[pkt_len] = '\0';

    LOG_I("text recv[%d]: %s", pkt_len, buf);

    /* CMD: prefix → operation mode command; do NOT forward to speech/translated-text path */
    if (strncmp(buf, "CMD:", 4) == 0) {
        if (operation_mode_handle_cmd(buf) != RT_TRUE) {
            LOG_W("unknown operation command ignored: %s", buf);
        }
        return 0;
    }

    if (_text_cb != RT_NULL) {
        _text_cb(buf, (int)pkt_len);
    }

    return 0;
}

/* ── GATT service table ───────────────────────────────────────────────────── */

static const struct ble_gatt_svc_def _gatt_svcs[] = {
    {
        .type     = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid     = &_svc_uuid.u,
        .includes = NULL,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                /* IMU data: readable and notifiable */
                .uuid        = &_notify_chr_uuid.u,
                .access_cb   = _notify_chr_access,
                .arg         = NULL,
                .descriptors = NULL,
                .flags       = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle  = &_notify_handle,
            },
            {
                /* Channel select: readable and writable */
                .uuid        = &_channel_chr_uuid.u,
                .access_cb   = _channel_chr_access,
                .arg         = NULL,
                .descriptors = NULL,
                .flags       = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
                .val_handle  = NULL,
            },
            {
                /* Text receive: writable (SAY:text → VTX316) */
                .uuid        = &_text_chr_uuid.u,
                .access_cb   = _text_chr_access,
                .arg         = NULL,
                .descriptors = NULL,
                .flags       = BLE_GATT_CHR_F_WRITE,
                .val_handle  = NULL,
            },
            { 0 }  /* sentinel */
        },
    },
    { 0 }  /* sentinel */
};

/* ── Public API ───────────────────────────────────────────────────────────── */

rt_err_t imu_ble_service_init(void)
{
    int rc;

    rc = ble_gatts_count_cfg(_gatt_svcs);
    if (rc != 0) {
        LOG_E("ble_gatts_count_cfg failed: %d", rc);
        return -RT_ERROR;
    }

    rc = ble_gatts_add_svcs(_gatt_svcs);
    if (rc != 0) {
        LOG_E("ble_gatts_add_svcs failed: %d", rc);
        return -RT_ERROR;
    }

    LOG_I("IMU GATT service registered (with text chr)");
    return RT_EOK;
}

uint16_t imu_ble_notify_handle(void)
{
    return _notify_handle;
}

int imu_ble_service_get_channel(void)
{
    return _channel;
}

void imu_ble_service_set_conn_handle(int32_t conn_handle)
{
    (void)conn_handle;
}

void imu_ble_text_set_callback(imu_ble_text_cb_t cb)
{
    _text_cb = cb;
    LOG_I("text callback %s", cb ? "registered" : "cleared");
}
