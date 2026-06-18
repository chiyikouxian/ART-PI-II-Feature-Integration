/*
 * imu_notify_thread.c — periodic BLE notification sender for IMU data (Left Hand)
 *
 * Reads all 11 IMU channels and sends CSV text format via BLE notification
 * every NOTIFY_INTERVAL_MS milliseconds when a client is subscribed.
 *
 * Format: [DATA]<timestamp>,<hand_type>,<ch0-9 6-axis>,<ch10 9-axis>
 * Same as serial data_start output for consistency.
 */

#include <rtthread.h>
#include "imu_notify_thread.h"
#include "imu_ble_service.h"
#include "operation_mode.h"

#include "host/ble_gatt.h"
#include "host/ble_hs_mbuf.h"

#include "../mpu6050/mpu6050_thread.h"

#define DBG_SECTION_NAME  "IMU_NTFY"
#define DBG_LEVEL          DBG_INFO
#include <rtdbg.h>

/* ── Config ───────────────────────────────────────────────────────────────── */
#define NOTIFY_INTERVAL_MS        90     /* ~11.1 Hz, matches right-hand setting */
#define NOTIFY_THREAD_PRIO        22
#define NOTIFY_THREAD_STACK_SIZE  3072
#define CSV_BUFFER_SIZE           600    /* 容纳新格式：517 字节 + 余量 */
#define BLE_NOTIFY_FRAGMENT_SAFE_SIZE 180
#define FRAG_HEADER_MAX_SIZE          48

/*
 * AI辅助参考：GLM-4，2026-04-20；DeepSeek-R1，2026-04-22。
 * 用途：BLE [DATA]字段文档整理与 MTU 分片方案分析；
 * 字段总数、raw IMU 数据路径、[FRAG]格式和 180 字节上限由团队人工确认。
 */

/* ── State ────────────────────────────────────────────────────────────────── */
static struct rt_thread   _thread;
static rt_uint8_t         _stack[NOTIFY_THREAD_STACK_SIZE];
static volatile int32_t   _conn_handle = -1;
static volatile rt_bool_t _subscribed  = RT_FALSE;
static volatile uint32_t  _frame_seq   = 0;

/* ── Public: update connection handle ────────────────────────────────────── */
void imu_notify_set_conn_handle(int32_t conn_handle)
{
    _conn_handle = conn_handle;
    if (conn_handle < 0)
        _subscribed = RT_FALSE;
}

/* ── Private: atomic frame_seq helpers ───────────────────────────────────── */

static uint32_t _frame_seq_get(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    uint32_t seq = _frame_seq;
    rt_hw_interrupt_enable(level);
    return seq;
}

static void _frame_seq_inc(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    _frame_seq++;
    rt_hw_interrupt_enable(level);
}

/* ── Public: reset BLE frame_seq counter ─────────────────────────────────── */
void imu_notify_reset_frame_seq(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    _frame_seq = 0;
    rt_hw_interrupt_enable(level);
    LOG_I("frame_seq reset to 0");
}

/* ── Public: update CCCD subscription state ──────────────────────────────── */
void imu_notify_set_subscribed(rt_bool_t subscribed)
{
    _subscribed = subscribed;
    LOG_I("notify subscription %s", subscribed ? "enabled" : "disabled");
}

/* ── Fragment send helper ────────────────────────────────────────────────── */

static int _send_csv_fragments(int32_t conn_handle,
                               uint32_t frame_seq,
                               const char *hand_type,
                               const char *csv,
                               int csv_len)
{
    char frag_buf[BLE_NOTIFY_FRAGMENT_SAFE_SIZE];
    int max_payload = BLE_NOTIFY_FRAGMENT_SAFE_SIZE - FRAG_HEADER_MAX_SIZE - 1;
    if (max_payload <= 0) return 1;

    uint16_t frag_total = (uint16_t)((csv_len + max_payload - 1) / max_payload);
    int offset = 0;
    int any_sent = 0;
    int last_err = 0;

    for (uint16_t i = 0; i < frag_total; i++) {
        if (offset >= csv_len) break;

        int hlen = rt_snprintf(frag_buf, sizeof(frag_buf),
                               "[FRAG]%u,%s,%u,%u,",
                               frame_seq, hand_type, i, frag_total);
        if (hlen < 0 || hlen >= BLE_NOTIFY_FRAGMENT_SAFE_SIZE) {
            LOG_W("frag[%u/%u] header overflow hlen=%d", i, frag_total, hlen);
            break;
        }

        int part_len = csv_len - offset;
        if (part_len > max_payload) part_len = max_payload;
        if (part_len <= 0) break;

        if (hlen + part_len + 1 > BLE_NOTIFY_FRAGMENT_SAFE_SIZE) {
            LOG_W("frag[%u/%u] size overflow: hlen=%d part=%d total=%d",
                  i, frag_total, hlen, part_len, hlen + part_len + 1);
            return 1;
        }

        memcpy(frag_buf + hlen, csv + offset, part_len);
        frag_buf[hlen + part_len] = '\n';
        int total_len = hlen + part_len + 1;

        struct os_mbuf *om = ble_hs_mbuf_from_flat(frag_buf, total_len);
        if (!om) {
            LOG_W("frag[%u/%u] mbuf alloc failed", i, frag_total);
            offset += part_len;
            continue;
        }

        int rc = ble_gattc_notify_custom(
            (uint16_t)conn_handle,
            imu_ble_notify_handle(),
            om
        );

        any_sent = 1;
        if (rc != 0) {
            LOG_D("frag[%u/%u] notify err %d", i, frag_total, rc);
            if (last_err == 0) last_err = rc;
            if (rc == BLE_HS_ENOTCONN) break;
        }

        offset += part_len;
    }

    if (!any_sent) return 1;
    return last_err;
}

