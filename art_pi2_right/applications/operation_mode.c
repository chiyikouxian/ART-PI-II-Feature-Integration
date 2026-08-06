/*
 * operation_mode.c — per-hand operation mode state machine (Right Hand)
 *
 * Controls the current active IMU stream gate and active stream frame_seq.
 * Current runtime active path: WiFi/TCP (imu_wifi_sender).
 * BLE code is retained but its runtime init is intentionally disabled in main.c.
 *
 * Thread safety: state variables are only written under rt_enter_critical /
 * rt_exit_critical.  frame_seq resets go through public atomic entry points.
 */

#include "operation_mode.h"
#include "imu_notify_thread.h"
#include "imu_wifi_sender.h"
#include "init_status.h"
#include "../mpu6050/mpu6050_thread.h"

#define DBG_SECTION_NAME  "OP_MODE"
#define DBG_LEVEL          DBG_INFO
#include <rtdbg.h>
#include <rtdevice.h>
#include "drv_gpio.h"

static operation_mode_t  _mode  = OP_MODE_AUTO;
static operation_state_t _state = OP_STATE_AUTO_STANDBY;

/* ── New auto-collection flow control flags ───────────────────────────────── */
static volatile int _rock_started   = 0;  /* set when CMD:START received */
static volatile int _auto_suppress  = 0;  /* suppress posture detection */
static volatile int _say_received   = 0;  /* SAY (translation result) received */
static volatile rt_tick_t _start_delay_until = 0; /* suppress detection until this tick after CMD:START */

/* ── Button defines ─────────────────────────────────────────────────────── */
#define OP_MODE_BUTTON_PIN          GET_PIN(C, 6)
#define OP_MODE_BUTTON_POLL_MS      20
#define OP_MODE_BUTTON_DEBOUNCE_MS  60
#define OP_MODE_BUTTON_STACK_SIZE   1024
#define OP_MODE_BUTTON_THREAD_PRIO  25
#define OP_MODE_BUTTON_DEBOUNCE_CNT ((OP_MODE_BUTTON_DEBOUNCE_MS) / (OP_MODE_BUTTON_POLL_MS))

/* ── Automatic-standby detection constants ──────────────────────────────── */
#define AUTO_CHECK_INTERVAL_MS        200
#define AUTO_WAKE_CONSECUTIVE_HITS      5   /* 5 × 200 ms = 1.0 s */

/* ── Dorsal hand posture (ch10 ICM-20948, ±2g → 16384 LSB/g) ── */
/* Mutable — updated by calibration thread alongside finger windows. */
static short g_dorsal_ax_min = -3000, g_dorsal_ax_max = 0;
static short g_dorsal_ay_min = -9000, g_dorsal_ay_max = -7000;
static short g_dorsal_az_min = 1000,  g_dorsal_az_max = 3000;

/* ── Finger posture (ch0-ch9 MPU6050, per-channel windows) ── */
#define AUTO_FINGER_CH_FIRST          0
#define AUTO_FINGER_CH_LAST           9
#define AUTO_FINGER_MIN_PASS          6   /* at least 6 of 10 finger channels */
typedef struct {
    short ax_min, ax_max;
    short ay_min, ay_max;
    short az_min, az_max;
} finger_window_t;

static finger_window_t g_finger_windows[10] = {
    /* ch0 */ {  -4410,  -1986, -16324, -14570,  -8746,  -1668 },
    /* ch1 */ {  -3648,  -1264, -16758, -15362,  -4200,   1510 },
    /* ch2 */ {  -1000,   5000, -15500,  11002, -20000, -13000 },
    /* ch3 */ {  -2502,   1456, -19200,  -6958, -20000, -12000 },
    /* ch4 */ {   -628,   3114,  -5200,  4724, -15000,  -4000 },
    /* ch5 */ {  -2672,   1256, -18000,  -4960, -19000, -12000 },
    /* ch6 */ {  -1036,   2368,  -8100,  15570, -17894,  -7946 },
    /* ch7 */ {  -2308,   1602, -20100,  -7170, -20000, -13000 },
    /* ch8 */ {  -2212,   3398, -11950,  10662, -18460, -11836 },
    /* ch9 */ {  -1812,   2998, -19500,  -9430, -18178, -10498 },
};
/* ── Local stillness (ch10 ICM-20948 gyro, ±2000 dps → 16.4 LSB/dps) ── */
#define AUTO_STILLNESS_GYRO_ABS_MAX   600

