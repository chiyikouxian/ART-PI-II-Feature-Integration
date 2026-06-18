/*
 * ble_app.c — NimBLE peripheral application for ART-Pi2 IMU BLE service (Left Hand)
 *
 * Device name: "ART-Pi2-IMU-L"
 * Advertising interval: 200–500 ms (connectable undirected, legacy PDU)
 *
 * Additional feature vs right hand:
 *   - Registers VTX316 text-to-speech callback on BLE text characteristic (A74D0004)
 */

#include <rtthread.h>
#include <rtdevice.h>
#include "cyw43438_bt.h"
#include "nimble_hci_adapter.h"
#include "imu_ble_service.h"
#include "imu_notify_thread.h"
#include "tcp_client.h"

#include "nimble/nimble_port.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

/* VTX316 recv handler reused for BLE text characteristic */
#include "../vtx316/vtx316.h"
#include "operation_mode.h"

extern void ble_hs_thread_startup(void);

#define DBG_SECTION_NAME  "BLE_APP"
#define DBG_LEVEL          DBG_INFO
#include <rtdbg.h>

#define DEVICE_NAME  "ART-Pi2-IMU-L"

#ifndef BLE_GAP_ADV_ITVL_MS
#define BLE_GAP_ADV_ITVL_MS(x)  ((x) * 1000 / 625)
#endif

static uint8_t _adv_addr_type;
static volatile rt_bool_t g_left_ble_init_ok = RT_FALSE;

static int _gap_event(struct ble_gap_event *event, void *arg);

static int _normalize_ble_text_payload(const char *data, int len, char *out, int out_size)
{
    const char *start = data;
    int text_len = len;
    int copy_len;

    if (data == RT_NULL || out == RT_NULL || out_size <= 1 || len <= 0)
        return 0;

    if (text_len >= 3 &&
        (start[0] == 'S' || start[0] == 's') &&
        (start[1] == 'A' || start[1] == 'a') &&
        (start[2] == 'Y' || start[2] == 'y'))
    {
        start += 3;
        text_len -= 3;

        while (text_len > 0)
        {
            if (*start == ':' || *start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
            {
                start++;
                text_len--;
                continue;
            }

            if (text_len >= 3 &&
                (rt_uint8_t)start[0] == 0xEF &&
                (rt_uint8_t)start[1] == 0xBC &&
                (rt_uint8_t)start[2] == 0x9A)
            {
                start += 3;
                text_len -= 3;
                continue;
            }

            if (text_len >= 2 &&
                (rt_uint8_t)start[0] == 0xA3 &&
                (rt_uint8_t)start[1] == 0xBA)
            {
                start += 2;
                text_len -= 2;
                continue;
            }

            break;
        }
    }

    while (text_len > 0 &&
           (start[text_len - 1] == ' ' || start[text_len - 1] == '\t' ||
            start[text_len - 1] == '\r' || start[text_len - 1] == '\n'))
    {
        text_len--;
    }

    if (text_len <= 0)
        return 0;

    copy_len = text_len;
    if (copy_len > out_size - 1)
        copy_len = out_size - 1;

    rt_memcpy(out, start, copy_len);
    out[copy_len] = '\0';
    return copy_len;
}

static void _ble_text_recv_handler(const char *data, int len)
{
    char normalized_text[257];
    int normalized_len = _normalize_ble_text_payload(data, len, normalized_text, sizeof(normalized_text));

    /*
     * Current compatibility-prep assumption for task 2.4:
     * - one BLE write carries one complete sentence
     * - payload is plain UTF-8 text
     * - no fragmentation / reassembly is performed here
     * - local normalization only strips lightweight command-style prefixes and separators
     */
    LOG_I("BLE text raw[%d] -> normalized processing", len);

    if (normalized_len <= 0)
    {
        LOG_W("BLE text dropped after normalization");
        return;
    }

    LOG_I("BLE text normalized[%d]: %s", normalized_len, normalized_text);
    vtx316_speak_wait(normalized_text);
    tcp_set_translated_text(normalized_text, normalized_len);
}

/* ── Start / restart BLE advertising ─────────────────────────────────────── */
static void _start_advertising(void)
{
    struct ble_hs_adv_fields  fields;
    struct ble_gap_adv_params adv_params;
    int rc;

    rt_memset(&fields,     0, sizeof(fields));
    rt_memset(&adv_params, 0, sizeof(adv_params));

    fields.flags                = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name                 = (uint8_t *)DEVICE_NAME;
    fields.name_len             = (uint8_t)strlen(DEVICE_NAME);
    fields.name_is_complete     = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        LOG_E("adv set fields failed: %d", rc);
        g_left_ble_init_ok = RT_FALSE;
        return;
    }

    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min  = BLE_GAP_ADV_ITVL_MS(200);
    adv_params.itvl_max  = BLE_GAP_ADV_ITVL_MS(500);

    rc = ble_gap_adv_start(_adv_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, _gap_event, NULL);
    if (rc != 0) {
        LOG_E("adv start failed: %d", rc);
        g_left_ble_init_ok = RT_FALSE;
        return;
    }

    LOG_I("advertising started as \"%s\"", DEVICE_NAME);
}