/* ── Thread entry ─────────────────────────────────────────────────────────── */
static void _notify_entry(void *param)
{
    (void)param;
    LOG_I("IMU notify thread started (%d ms interval, CSV format)", NOTIFY_INTERVAL_MS);

    char csv_buf[CSV_BUFFER_SIZE];

    while (1) {
        rt_thread_mdelay(NOTIFY_INTERVAL_MS);

        int32_t ch_local = _conn_handle;
        if (ch_local < 0 || !_subscribed)
            continue;

        /* BLE notify gate: silent in AUTO_STANDBY and MANUAL_SLEEP */
        if (!operation_mode_ble_notify_enabled())
            continue;

        /* Build CSV text format: [DATA]timestamp,hand_type,ch0-9(6-axis),ch10(9-axis) */
        uint32_t timestamp_ms = (uint32_t)(rt_tick_get() * 1000u / RT_TICK_PER_SECOND);
        uint32_t frame_seq = _frame_seq_get();
        int pos = 0;

        /* Start with [DATA] prefix and timestamp */
        pos += rt_snprintf(csv_buf + pos, CSV_BUFFER_SIZE - pos,
                          "[DATA]%u,left,%u", timestamp_ms, frame_seq);

        /* ch0–ch9: MPU6050 sensors (6-axis each) */
        for (int i = 0; i < 10; i++) {
            mpu_channel_data_t d;
            if (mpu_get_channel_raw_data(i, &d) == RT_EOK && d.valid) {
                pos += rt_snprintf(csv_buf + pos, CSV_BUFFER_SIZE - pos,
                                  ",%d,%d,%d,%d,%d,%d",
                                  d.ax, d.ay, d.az, d.gx, d.gy, d.gz);
            } else {
                /* Invalid channel: output zeros */
                pos += rt_snprintf(csv_buf + pos, CSV_BUFFER_SIZE - pos,
                                  ",0,0,0,0,0,0");
            }
        }

        /* ch10: dorsal ICM-20948 (9-axis with magnetometer) */
        {
            mpu_channel_data_t d;
            if (mpu_get_channel_raw_data(10, &d) == RT_EOK && d.valid) {
                pos += rt_snprintf(csv_buf + pos, CSV_BUFFER_SIZE - pos,
                                  ",%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
                                  d.ax, d.ay, d.az, d.gx, d.gy, d.gz,
                                  d.mx, d.my, d.mz);
            } else {
                /* Invalid channel: output zeros */
                pos += rt_snprintf(csv_buf + pos, CSV_BUFFER_SIZE - pos,
                                  ",0,0,0,0,0,0,0,0,0\n");
            }
        }

        /* Strip trailing LF from CSV so payload parts are clean */
        int csv_payload_len = pos;
        if (csv_payload_len > 0 && csv_buf[csv_payload_len - 1] == '\n')
            csv_payload_len--;

        /* Send CSV via fragmented BLE notifications */
        int rc = _send_csv_fragments(ch_local, frame_seq, "left", csv_buf, csv_payload_len);

        if (rc == 1) {
            /* No fragment reached notify — skip frame_seq increment */
            continue;
        }

        _frame_seq_inc();

        if (rc == BLE_HS_ENOTCONN) {
            LOG_D("stale conn handle — clearing");
            _conn_handle = -1;
        } else if (rc != 0) {
            LOG_D("notify err %d", rc);
        }
    }
}

/* ── Start ────────────────────────────────────────────────────────────────── */
rt_err_t imu_notify_thread_start(void)
{
    rt_err_t ret = rt_thread_init(
        &_thread,
        "imu_ntfy",
        _notify_entry,
        RT_NULL,
        _stack,
        sizeof(_stack),
        NOTIFY_THREAD_PRIO,
        10
    );
    if (ret != RT_EOK) {
        LOG_E("thread init failed: %d", ret);
        return ret;
    }
    return rt_thread_startup(&_thread);
}