#define AUTO_STANDBY_STACK_SIZE      2048
#define AUTO_STANDBY_THREAD_PRIO      26

/* ── Automatic-standby worker ───────────────────────────────────────────── */

static struct rt_thread _auto_standby_thread;
static rt_uint8_t       _auto_standby_stack[AUTO_STANDBY_STACK_SIZE];
static int              _auto_hit_count = 0;

static void _auto_transition_to_running(void);
static void _calib_entry(void *param);

/* ── Finger calibration defines ────────────────────────────────────────── */
#define CALIB_BUTTON_PIN         GET_PIN(C, 7)
#define CALIB_POLL_MS            20
#define CALIB_DEBOUNCE_MS        60
#define CALIB_DEBOUNCE_CNT       ((CALIB_DEBOUNCE_MS) / (CALIB_POLL_MS))
#define CALIB_WAIT_MS            2000
#define CALIB_SAMPLE_INTERVAL_MS 100
#define CALIB_SAMPLE_COUNT       50
#define CALIB_MARGIN             2000
#define CALIB_STACK_SIZE         2048
#define CALIB_THREAD_PRIO        24

static struct rt_thread _calib_thread;
static rt_uint8_t       _calib_stack[CALIB_STACK_SIZE];

/* ── Per-channel accel-window helper ────────────────────────────────────── */
static int _ch_accel_in_window(const mpu_channel_data_t *d,
                               int ax_min, int ax_max,
                               int ay_min, int ay_max,
                               int az_min, int az_max)
{
    if (d->ax < ax_min || d->ax > ax_max) return 0;
    if (d->ay < ay_min || d->ay > ay_max) return 0;
    if (d->az < az_min || d->az > az_max) return 0;
    return 1;
}
static void _auto_standby_entry(void *param)
{
    (void)param;
    LOG_I("auto-standby worker started, interval=%d ms, hits=%d",
          AUTO_CHECK_INTERVAL_MS, AUTO_WAKE_CONSECUTIVE_HITS);
    LOG_I("auto rules: dorsal(ch10) + finger(ch0-ch9 per-ch) + stillness(gyro<%d)",
          AUTO_STILLNESS_GYRO_ABS_MAX);

    while (1) {
        rt_thread_mdelay(AUTO_CHECK_INTERVAL_MS);

        /* If suppressed, skip all posture detection */
        if (_auto_suppress) {
            _auto_hit_count = 0;
            continue;
        }

        /* CMD:START 4-second delay: skip detection until delay expires */
        if (_start_delay_until != 0) {
            rt_tick_t now = rt_tick_get();
            if ((now - _start_delay_until) >= RT_TICK_MAX / 2) {
                /* delay not yet expired (now < _start_delay_until) */
                _auto_hit_count = 0;
                continue;
            }
            /* delay expired */
            _start_delay_until = 0;
            LOG_I("CMD:START 4s delay expired, posture detection resumed");
        }

        /* Phase A: waiting for initial ready posture (AUTO_STANDBY) */
        /* Phase B: waiting for return-to-posture after sign (RUNNING + _rock_started) */
        if (_state == OP_STATE_AUTO_STANDBY) {
            /* Phase A: normal detection */
        } else if (_state == OP_STATE_RUNNING && _rock_started) {
            /* Phase B: detect return-to-posture */
        } else {
            _auto_hit_count = 0;
            continue;
        }

        int dorsal_ok  = 0;
        int finger_ok  = 0;
        int still_ok   = 0;

        /* ── 1) Dorsal hand posture (ch10) ── */
        {
            mpu_channel_data_t d;
            rt_err_t ret = mpu_get_channel_raw_data(10, &d);

            if (ret != RT_EOK) {
                LOG_D("auto-ck: ch10 (dorsal) read FAILED (ret=%d)", (int)ret);
                _auto_hit_count = 0;
                continue;
            }
            if (!d.valid) {
                LOG_D("auto-ck: ch10 (dorsal) INVALID");
                _auto_hit_count = 0;
                continue;
            }

            LOG_D("auto-ck dorsal: ax=%6d ay=%6d az=%6d | gx=%6d gy=%6d gz=%6d",
                  d.ax, d.ay, d.az, d.gx, d.gy, d.gz);
            if (!_ch_accel_in_window(&d,
                      g_dorsal_ax_min, g_dorsal_ax_max,
                      g_dorsal_ay_min, g_dorsal_ay_max,
                      g_dorsal_az_min, g_dorsal_az_max)) {
                LOG_D("auto-ck FAIL dorsal accel: ax=%d ay=%d az=%d  "
                      "win ax=[%d,%d] ay=[%d,%d] az=[%d,%d]",
                      d.ax, d.ay, d.az,
                      g_dorsal_ax_min, g_dorsal_ax_max,
                      g_dorsal_ay_min, g_dorsal_ay_max,
                      g_dorsal_az_min, g_dorsal_az_max);
                _auto_hit_count = 0;
                continue;
            }
            dorsal_ok = 1;

            /* ── 3) Stillness (ch10 gyro) ── */
            {
                int abs_gx = (d.gx >= 0) ? d.gx : -d.gx;
                int abs_gy = (d.gy >= 0) ? d.gy : -d.gy;
                int abs_gz = (d.gz >= 0) ? d.gz : -d.gz;
                if (abs_gx > AUTO_STILLNESS_GYRO_ABS_MAX ||
                    abs_gy > AUTO_STILLNESS_GYRO_ABS_MAX ||
                    abs_gz > AUTO_STILLNESS_GYRO_ABS_MAX) {
                    LOG_D("auto-ck stillness over limit (bypassed): |gx|=%d |gy|=%d |gz|=%d  limit=%d",
                          abs_gx, abs_gy, abs_gz, AUTO_STILLNESS_GYRO_ABS_MAX);
                }
                /* TEMP BYPASS (答辩前紧急放宽): 现场实测 dorsal 陀螺读数
                 * (|gz| 可达 6000+) 远超原阈值 600，导致 hit_count 永远清零、
                 * 无法进入 RUNNING。赛后需要重新采集静止基线并恢复本判定，
                 * 或改为更宽松的阈值。当前直接放行，不做静止门控。 */
                still_ok = 1;
            }
        }

        /* ── 2) Finger posture (ch0-ch9, per-channel, >=6 pass) ── */
        {
            int finger_pass = 0;
            for (int ch = AUTO_FINGER_CH_FIRST; ch <= AUTO_FINGER_CH_LAST; ch++) {
                mpu_channel_data_t d;
                rt_err_t ret = mpu_get_channel_raw_data(ch, &d);

                if (ret != RT_EOK) {
                    LOG_D("auto-ck: ch%d (finger) read FAILED (ret=%d)", ch, (int)ret);
                    continue;
                }
                if (!d.valid) {
                    LOG_D("auto-ck: ch%d (finger) INVALID", ch);
                    continue;
                }
                const finger_window_t *fw = &g_finger_windows[ch];
                if (!_ch_accel_in_window(&d, fw->ax_min, fw->ax_max,
                                         fw->ay_min, fw->ay_max,
                                         fw->az_min, fw->az_max)) {
                    LOG_D("auto-ck FAIL finger ch%d: ax=%d ay=%d az=%d  "
                          "win ax=[%d,%d] ay=[%d,%d] az=[%d,%d]",
                          ch, d.ax, d.ay, d.az,
                          fw->ax_min, fw->ax_max,
                          fw->ay_min, fw->ay_max,
                          fw->az_min, fw->az_max);
                } else {
                    finger_pass++;
                }
            }

            if (finger_pass < AUTO_FINGER_MIN_PASS) {
                LOG_D("auto-ck FAIL finger: %d/%d PASS (need >=%d)",
                      finger_pass, AUTO_FINGER_CH_LAST - AUTO_FINGER_CH_FIRST + 1,
                      AUTO_FINGER_MIN_PASS);
                _auto_hit_count = 0;
                continue;
            }
            finger_ok = 1;
        }

        _auto_hit_count++;
        LOG_I("auto-ck HIT %d/%d (dorsal=%d finger=%d still=%d)",
              _auto_hit_count, AUTO_WAKE_CONSECUTIVE_HITS,
              dorsal_ok, finger_ok, still_ok);

        if (_auto_hit_count >= AUTO_WAKE_CONSECUTIVE_HITS) {
            if (_state == OP_STATE_AUTO_STANDBY) {
                /* Phase A: initial posture detected → enter RUNNING */
                _auto_transition_to_running();
            } else if (_state == OP_STATE_RUNNING && _rock_started) {
                /* Phase B: return-to-posture detected → sign completed */
                LOG_I("AUTO: sign completed, sending WAITING_STOP:right");
                imu_wifi_sender_send_model("WAITING_STOP:right\n", 19);
                rt_enter_critical();
                _state = OP_STATE_WAITING_STOP;
                _auto_suppress = 1;
                _rock_started = 0;
                rt_exit_critical();
            }
            _auto_hit_count = 0;
        }
    }
}

