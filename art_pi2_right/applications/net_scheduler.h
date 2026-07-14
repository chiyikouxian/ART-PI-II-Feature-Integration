/**
 * @file net_scheduler.h
 * @brief Lightweight congestion signal for two-path scheduling.
 *
 * Both TCP paths (PC frontend on tcp_client, ROCK raw-data on
 * imu_wifi_sender) run at different cadences and share the same WiFi
 * NIC + lwIP stack. Under load we need a way to:
 *   1. Detect that the shared link is congested (keepalive silence)
 *   2. Degrade the lower-priority path (PC frontend) by skipping frames
 *      when the higher-priority path (ROCK inference) needs bandwidth
 *
 * Design
 * ------
 * The congestion signal is "last meaningful data age": if the keepalive
 * tick on either path measured a long silence, the link is considered
 * loaded. TCP's own congestion window also backs off, but this
 * application-level signal lets us skip frames proactively.
 *
 * Priority levels:
 *   PRIORITY_HIGH   = ROCK raw-data path (inference, 90ms cadence)
 *   PRIORITY_NORMAL = PC frontend JSON path (display, 100ms cadence)
 *
 * When the shared link shows congestion, the NORMAL path skips its
 * next frame and tries again the cycle after. The HIGH path never
 * skips (we always try to send, just with the send_all deadline).
 *
 * Counters: skips_requested / skips_actually_skipped on the NORMAL path.
 */
#ifndef __NET_SCHEDULER_H
#define __NET_SCHEDULER_H

#include <rtthread.h>

typedef enum {
    NET_PRIO_NORMAL = 0,
    NET_PRIO_HIGH   = 1,
} net_link_prio_t;

/**
 * @brief  Register the keepalive silence as a congestion signal.
 *         Both paths call this on every keepalive tick so the shared
 *         signal reflects the worst-case (busiest) path.
 *
 * @param  busy_ms  Silence measured on this path in the last tick
 *                  (0 if data arrived recently).
 */
void net_scheduler_on_tick(rt_uint32_t busy_ms);

/**
 * @brief  Ask the scheduler whether to skip the current frame for a
 *         given priority level. HIGH always returns RT_FALSE.
 *         NORMAL returns RT_TRUE (skip) when the link has been
 *         congested for more than NET_SCHED_CONGEST_THRESH_MS.
 *
 * @param  prio  PRIORITY_HIGH or PRIORITY_NORMAL
 */
rt_bool_t net_scheduler_should_skip(net_link_prio_t prio);

/**
 * @brief  Read-only snapshot of scheduler counters.
 */
typedef struct {
    rt_uint32_t congested_ticks;   /* ticks with silence > threshold */
    rt_uint32_t skips_requested;  /* NORMAL path asked to skip */
} net_scheduler_stats_t;

void net_scheduler_get_stats(net_scheduler_stats_t *out);

#endif /* __NET_SCHEDULER_H */