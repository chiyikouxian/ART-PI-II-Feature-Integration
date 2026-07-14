/**
 * @file pc_discovery.h
 * @brief PC UDP Discovery module.
 *
 * Listens on 0.0.0.0:9108 for PC broadcasts of the form:
 *   ARTPI_PC,1,<tcp_port>\n
 *
 * The PC IP is extracted from recvfrom() source address (not from the
 * payload). When the TCP endpoint changes the shared server_config
 * generation increments, causing tcp_client to gracefully reconnect
 * without this module ever touching its socket.
 *
 * Design constraints
 * ------------------
 * - No dynamic memory allocation (static thread, static socket).
 * - All recvfrom(), Wi-Fi state checks, and server_config updates
 *   happen inside the discovery thread — no cross-thread socket or
 *   config races.
 * - pc_discovery does NOT call closesocket() on tcp_client's socket.
 * - Wi-Fi disconnect → close socket and loop on rt_wlan_is_ready();
 *   Wi-Fi reconnect → rebuild socket and continue.
 */
#ifndef __PC_DISCOVERY_H__
#define __PC_DISCOVERY_H__

#include <rtthread.h>

/**
 * @brief  Initialise the discovery module.  Must be called once from main()
 *         before any discovery thread starts.  Initialises the internal
 *         rt_event used to signal discovery events.
 * @return RT_EOK on success.
 */
int pc_discovery_init(void);

/**
 * @brief  Start the discovery thread (daemon). Idempotent.
 * @return RT_EOK on success, -RT_EFULL if already running.
 */
rt_err_t pc_discovery_start(void);

/**
 * @brief  Signal the discovery thread to stop and wait for it.
 * @return RT_EOK.
 */
rt_err_t pc_discovery_stop(void);

/**
 * @brief  True once a valid PC broadcast has been received at least once.
 */
rt_bool_t pc_discovery_has_server(void);

/**
 * @brief  Block until a PC is discovered or @p timeout_ms elapses.
 * @param  timeout_ms  Maximum wait; -1 means forever.
 * @return RT_EOK if found, -RT_ETIMEOUT on expiry,
 *         -RT_ERROR if stopped before a server was found.
 */
rt_err_t pc_discovery_wait_server(rt_int32_t timeout_ms);

/**
 * @brief  Statistics snapshot.  pkts_* fields are monotonic within a single
 *         start/stop cycle.  endpoint_changes, ip_changed, port_changed use
 *         an epoch counter so callers can detect "stats from a previous run".
 */
typedef struct {
    rt_uint32_t pkts_received;
    rt_uint32_t pkts_valid;
    rt_uint32_t pkts_invalid;
    rt_uint32_t endpoint_changes;
    rt_uint32_t ip_changed;
    rt_uint32_t port_changed;
    rt_uint32_t discovery_epoch;  /* increments each time start() is called */
    char        discovered_ip[16];
    int         discovered_port;
    char        device_name[8];
} pc_discovery_stats_t;

/**
 * @brief  Fill @p out with the current statistics.
 */
void pc_discovery_get_stats(pc_discovery_stats_t *out);

#endif /* __PC_DISCOVERY_H__ */