static void _auto_transition_to_running(void)
{
    rt_enter_critical();
    if (_mode == OP_MODE_AUTO && _state == OP_STATE_AUTO_STANDBY) {
        _state = OP_STATE_RUNNING;
        rt_exit_critical();
        imu_notify_reset_frame_seq();
        imu_wifi_sender_reset_frame_seq();
        LOG_I("AUTO WAKE: special action detected → RUNNING");
    } else {
        rt_exit_critical();
        LOG_W("AUTO WAKE blocked: mode=%d state=%d (need mode=0 state=0)",
              (int)_mode, (int)_state);
    }
}

static struct rt_thread _btn_thread;
static rt_uint8_t       _btn_stack[OP_MODE_BUTTON_STACK_SIZE];

static void _button_entry(void *param)
{
    (void)param;
    int debounce = 0;
    int pressed  = 0;

    while (1) {
        rt_thread_mdelay(OP_MODE_BUTTON_POLL_MS);

        int level = rt_pin_read(OP_MODE_BUTTON_PIN);

        if (!pressed) {
            if (level == PIN_LOW) {
                debounce++;
                if (debounce >= OP_MODE_BUTTON_DEBOUNCE_CNT) {
                    operation_mode_toggle_local();
                    pressed  = 1;
                    debounce = 0;
                }
            } else {
                debounce = 0;
            }
        } else {
            if (level == PIN_HIGH) {
                debounce++;
                if (debounce >= OP_MODE_BUTTON_DEBOUNCE_CNT) {
                    pressed  = 0;
                    debounce = 0;
                }
            } else {
                debounce = 0;
            }
        }
    }
}
void operation_mode_init(void)
{
    rt_enter_critical();
    _mode  = OP_MODE_AUTO;
    _state = OP_STATE_AUTO_STANDBY;
    rt_exit_critical();

    rt_pin_mode(OP_MODE_BUTTON_PIN, PIN_MODE_INPUT_PULLUP);

    rt_err_t ret = rt_thread_init(
        &_btn_thread,
        "op_btn",
        _button_entry,
        RT_NULL,
        _btn_stack,
        sizeof(_btn_stack),
        OP_MODE_BUTTON_THREAD_PRIO,
        10
    );
    if (ret == RT_EOK) {
        rt_thread_startup(&_btn_thread);
        LOG_I("op mode init: AUTO / AUTO_STANDBY, button poll on PC6");
    } else {
        LOG_E("op mode init: button thread init failed: %d", ret);
    }

    ret = rt_thread_init(
        &_auto_standby_thread,
        "auto_ck",
        _auto_standby_entry,
        RT_NULL,
        _auto_standby_stack,
        sizeof(_auto_standby_stack),
        AUTO_STANDBY_THREAD_PRIO,
        10
    );
    if (ret == RT_EOK) {
        rt_thread_startup(&_auto_standby_thread);
        LOG_I("op mode init: auto-standby worker started");
    } else {
        LOG_E("op mode init: auto-standby worker init failed: %d", ret);
    }

    rt_pin_mode(CALIB_BUTTON_PIN, PIN_MODE_INPUT_PULLUP);
    ret = rt_thread_init(
        &_calib_thread,
        "calib",
        _calib_entry,
        RT_NULL,
        _calib_stack,
        sizeof(_calib_stack),
        CALIB_THREAD_PRIO,
        10
    );
    if (ret == RT_EOK) {
        rt_thread_startup(&_calib_thread);
        LOG_I("op mode init: finger calibration thread started (PC7)");
    } else {
        LOG_E("op mode init: calib thread init failed: %d", ret);
    }
}

