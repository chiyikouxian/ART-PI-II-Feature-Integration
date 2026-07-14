/**
 * @file net_keepalive.h
 * @brief TCP keepalive + application-level PING/PONG watchdog.
 *
 * Two layers run side-by-side:
 *
 *   1. SO_KEEPALIVE with TCP_KEEPIDLE/INTVL/CNT — kernel-level probes
 *      that the lwIP stack honours. These catch a peer that has gone
 *      away without FIN/RST, but only after the keepalive timer (often
 *      several minutes on default lwIP tunings).
 *
 *   2. Application-level PING/PONG — every NET_KA_PING_PERIOD_MS the
 *      sender fires a "PING\n" heartbeat; if the peer is alive it must
 *      echo "PONG\n". If no PONG arrives within NET_KA_PONG_TIMEOUT_MS
 *      the link is considered half-open and the caller is told to
 *      reconnect.
 *
 * The peer (PC frontend / ROCK) is expected to echo PONG when it sees
 * PING. If a deployment can't do that, simply raise NET_KA_PONG_TIMEOUT_MS
 * and rely on TCP keepalive.
 */
#ifndef __NET_KEEPALIVE_H
#define __NET_KEEPALIVE_H

#include <rtthread.h>

/* Tunables. Defaults match a 90ms-cadence CSV stream: short pings
 * keep the link warm, but a 6s silence window means real congestion. */
#define NET_KA_PING_PERIOD_MS     2000
#define NET_KA_PONG_TIMEOUT_MS     6000
#define NET_KA_TCP_KEEPIDLE_S      5
#define NET_KA_TCP_KEEPINTVL_S     2
#define NET_KA_TCP_KEEPCNT         3

/**
 * @brief  Enable kernel-level TCP keepalive on @p sock with our defaults.
 *         Returns RT_EOK or negative errno on failure (lwIP may not
 *         support TCP_KEEPIDLE — falls back to a plain SO_KEEPALIVE).
 */
rt_err_t net_keepalive_enable(int sock);

/**
 * @brief  Mark a freshly-connected socket and start its heartbeat. Each
 *         call resets the watchdog timer. Re-call after every reconnect.
 *
 * @param  sock         Connected TCP socket.
 * @param  role         'C' for client (sends PING), 'S' for server
 *                      (only replies PONG). The right-hand boards are
 *                      always clients; the PC frontend uses 'C' too if
 *                      it talks to the board (out of scope here).
 */
rt_err_t net_keepalive_start(int sock, char role);

/**
 * @brief  Per-tick maintenance: emits a PING if it's been long enough,
 *         and checks whether NET_KA_PONG_TIMEOUT_MS has elapsed since
 *         the last received byte. Returns RT_EOK if the link is still
 *         considered healthy; -RT_ETIMEOUT if the caller should drop
 *         the socket and reconnect.
 *
 *         Cheap (a few microseconds), safe to call once per loop iteration.
 */
rt_err_t net_keepalive_tick(int sock);

/**
 * @brief  Notify the watchdog that bytes arrived on @p sock. Line callback
 *         and recv paths call this whenever they read anything.
 */
void net_keepalive_on_rx(int sock);

/**
 * @brief  Stop the heartbeat for @p sock (called from disconnect paths).
 */
void net_keepalive_stop(int sock);

/**
 * @brief  Test whether a payload line is a heartbeat (PING/PONG) and
 *         auto-reply if so. Returns RT_TRUE if the line was consumed
 *         (caller should NOT dispatch it as a normal command).
 *
 *         Usage from a line callback:
 *             if (net_keepalive_handle_line(sock, data, len))
 *                 return;
 *             // ... normal command dispatch ...
 */
rt_bool_t net_keepalive_handle_line(int sock, const char *data, int len);

/**
 * @brief  Stats snapshot for a single socket. sock <= 0 dumps a rolled-up
 *         summary across all active heartbeats.
 */
typedef struct {
    rt_uint32_t ping_sent;
    rt_uint32_t pong_sent;
    rt_uint32_t pong_recv;
    rt_uint32_t timeout_events;
    rt_uint32_t last_rx_age_ms;     /* 0xFFFFFFFF if never rx'd */
} net_keepalive_stats_t;

void net_keepalive_get_stats(int sock, net_keepalive_stats_t *out);

#endif /* __NET_KEEPALIVE_H */