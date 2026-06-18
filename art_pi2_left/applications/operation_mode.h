/*
 * operation_mode.h - per-hand operation mode and state machine
 *
 * Controls the current active IMU stream gate and active stream frame_seq
 * reset behavior. In the current runtime stage, the active path is
 * WiFi/TCP (imu_wifi_sender). BLE code is retained in the tree but its
 * runtime init is intentionally disabled in main.c.
 *
 * States:
 *   OP_STATE_AUTO_STANDBY  - boot default; stream silent; auto special-action check
 *   OP_STATE_MANUAL_SLEEP  - manual mode; stream silent; no auto wake-up
 *   OP_STATE_RUNNING       - active IMU stream enabled at 90 ms cadence
 *
 * Modes:
 *   OP_MODE_AUTO   - automatic mode (boot default)
 *   OP_MODE_MANUAL - manual mode (switched by local button)
 */

#ifndef OPERATION_MODE_H
#define OPERATION_MODE_H

#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OP_MODE_AUTO   = 0,
    OP_MODE_MANUAL = 1,
} operation_mode_t;

typedef enum {
    OP_STATE_AUTO_STANDBY = 0,
    OP_STATE_MANUAL_SLEEP,
    OP_STATE_RUNNING,
    OP_STATE_WAITING_STOP,
} operation_state_t;

/**
 * @brief  Initialize operation mode state machine.
 *         Must be called once at startup before any other API.
 *         Boot default: OP_MODE_AUTO, OP_STATE_AUTO_STANDBY.
 */
void operation_mode_init(void);

/**
 * @brief  Toggle local endpoint between automatic and manual mode.
 *         OP_MODE_AUTO  → OP_MODE_MANUAL / OP_STATE_MANUAL_SLEEP
 *         OP_MODE_MANUAL → OP_MODE_AUTO  / OP_STATE_AUTO_STANDBY
 *         Does NOT persist across power cycles.
 *         Does NOT communicate with the other hand.
 */
void operation_mode_toggle_local(void);

/** @brief  Query the current mode. */
operation_mode_t operation_mode_get_mode(void);

/** @brief  Query the current state. */
operation_state_t operation_mode_get_state(void);

/**
 * @brief  Return RT_TRUE when the current active IMU stream may emit a frame.
 *         Returns RT_FALSE in AUTO_STANDBY and MANUAL_SLEEP states.
 */
rt_bool_t operation_mode_stream_enabled(void);

/**
 * @brief  Compatibility wrapper for historical callers.
 *         Semantics are identical to operation_mode_stream_enabled().
 *
 * @deprecated Prefer operation_mode_stream_enabled() for new code.
 *         Returns RT_FALSE in AUTO_STANDBY and MANUAL_SLEEP states.
 */
rt_bool_t operation_mode_ble_notify_enabled(void);

/**
 * @brief  Handle a ROCK command received over the current active command path.
 *
 *         Recognised commands (exact UTF-8 match, no trailing whitespace):
 *           "CMD:RESET_SEQ" - reset active stream frame_seq to 0; no state change
 *           "CMD:START"     - reset active stream frame_seq; enter RUNNING
 *           "CMD:STOP"      - reset active stream frame_seq; enter MANUAL_SLEEP
 *                              (manual) or AUTO_STANDBY (auto). In AUTO mode,
 *                              this also clears the local auto-hit counter.
 *
 *         Returns RT_TRUE if the payload was a recognised command (caller must
 *         NOT pass it to the speech/translated-text path).
 *         Returns RT_FALSE for any other payload.
 */
rt_bool_t operation_mode_handle_cmd(const char *cmd);

/**
 * @brief  Notify operation_mode that a SAY (translation result) was received.
 *         In WAITING_STOP state this triggers the 2-second cooldown before
 *         re-enabling posture detection.
 */
void operation_mode_notify_say_received(void);

#ifdef __cplusplus
}
#endif

#endif /* OPERATION_MODE_H */