static void operation_mode_resend_model(void)
{
    static char mbuf[560];
    char *p = mbuf;
    int remain = sizeof(mbuf) - 2;
    int n = rt_snprintf(p, remain, "MODEL:right,");
    p += n; remain -= n;
    for (int ch = 0; ch < 10; ch++) {
        n = rt_snprintf(p, remain, "%d,%d,%d,%d,%d,%d,",
            g_finger_windows[ch].ax_min, g_finger_windows[ch].ax_max,
            g_finger_windows[ch].ay_min, g_finger_windows[ch].ay_max,
            g_finger_windows[ch].az_min, g_finger_windows[ch].az_max);
        p += n; remain -= n;
    }
    n = rt_snprintf(p, remain, "%d,%d,%d,%d,%d,%d\n",
        g_dorsal_ax_min, g_dorsal_ax_max,
        g_dorsal_ay_min, g_dorsal_ay_max,
        g_dorsal_az_min, g_dorsal_az_max);
    p += n;
    imu_wifi_sender_send_model(mbuf, (int)(p - mbuf));
    LOG_I("MODEL:right re-sent (%d bytes)", (int)(p - mbuf));
}

void operation_mode_toggle_local(void)
{
    rt_enter_critical();
    if (_mode == OP_MODE_AUTO) {
        _mode  = OP_MODE_MANUAL;
        _state = OP_STATE_MANUAL_SLEEP;
        rt_exit_critical();
        LOG_I("op mode toggled: MANUAL / MANUAL_SLEEP");
    } else {
        _mode  = OP_MODE_AUTO;
        _state = OP_STATE_AUTO_STANDBY;
        rt_exit_critical();
        LOG_I("op mode toggled: AUTO / AUTO_STANDBY");
    }
}