/* ── GAP event handler ────────────────────────────────────────────────────── */
static int _gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            LOG_I("connected (handle=%d)", event->connect.conn_handle);
            imu_notify_set_conn_handle((int32_t)event->connect.conn_handle);
            imu_ble_service_set_conn_handle((int32_t)event->connect.conn_handle);
        } else {
            LOG_W("connect failed: %d", event->connect.status);
            imu_notify_set_conn_handle(-1);
            imu_ble_service_set_conn_handle(-1);
            _start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        LOG_I("disconnected (reason=0x%02X) — re-advertising",
              event->disconnect.reason);
        imu_notify_set_subscribed(RT_FALSE);
        imu_notify_set_conn_handle(-1);
        imu_ble_service_set_conn_handle(-1);
        _start_advertising();
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        _start_advertising();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        LOG_I("subscribe attr_handle=%d notify=%d",
              event->subscribe.attr_handle,
              event->subscribe.cur_notify);
        imu_notify_set_subscribed(event->subscribe.cur_notify != 0 ? RT_TRUE : RT_FALSE);
        return 0;

    case BLE_GAP_EVENT_MTU:
        LOG_D("MTU update: conn=%d cid=%d mtu=%d",
              event->mtu.conn_handle,
              event->mtu.channel_id,
              event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

/* ── NimBLE host sync callback ────────────────────────────────────────────── */
static void _on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &_adv_addr_type);
    if (rc != 0) {
        LOG_E("ble_hs_id_infer_auto failed: %d", rc);
        g_left_ble_init_ok = RT_FALSE;
        return;
    }
    LOG_I("NimBLE host synced with CYW43438 controller");

    /* Print the real BLE address so it is visible on the serial console */
    uint8_t addr[6];
    ble_hs_id_copy_addr(_adv_addr_type, addr, NULL);
    LOG_I("BLE address: %02X:%02X:%02X:%02X:%02X:%02X (type=%s)",
          addr[5], addr[4], addr[3], addr[2], addr[1], addr[0],
          _adv_addr_type == 0 ? "public" : "random");

    _start_advertising();
}

static void _on_reset(int reason)
{
    LOG_E("NimBLE host reset (reason=%d)", reason);
}

/* ── Application init ─────────────────────────────────────────────────────── */
/**
 * @brief   BLE应用初始化函数（手动调用，在WiFi连接成功后执行）
 * @details 初始化NimBLE协议栈、注册GATT服务、启动广播
 * @return  RT_EOK 成功，其他值失败
 */
int left_ble_app_init(void)
{
    rt_err_t ret;

    LOG_I("Starting BLE initialization after WiFi connected...");

    /* 0. Initialize operation mode state machine (boot default: AUTO/AUTO_STANDBY) */
    operation_mode_init();

    /* 1. Wait for CYW43438 BT core ready */
    int waited = 0;
    while (!cyw43438_bt_is_ready() && waited < 100) {
        rt_thread_mdelay(50);
        waited++;
    }
    if (!cyw43438_bt_is_ready()) {
        LOG_E("CYW43438 BT not ready — NimBLE init aborted");
        g_left_ble_init_ok = RT_FALSE;
        return -RT_ETIMEOUT;
    }

    /* 2. Install HCI transport shim */
    ret = nimble_hci_adapter_init();
    if (ret != RT_EOK) {
        LOG_E("nimble_hci_adapter_init failed: %d", ret);
        g_left_ble_init_ok = RT_FALSE;
        return ret;
    }

    /* 3. Configure host callbacks */
    ble_hs_cfg.sync_cb  = _on_sync;
    ble_hs_cfg.reset_cb = _on_reset;

    /* 4. Register standard services */
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(DEVICE_NAME);

    /* 5. Register custom IMU GATT service (with text characteristic) */
    ret = imu_ble_service_init();
    if (ret != RT_EOK) {
        LOG_E("imu_ble_service_init failed: %d", ret);
        g_left_ble_init_ok = RT_FALSE;
        return ret;
    }

    /* 6. Register VTX316 as text receive callback (SAY:text → speech) */
    imu_ble_text_set_callback(_ble_text_recv_handler);

    /* 7. Start NimBLE host thread */
    ble_hs_thread_startup();

    /* 8. Start IMU notification thread */
    ret = imu_notify_thread_start();
    if (ret != RT_EOK) {
        LOG_E("imu_notify_thread_start failed: %d", ret);
        g_left_ble_init_ok = RT_FALSE;
        return ret;
    }

    g_left_ble_init_ok = RT_TRUE;
    LOG_I("BLE app init complete — waiting for NimBLE host sync");
    return RT_EOK;
}

rt_bool_t left_ble_init_ok(void)
{
    return g_left_ble_init_ok;
}
