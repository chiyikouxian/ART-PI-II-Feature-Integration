/*
 * ble_app.c — NimBLE peripheral application for ART-Pi2 IMU BLE service
 *
 * Responsibilities:
 *   1. Wait for CYW43438 BT core ready (cyw43438_bt_init via INIT_DEVICE_EXPORT)
 *   2. Install the NimBLE HCI transport shim (nimble_hci_adapter)
 *   3. Register standard GAP/GATT services and our custom IMU service
 *   4. Start the NimBLE host thread (ble_hs_thread_startup)
 *   5. Start advertising when NimBLE host syncs with the controller
 *   6. On connect: hand connection handle to notify thread
 *   7. On disconnect: restart advertising
 *
 * Init levels:
 *   INIT_DEVICE_EXPORT (level 3) — cyw43438_bt_init, UART7 + HCI Reset
 *   INIT_COMPONENT_EXPORT (level 4) — nimble_port_rtthread_init (auto, calls
 *                                      nimble_port_init internally)
 *   INIT_ENV_EXPORT (level 5) — _ble_app_init (this file)
 *
 * Device name advertised: "ART-Pi2-IMU-R"
 * Advertising interval  : 200–500 ms (connectable undirected, legacy PDU)
 */

#include <rtthread.h>
#include "cyw43438_bt.h"
#include "nimble_hci_adapter.h"
#include "imu_ble_service.h"
#include "imu_notify_thread.h"
#include "operation_mode.h"

/* NimBLE port (provides nimble_port_run used by the host thread) */
#include "nimble/nimble_port.h"

/* NimBLE host */
#include "host/ble_hs.h"
#include "host/ble_gap.h"

/* Standard NimBLE services */
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

/*
 * ble_hs_thread_startup() is defined in
 * packages/NimBLE-v1.0.0/porting/npl/rtthread/src/nimble_port_rtthread.c
 * and is not declared in any public header.
 */
extern void ble_hs_thread_startup(void);

#define DBG_SECTION_NAME  "BLE_APP"
#define DBG_LEVEL          DBG_INFO
#include <rtdbg.h>

#define DEVICE_NAME  "ART-Pi2-IMU-R"

/* Convert milliseconds to 0.625 ms advertising interval units */
#ifndef BLE_GAP_ADV_ITVL_MS
#define BLE_GAP_ADV_ITVL_MS(x)  ((x) * 1000 / 625)
#endif

static uint8_t _adv_addr_type;
static volatile rt_bool_t g_right_ble_init_ok = RT_FALSE;

/* ── Forward declaration ──────────────────────────────────────────────────── */
static int _gap_event(struct ble_gap_event *event, void *arg);

/* ── Start / restart BLE advertising ─────────────────────────────────────── */
static void _start_advertising(void)
{
    struct ble_hs_adv_fields  fields;
    struct ble_gap_adv_params adv_params;
    int rc;

    rt_memset(&fields,     0, sizeof(fields));
    rt_memset(&adv_params, 0, sizeof(adv_params));

    /* Advertising data */
    fields.flags                = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name                 = (uint8_t *)DEVICE_NAME;
    fields.name_len             = (uint8_t)strlen(DEVICE_NAME);
    fields.name_is_complete     = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        LOG_E("adv set fields failed: %d", rc);
        g_right_ble_init_ok = RT_FALSE;
        return;
    }

    /* Advertising parameters: connectable undirected, 200–500 ms interval */
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min  = BLE_GAP_ADV_ITVL_MS(200);
    adv_params.itvl_max  = BLE_GAP_ADV_ITVL_MS(500);

    rc = ble_gap_adv_start(_adv_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, _gap_event, NULL);
    if (rc != 0) {
        LOG_E("adv start failed: %d", rc);
        g_right_ble_init_ok = RT_FALSE;
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
            /* Connection failed — restart advertising */
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
        /* Advertising timed out (BLE_HS_FOREVER means this shouldn't fire) */
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

/* ── NimBLE host sync callback — fires when host and controller are in sync ─ */
static void _on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &_adv_addr_type);
    if (rc != 0) {
        LOG_E("ble_hs_id_infer_auto failed: %d", rc);
        g_right_ble_init_ok = RT_FALSE;
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

/* ── NimBLE host reset callback ──────────────────────────────────────────── */
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
int right_ble_app_init(void)
{
    rt_err_t ret;

    LOG_I("Starting BLE initialization after WiFi connected...");

    /* 0. Initialize operation mode state machine (boot default: AUTO/AUTO_STANDBY) */
    operation_mode_init();

    /*
     * 1. Wait for CYW43438 BT core ready.
     *    cyw43438_bt_init() ran at INIT_DEVICE_EXPORT (level 3) and performed
     *    HCI Reset; _ready flag is set when the Reset Complete event arrives.
     */
    int waited = 0;
    while (!cyw43438_bt_is_ready() && waited < 100) {
        rt_thread_mdelay(50);
        waited++;
    }
    if (!cyw43438_bt_is_ready()) {
        LOG_E("CYW43438 BT not ready — NimBLE init aborted");
        g_right_ble_init_ok = RT_FALSE;
        return -RT_ETIMEOUT;
    }

    /*
     * 2. Install HCI transport shim.
     */
    ret = nimble_hci_adapter_init();
    if (ret != RT_EOK) {
        LOG_E("nimble_hci_adapter_init failed: %d", ret);
        g_right_ble_init_ok = RT_FALSE;
        return ret;
    }

    /*
     * 3. Configure host callbacks.
     */
    ble_hs_cfg.sync_cb  = _on_sync;
    ble_hs_cfg.reset_cb = _on_reset;

    /* 4. Register standard services */
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(DEVICE_NAME);

    /* 5. Register custom IMU GATT service */
    ret = imu_ble_service_init();
    if (ret != RT_EOK) {
        LOG_E("imu_ble_service_init failed: %d", ret);
        g_right_ble_init_ok = RT_FALSE;
        return ret;
    }

    /*
     * 6. Start NimBLE host thread.
     */
    ble_hs_thread_startup();

    /* 7. Start IMU notification thread */
    ret = imu_notify_thread_start();
    if (ret != RT_EOK) {
        LOG_E("imu_notify_thread_start failed: %d", ret);
        g_right_ble_init_ok = RT_FALSE;
        return ret;
    }

    g_right_ble_init_ok = RT_TRUE;
    LOG_I("BLE app init complete — waiting for NimBLE host sync");
    return RT_EOK;
}

rt_bool_t right_ble_init_ok(void)
{
    return g_right_ble_init_ok;
}