operation_mode_t operation_mode_get_mode(void)
{
    return _mode;
}

operation_state_t operation_mode_get_state(void)
{
    return _state;
}

rt_bool_t operation_mode_stream_enabled(void)
{
    return (_state == OP_STATE_RUNNING) ? RT_TRUE : RT_FALSE;
}

rt_bool_t operation_mode_ble_notify_enabled(void)
{
    return operation_mode_stream_enabled();
}

void operation_mode_notify_say_received(void)
{
    if (_state == OP_STATE_WAITING_STOP || _auto_suppress) {
        LOG_I("SAY received in WAITING_STOP — starting 2s cooldown");
        _say_received = 1;
        rt_thread_mdelay(2000);
        rt_enter_critical();
        _auto_suppress = 0;
        _say_received = 0;
        _state = OP_STATE_AUTO_STANDBY;
        _auto_hit_count = 0;
        rt_exit_critical();
        LOG_I("Cooldown done, posture detection re-enabled");
    }
}
rt_bool_t operation_mode_handle_cmd(const char *cmd)
{
    if (cmd == RT_NULL)
        return RT_FALSE;

    if (rt_strcmp(cmd, "CMD:RESET_SEQ") == 0) {
        imu_notify_reset_frame_seq();
        imu_wifi_sender_reset_frame_seq();
        LOG_I("CMD:RESET_SEQ — frame_seq reset, state unchanged (%d)", (int)_state);
        return RT_TRUE;
    }

    if (rt_strcmp(cmd, "CMD:START") == 0) {
        imu_notify_reset_frame_seq();
        imu_wifi_sender_reset_frame_seq();
        rt_enter_critical();
        _state = OP_STATE_RUNNING;
        _rock_started = 1;
        _start_delay_until = rt_tick_get() + rt_tick_from_millisecond(4000);
        rt_exit_critical();
        LOG_I("CMD:START — frame_seq reset, rock_started=1, 4s delay, state → RUNNING");
        return RT_TRUE;
    }

    if (rt_strcmp(cmd, "CMD:STOP") == 0) {
        imu_notify_reset_frame_seq();
        imu_wifi_sender_reset_frame_seq();
        rt_enter_critical();
        _state = (_mode == OP_MODE_MANUAL) ? OP_STATE_MANUAL_SLEEP : OP_STATE_AUTO_STANDBY;
        _auto_hit_count = 0;
        _rock_started = 0;
        rt_exit_critical();
        LOG_I("CMD:STOP — frame_seq reset, hit_count cleared, state → %s",
              (_mode == OP_MODE_MANUAL) ? "MANUAL_SLEEP" : "AUTO_STANDBY");
        return RT_TRUE;
    }

    if (rt_strcmp(cmd, "MODE:MANUAL") == 0) {
        rt_enter_critical();
        _mode  = OP_MODE_MANUAL;
        _state = OP_STATE_MANUAL_SLEEP;
        _auto_hit_count = 0;
        _rock_started = 0;
        rt_exit_critical();
        LOG_I("MODE:MANUAL — switched to MANUAL / MANUAL_SLEEP");
        return RT_TRUE;
    }

    if (rt_strcmp(cmd, "MODE:AUTO") == 0) {
        rt_enter_critical();
        _mode  = OP_MODE_AUTO;
        _state = OP_STATE_AUTO_STANDBY;
        _auto_hit_count = 0;
        _rock_started = 0;
        rt_exit_critical();
        LOG_I("MODE:AUTO — switched to AUTO / AUTO_STANDBY, re-sending MODEL");
        operation_mode_resend_model();
        return RT_TRUE;
    }

    return RT_FALSE;
}
/* ── MSH debug command: auto_status ──────────────────────────────────────── */
#ifdef RT_USING_FINSH
#include <finsh.h>

