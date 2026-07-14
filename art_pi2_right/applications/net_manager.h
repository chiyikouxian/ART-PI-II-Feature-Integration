/**
 * @file net_manager.h
 * @brief Independent WiFi link supervisor.
 *
 * The legacy autostart sequence connected WiFi exactly once and never
 * revisited it. If the AP rebooted, the signal faded past the roam
 * threshold, or DHCP lost its lease, business threads kept looping on
 * socket()/connect() without ever repairing the link itself.
 *
 * net_manager() runs in its own thread and owns:
 *   - a state machine that observes the WLAN subsystem
 *   - the only path that calls rt_wlan_connect()
 *   - a "link ready" semaphore that the TCP threads wait on instead
 *     of polling with multi-second sleeps
 *
 * Public surface:
 *   net_manager_start()  : boot the thread (called from main.c)
 *   net_manager_is_ready(): RT_TRUE iff IP is valid AND DHCP lease is fresh
 *   net_manager_wait_ready(timeout_ms): block until ready or timeout
 *   net_manager_notify_disconnected() : socket layer hints that the
 *       link is gone (e.g. connect() failed repeatedly) so the
 *       supervisor jumps state immediately rather than waiting for
 *       the next poll.
 */
#ifndef __NET_MANAGER_H
#define __NET_MANAGER_H

#include <rtthread.h>

rt_err_t net_manager_start(void);

/**
 * @brief  True when the WLAN link is up AND we have a routable IP.
 *         Cheap, safe to call from any thread.
 */
rt_bool_t net_manager_is_ready(void);

/**
 * @brief  Block up to @p timeout_ms until the link is ready.
 *         Returns RT_EOK on success, -RT_ETIMEOUT on expiry.
 */
rt_err_t net_manager_wait_ready(rt_int32_t timeout_ms);

/**
 * @brief  Hint from the socket layer that the link may be broken.
 *         Causes the supervisor to immediately leave WLAN_READY and
 *         transition to WLAN_CONNECTING.
 */
void net_manager_notify_disconnected(void);

/**
 * @brief  Read-only access to the current state, useful for diagnostics
 *         and the net_stat MSH command.
 */
typedef enum {
    NET_STATE_IDLE = 0,
    NET_STATE_CONNECTING,
    NET_STATE_NO_IP,          /* associated, no DHCP lease yet */
    NET_STATE_READY,          /* IP valid + link up */
    NET_STATE_LOST            /* detected drop, going back to CONNECTING */
} net_state_t;

const char *net_manager_state_name(net_state_t s);
net_state_t net_manager_get_state(void);

/**
 * @brief  Counters useful for fault triage. All counters are 32-bit
 *         and may wrap on very long uptimes; that's fine for the
 *         intended use (delta vs. last inspection).
 */
typedef struct {
    rt_uint32_t connect_attempts;   /* rt_wlan_connect() calls */
    rt_uint32_t connect_ok;         /* ones that succeeded */
    rt_uint32_t dhcp_acquired;      /* IP transitions to non-zero */
    rt_uint32_t link_lost_events;   /* RT_WLAN_EVT_STA_LOST received */
    rt_uint32_t manual_disconnect_hints; /* socket layer pushed us */
    rt_uint32_t time_in_ready_ms;   /* cumulative, low-precision */
} net_manager_stats_t;

void net_manager_get_stats(net_manager_stats_t *out);

#endif /* __NET_MANAGER_H */