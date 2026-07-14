/**
 * @file net_session.h
 * @brief Per-TCP-connection session identity.
 *
 * Two TCP paths share the same module:
 *   - PC path  (role 'C'): announces HELLO so PC can flush stale cache.
 *     Does NOT reset any frame_seq — the PC only cares about JSON id.
 *   - ROCK path (role 'S'): announces HELLO so ROCK can flush stale cache.
 *     DOES call the rock-specific seq_reset to prevent new-session seqs
 *     colliding with any cache the peer still holds.
 *
 * Design
 * ------
 * A single global info slot holds (boot_id, session_id). These are sent
 * in the HELLO line so the consumer can key its per-session cache.
 * Each path registers its own seq_reset callback so PC reconnects don't
 * touch the ROCK stream and vice versa.
 *
 * Connection rebuild sequence:
 *   1. Board connect()
 *   2. Board sends HELLO,<side>,<boot_id>,<session_id>\n
 *   3. Board calls registered seq_reset (ROCK path only)
 *   4. Board starts streaming [DATA]... or JSON lines
 *   5. Consumer: on HELLO, flush all cached frames with old session_id
 */
#ifndef __NET_SESSION_H
#define __NET_SESSION_H

#include <rtthread.h>

/**
 * @brief  Per-path callback: zeros the worker's frame counter.
 *         Only the ROCK path needs this; the PC path leaves it NULL.
 */
typedef void (*net_session_seq_reset_t)(void);

typedef struct {
    char        side;             /* 'C' or 'S' — sent in HELLO for consumer cache key */
    rt_uint32_t boot_id;          /* stable across reconnects within same boot */
    rt_uint32_t session_id;       /* increments every successful connect */
    rt_tick_t   started_tick;     /* set by net_session_init() */
    rt_tick_t   last_announce_tick; /* tick at the most recent successful announce */
    rt_uint32_t announce_count;   /* total successful announces across both roles */
    rt_uint32_t announce_fail_count; /* total announce attempts that failed */
} net_session_info_t;

/**
 * @brief  Register the ROCK path's frame-seq reset callback.
 *         imu_wifi_sender calls this during startup.
 */
void net_session_register_seq_reset_rock(net_session_seq_reset_t fn);

/**
 * @brief  Register the PC path's frame-seq reset callback (currently unused
 *         since PC JSON id is monotonic, but available for future use).
 */
void net_session_register_seq_reset_pc(net_session_seq_reset_t fn);

/**
 * @brief  Emit "HELLO,<side>,<boot_id>,<session_id>\n" on @p sock.
 *         Sends HELLO so the consumer can flush stale cache.
 *         Calls the role-specific seq_reset callback:
 *           'C' (PC path): does NOT call seq_reset
 *           'S' (ROCK path): calls ROCK's seq_reset
 *
 * @param  sock  Connected TCP socket.
 * @param  role  'C' for PC path, 'S' for ROCK path.
 * @return RT_EOK if fully sent, -RT_ERROR otherwise.
 */
rt_err_t net_session_announce(int sock, char role);

/**
 * @brief  Read-only snapshot of the current session.
 */
const net_session_info_t *net_session_current(void);

/**
 * @brief  Initialise net_session (mutex + boot_id). MUST be called once
 *         from the main thread before either TCP worker starts. This
 *         replaces the previous lazy mutex-init which had a race window
 *         where two worker threads could both observe g_lock_inited=RT_FALSE.
 */
void net_session_init(void);

/**
 * @brief  Check whether a line is a HELLO and extract its session info.
 */
rt_bool_t net_session_parse_hello(const char *data, int len,
                                  net_session_info_t *out);

#endif /* __NET_SESSION_H */