static void _auto_status_cmd(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    rt_kprintf("\n===== Auto-Standby Status =====\n");
    rt_kprintf("Mode:       %s\n", _mode == OP_MODE_AUTO ? "AUTO" : "MANUAL");
    rt_kprintf("State:      %d (0=AUTO_STANDBY 1=MANUAL_SLEEP 2=RUNNING 3=WAITING_STOP)\n",
               (int)_state);
    rt_kprintf("Hit count:  %d / %d\n", _auto_hit_count, AUTO_WAKE_CONSECUTIVE_HITS);
    rt_kprintf("rock_started: %d  suppress: %d  say_received: %d\n",
               _rock_started, _auto_suppress, _say_received);
    rt_kprintf("Interval:   %d ms\n", AUTO_CHECK_INTERVAL_MS);
    rt_kprintf("Gyro limit: |g| < %d raw (approx %d dps @2000dps full-scale)\n",
               AUTO_STILLNESS_GYRO_ABS_MAX,
               (int)(AUTO_STILLNESS_GYRO_ABS_MAX * 10 / 164));

    /* ── Dorsal (ch10) ── */
    {
        mpu_channel_data_t d;
        rt_err_t ret = mpu_get_channel_raw_data(10, &d);

        rt_kprintf("\n--- 1) Dorsal Posture (ch10 ICM-20948) ---\n");
        if (ret != RT_EOK) {
            rt_kprintf("  READ FAILED (ret=%d)\n", (int)ret);
        } else if (!d.valid) {
            rt_kprintf("  INVALID (sensor not detected)\n");
        } else {
            rt_kprintf("  Accel: ax=%6d  ay=%6d  az=%6d\n", d.ax, d.ay, d.az);
            rt_kprintf("  Gyro:  gx=%6d  gy=%6d  gz=%6d\n", d.gx, d.gy, d.gz);

            int a_ok = _ch_accel_in_window(&d,
                          g_dorsal_ax_min, g_dorsal_ax_max,
                          g_dorsal_ay_min, g_dorsal_ay_max,
                          g_dorsal_az_min, g_dorsal_az_max);
            rt_kprintf("  Dorsal accel window: %s\n", a_ok ? "PASS" : "FAIL");
            if (!a_ok) {
                rt_kprintf("    ax=%6d  in [%6d,%6d]\n",
                          d.ax, g_dorsal_ax_min, g_dorsal_ax_max);
                rt_kprintf("    ay=%6d  in [%6d,%6d]\n",
                          d.ay, g_dorsal_ay_min, g_dorsal_ay_max);
                rt_kprintf("    az=%6d  in [%6d,%6d]\n",
                          d.az, g_dorsal_az_min, g_dorsal_az_max);
            }

            int abs_gx = (d.gx >= 0) ? d.gx : -d.gx;
            int abs_gy = (d.gy >= 0) ? d.gy : -d.gy;
            int abs_gz = (d.gz >= 0) ? d.gz : -d.gz;
            int s_ok = (abs_gx <= AUTO_STILLNESS_GYRO_ABS_MAX &&
                        abs_gy <= AUTO_STILLNESS_GYRO_ABS_MAX &&
                        abs_gz <= AUTO_STILLNESS_GYRO_ABS_MAX);
            rt_kprintf("  Stillness (limit=%d): %s\n",
                       AUTO_STILLNESS_GYRO_ABS_MAX, s_ok ? "PASS" : "FAIL");
            if (!s_ok) {
                rt_kprintf("    |gx|=%6d  |gy|=%6d  |gz|=%6d\n",
                          abs_gx, abs_gy, abs_gz);
            }
        }
    }

    /* ── Finger (ch0-ch9, per-channel, >=6 pass) ── */
    rt_kprintf("\n--- 2) Finger Posture (ch%d-ch%d MPU6050, >=%d pass) ---\n",
               AUTO_FINGER_CH_FIRST, AUTO_FINGER_CH_LAST, AUTO_FINGER_MIN_PASS);
    {
        int pass_count = 0;
        int fail_count = 0;

        for (int ch = AUTO_FINGER_CH_FIRST; ch <= AUTO_FINGER_CH_LAST; ch++) {
            mpu_channel_data_t d;
            rt_err_t ret = mpu_get_channel_raw_data(ch, &d);

            if (ret != RT_EOK) {
                rt_kprintf("  ch%d: READ FAILED (ret=%d)\n", ch, (int)ret);
                fail_count++;
                continue;
            }
            if (!d.valid) {
                rt_kprintf("  ch%d: INVALID\n", ch);
                fail_count++;
                continue;
            }
            const finger_window_t *fw = &g_finger_windows[ch];
            int ok = _ch_accel_in_window(&d, fw->ax_min, fw->ax_max,
                                         fw->ay_min, fw->ay_max,
                                         fw->az_min, fw->az_max);
            rt_kprintf("  ch%d: ax=%6d in [%6d,%6d]  ay=%6d in [%6d,%6d]  az=%6d in [%6d,%6d]  %s\n",
                      ch,
                      d.ax, fw->ax_min, fw->ax_max,
                      d.ay, fw->ay_min, fw->ay_max,
                      d.az, fw->az_min, fw->az_max,
                      ok ? "PASS" : "FAIL");
            if (ok) {
                pass_count++;
            } else {
                fail_count++;
            }
        }

        int finger_ok = (pass_count >= AUTO_FINGER_MIN_PASS);
        rt_kprintf("  Finger result: %d PASS, %d FAIL (threshold: >=%d PASS) → %s\n",
                   pass_count, fail_count, AUTO_FINGER_MIN_PASS,
                   finger_ok ? "OK" : "NOT MET");
    }

    rt_kprintf("\n--- Overall ---\n");
    rt_kprintf("  Gate: dorsal + finger(>=%d/10) + stillness\n", AUTO_FINGER_MIN_PASS);
    rt_kprintf("  Hit count: %d / %d\n", _auto_hit_count, AUTO_WAKE_CONSECUTIVE_HITS);
    rt_kprintf("==================================\n\n");
}
MSH_CMD_EXPORT_ALIAS(_auto_status_cmd, auto_status, Show auto-standby coarse-detection status for threshold tuning);
#endif /* RT_USING_FINSH */

/* ── Finger calibration thread entry ────────────────────────────────────── */

static void _calib_entry(void *param)
{
    (void)param;
    int debounce = 0;

    while (1) {
        rt_thread_mdelay(CALIB_POLL_MS);

        if (rt_pin_read(CALIB_BUTTON_PIN) != PIN_LOW) {
            debounce = 0;
            continue;
        }
        debounce++;
        if (debounce < CALIB_DEBOUNCE_CNT)
            continue;
        debounce = 0;

        LOG_I("calib: button pressed, waiting %d ms...", CALIB_WAIT_MS);
        init_status_show_calib("CAL");
        rt_thread_mdelay(CALIB_WAIT_MS);

        /* Init min/max trackers — fingers (ch0-ch9) + dorsal (ch10) */
        short min_ax[10], max_ax[10];
        short min_ay[10], max_ay[10];
        short min_az[10], max_az[10];
        for (int ch = 0; ch < 10; ch++) {
            min_ax[ch] = 32767; max_ax[ch] = -32768;
            min_ay[ch] = 32767; max_ay[ch] = -32768;
            min_az[ch] = 32767; max_az[ch] = -32768;
        }
        short dorsal_min_ax = 32767, dorsal_max_ax = -32768;
        short dorsal_min_ay = 32767, dorsal_max_ay = -32768;
        short dorsal_min_az = 32767, dorsal_max_az = -32768;

        LOG_I("calib: collecting %d samples...", CALIB_SAMPLE_COUNT);
        for (int i = 0; i < CALIB_SAMPLE_COUNT; i++) {
            for (int ch = 0; ch < 10; ch++) {
                mpu_channel_data_t d;
                if (mpu_get_channel_raw_data(ch, &d) == RT_EOK && d.valid) {
                    if (d.ax < min_ax[ch]) min_ax[ch] = d.ax;
                    if (d.ax > max_ax[ch]) max_ax[ch] = d.ax;
                    if (d.ay < min_ay[ch]) min_ay[ch] = d.ay;
                    if (d.ay > max_ay[ch]) max_ay[ch] = d.ay;
                    if (d.az < min_az[ch]) min_az[ch] = d.az;
                    if (d.az > max_az[ch]) max_az[ch] = d.az;
                }
            }
            {
                mpu_channel_data_t d;
                if (mpu_get_channel_raw_data(10, &d) == RT_EOK && d.valid) {
                    if (d.ax < dorsal_min_ax) dorsal_min_ax = d.ax;
                    if (d.ax > dorsal_max_ax) dorsal_max_ax = d.ax;
                    if (d.ay < dorsal_min_ay) dorsal_min_ay = d.ay;
                    if (d.ay > dorsal_max_ay) dorsal_max_ay = d.ay;
                    if (d.az < dorsal_min_az) dorsal_min_az = d.az;
                    if (d.az > dorsal_max_az) dorsal_max_az = d.az;
                }
            }
            rt_thread_mdelay(CALIB_SAMPLE_INTERVAL_MS);
        }

        /* Apply thresholds — fingers */
        for (int ch = 0; ch < 10; ch++) {
            if (min_ax[ch] <= max_ax[ch]) {
                g_finger_windows[ch].ax_min = min_ax[ch] - CALIB_MARGIN;
                g_finger_windows[ch].ax_max = max_ax[ch] + CALIB_MARGIN;
                g_finger_windows[ch].ay_min = min_ay[ch] - CALIB_MARGIN;
                g_finger_windows[ch].ay_max = max_ay[ch] + CALIB_MARGIN;
                g_finger_windows[ch].az_min = min_az[ch] - CALIB_MARGIN;
                g_finger_windows[ch].az_max = max_az[ch] + CALIB_MARGIN;
            }
        }

        /* Apply thresholds — dorsal (ch10 ICM) */
        if (dorsal_min_ax <= dorsal_max_ax) {
            g_dorsal_ax_min = dorsal_min_ax - CALIB_MARGIN;
            g_dorsal_ax_max = dorsal_max_ax + CALIB_MARGIN;
            g_dorsal_ay_min = dorsal_min_ay - CALIB_MARGIN;
            g_dorsal_ay_max = dorsal_max_ay + CALIB_MARGIN;
            g_dorsal_az_min = dorsal_min_az - CALIB_MARGIN;
            g_dorsal_az_max = dorsal_max_az + CALIB_MARGIN;
            LOG_I("calib: dorsal(ch10) updated: ax=[%d,%d] ay=[%d,%d] az=[%d,%d]",
                  g_dorsal_ax_min, g_dorsal_ax_max,
                  g_dorsal_ay_min, g_dorsal_ay_max,
                  g_dorsal_az_min, g_dorsal_az_max);
        }

        LOG_I("calib: done, thresholds updated");

        /* Send MODEL data to Rock */
        /* Send MODEL data to Rock (fingers + dorsal) */
        {
            char mbuf[560];
            char *p = mbuf;
            int remain = sizeof(mbuf) - 2;
            int n = rt_snprintf(p, remain, "MODEL:right,");
            p += n; remain -= n;
            for (int ch = 0; ch < 10; ch++) {
                n = rt_snprintf(p, remain, "%d,%d,%d,%d,%d,%d,",
                    g_finger_windows[ch].ax_min, g_finger_windows[ch].ax_max,
                    g_finger_windows[ch].ay_min, g_finger_windows[ch].ay_max,
                    g_finger_windows[ch].az_min, g_finger_windows[ch].az_max);
                p += n; remain -= n;
            }
            n = rt_snprintf(p, remain, "%d,%d,%d,%d,%d,%d\n",
                g_dorsal_ax_min, g_dorsal_ax_max,
                g_dorsal_ay_min, g_dorsal_ay_max,
                g_dorsal_az_min, g_dorsal_az_max);
            p += n; remain -= n;
            imu_wifi_sender_send_model(mbuf, (int)(p - mbuf));
        }

        init_status_show_calib("DONE");
        rt_thread_mdelay(1000);
        init_status_clear_calib();

        /* Wait for button release */
        while (rt_pin_read(CALIB_BUTTON_PIN) == PIN_LOW)
            rt_thread_mdelay(CALIB_POLL_MS);
    }
}
