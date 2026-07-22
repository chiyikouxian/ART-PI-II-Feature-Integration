/**
 * @file net_manager.c
 * @brief WiFi link supervisor thread.
 *
 * Design
 * ------
 * net_manager sits between the RT-Thread WLAN driver and the TCP
 * workers. It owns the only path that calls rt_wlan_connect(), and it
 * keeps a "ready" semaphore that the TCP threads park on instead of
 * burning CPU in 3-second connect retries.
 *
 * The state machine has four stable states (see net_manager.h) and a
 * single transition table:
 *
 *     IDLE ── start ──► CONNECTING
 *     CONNECTING ── assoc OK + IP ──► READY
 *     CONNECTING ── assoc OK, no IP ──► NO_IP (retry DHCP up to N times)
 *     READY ── disconnect event ──► LOST ──► CONNECTING
 *     NO_IP ── disconnect event ──► CONNECTING
 *     any ── manual_disconnect hint ──► LOST ──► CONNECTING
 *
 * The thread is intentionally conservative: it never tears down the
 * TCP workers by itself; instead it makes them wait on the semaphore.
 * That keeps the socket-layer fix (commit 1) orthogonal to the link
 * fix (this commit).
 */

#include <rtthread.h>
#include <wlan_mgnt.h>
#include <wlan_prot.h>
#include <wlan_cfg.h>

#include "net_manager.h"

#define DBG_SECTION_NAME    "net_mgr"
#define DBG_LEVEL           DBG_INFO
#include <rtdbg.h>

/* WiFi connection source for the right hand.
 * NET_MGR_SSID / NET_MGR_PASSWORD are the authoritative credentials
 * used by net_manager when calling rt_wlan_connect().
 * Plan B: ROCK self-hosted AP (hotspot iface wlan1, hotspot IP
 * 192.168.1.1/24). These credentials are field-verified against the
 * actual ROCK hotspot. */
#define NET_MGR_SSID        "rockchip_4eabbe"
#define NET_MGR_PASSWORD    "rockchip"

#define NET_MGR_THREAD_STACK_SIZE   2048
#define NET_MGR_THREAD_PRIORITY     10   /* low: should never preempt TCP */
#define NET_MGR_THREAD_TIMESLICE    10

#define NET_MGR_POLL_INTERVAL_MS    500
#define NET_MGR_CONNECT_COOLDOWN_MS 3000
#define NET_MGR_DHCP_TIMEOUT_MS     15000

/* Track whether we issued rt_wlan_connect() ourselves. The driver
 * fires RT_WLAN_EVT_STA_CONNECTED on every association including
 * the initial boot-time join, so we have to disambiguate. */
static rt_bool_t            g_running = RT_FALSE;
static rt_thread_t          g_thread  = RT_NULL;
static net_state_t          g_state   = NET_STATE_IDLE;

static struct rt_semaphore  g_ready_sem;     /* posted on READY entry */
static rt_bool_t            g_sem_inited = RT_FALSE;

static volatile rt_bool_t   g_force_disconnect = RT_FALSE;

static net_manager_stats_t  g_stats;

static const char *state_names[] = {
    "IDLE", "CONNECTING", "NO_IP", "READY", "LOST"
};

const char *net_manager_state_name(net_state_t s)
{
    if ((unsigned)s >= sizeof(state_names)/sizeof(state_names[0]))
        return "UNKNOWN";
    return state_names[s];
}

net_state_t net_manager_get_state(void)
{
    return g_state;
}

void net_manager_get_stats(net_manager_stats_t *out)
{
    if (out == RT_NULL)
        return;
    *out = g_stats;
}

void net_manager_notify_disconnected(void)
{
    g_force_disconnect = RT_TRUE;
    g_stats.manual_disconnect_hints++;
}

rt_bool_t net_manager_is_ready(void)
{
    return (g_state == NET_STATE_READY) && rt_wlan_is_ready();
}

rt_err_t net_manager_wait_ready(rt_int32_t timeout_ms)
{
    rt_err_t ret;

    if (!g_sem_inited)
        return -RT_ERROR;

    if (timeout_ms < 0)
    {
        ret = rt_sem_take(&g_ready_sem, RT_WAITING_FOREVER);
    }
    else
    {
        rt_tick_t ticks = (rt_tick_t)timeout_ms * RT_TICK_PER_SECOND / 1000;
        if (ticks == 0)
            ticks = 1;
        ret = rt_sem_take(&g_ready_sem, ticks);
    }

    /* Re-post immediately if we're still in READY so the next caller
     * doesn't block. This makes the semaphore behave like a "level"
     * trigger rather than edge. */
    if (ret == RT_EOK && net_manager_is_ready())
    {
        rt_sem_release(&g_ready_sem);
    }
    else if (ret == RT_EOK)
    {
        /* We got the token but already left READY; treat as miss. */
        ret = -RT_ETIMEOUT;
    }

    return ret;
}

static void enter_state(net_state_t s)
{
    if (g_state == s)
        return;

    LOG_I("net_mgr: %s -> %s",
          net_manager_state_name(g_state),
          net_manager_state_name(s));

    g_state = s;

    if (s == NET_STATE_READY)
    {
        g_stats.dhcp_acquired++;
        if (g_sem_inited)
            rt_sem_release(&g_ready_sem);
    }
}

/* Called from RT-Thread WLAN event thread context. Must be tiny.
 *
 * The first parameter must be typed as plain `int`, not `rt_wlan_event_t`.
 * RT-Thread's wlan_mgnt.h typedef is:
 *
 *     typedef void (*rt_wlan_event_handler)(int event, ...);
 *
 * — note the `int`, not the enum. armclang enforces strict function-pointer
 * compatibility, so passing a function with `rt_wlan_event_t` to
 * rt_wlan_register_event_handler() is rejected with
 *   "incompatible function pointer types ... passing ... to parameter of type
 *    'rt_wlan_event_handler' (aka 'void (*)(int, struct rt_wlan_buff *, void *)')".
 * Enum case labels inside the switch below still work because enum constants
 * decay to int when matched against an int control expression. */
static void on_wlan_event(int event, struct rt_wlan_buff *buff, void *parameter)
{
    (void)buff;
    (void)parameter;

    switch (event)
    {
    case RT_WLAN_EVT_STA_CONNECTED:
        /* We only count this as success if we initiated the connect
         * ourselves. Boot-time users don't go through net_manager. */
        g_stats.connect_ok++;
        break;

    case RT_WLAN_EVT_STA_DISCONNECTED:
        g_stats.link_lost_events++;
        if (g_state == NET_STATE_READY || g_state == NET_STATE_NO_IP)
            enter_state(NET_STATE_LOST);
        break;

    default:
        break;
    }
}

/**
 * @brief  Poll once, advance the state machine, return ms until next poll.
 */
static rt_int32_t step(void)
{
    rt_bool_t associated = rt_wlan_is_connected();
    rt_bool_t has_ip     = rt_wlan_is_ready();

    switch (g_state)
    {
    case NET_STATE_IDLE:
    case NET_STATE_LOST:
    {
        rt_tick_t backoff = NET_MGR_CONNECT_COOLDOWN_MS;

        if (g_force_disconnect)
        {
            g_force_disconnect = RT_FALSE;
            LOG_I("net_mgr: forced reconnect");
        }

        if (associated)
        {
            /* Driver already joined (boot-time join). Skip connect(). */
            if (has_ip)
                enter_state(NET_STATE_READY);
            else
                enter_state(NET_STATE_NO_IP);
            return NET_MGR_POLL_INTERVAL_MS;
        }

        LOG_I("net_mgr: connecting to %s ...", NET_MGR_SSID);
        g_stats.connect_attempts++;
        rt_err_t r = rt_wlan_connect(NET_MGR_SSID, NET_MGR_PASSWORD);
        if (r != RT_EOK)
        {
            LOG_W("net_mgr: rt_wlan_connect err=%d, retry later", r);
            backoff = NET_MGR_CONNECT_COOLDOWN_MS * 2;
        }
        enter_state(NET_STATE_CONNECTING);
        return backoff;
    }

    case NET_STATE_CONNECTING:
    case NET_STATE_NO_IP:
        if (!associated)
        {
            enter_state(NET_STATE_LOST);
            return NET_MGR_CONNECT_COOLDOWN_MS;
        }
        if (has_ip)
        {
            enter_state(NET_STATE_READY);
            return NET_MGR_POLL_INTERVAL_MS;
        }
        return NET_MGR_POLL_INTERVAL_MS;

    case NET_STATE_READY:
        /* Track cumulative time in READY for diagnostics. The +/- 500ms
         * tick granularity is fine for the net_stat MSH output. */
        g_stats.time_in_ready_ms += NET_MGR_POLL_INTERVAL_MS;

        if (g_force_disconnect)
        {
            g_force_disconnect = RT_FALSE;
            enter_state(NET_STATE_LOST);
            return NET_MGR_CONNECT_COOLDOWN_MS;
        }
        if (!associated)
        {
            enter_state(NET_STATE_LOST);
            return NET_MGR_CONNECT_COOLDOWN_MS;
        }
        if (!has_ip)
        {
            LOG_W("net_mgr: link up but IP lost, refreshing DHCP");
            enter_state(NET_STATE_NO_IP);
            return NET_MGR_POLL_INTERVAL_MS;
        }
        return NET_MGR_POLL_INTERVAL_MS;
    }

    return NET_MGR_POLL_INTERVAL_MS;
}

static void net_manager_thread_entry(void *parameter)
{
    (void)parameter;
    rt_int32_t wait_ms = NET_MGR_POLL_INTERVAL_MS;

    LOG_I("net_mgr: thread started");

    /* Register WLAN event listener so we react quickly to driver-level
     * disconnect events. rt_wlan_register_event_handler is RT-Thread's
     * official hook (see wlan_mgnt.h). */
    rt_wlan_register_event_handler(RT_WLAN_EVT_STA_DISCONNECTED,
                                   on_wlan_event, RT_NULL);
    rt_wlan_register_event_handler(RT_WLAN_EVT_STA_CONNECTED,
                                   on_wlan_event, RT_NULL);

    /* If the boot-time autostart already finished, we begin in the
     * whatever state the driver is in. Step() will figure it out. */
    while (g_running)
    {
        wait_ms = step();
        rt_thread_mdelay(wait_ms);
    }

    LOG_I("net_mgr: thread stopped");
}

rt_err_t net_manager_start(void)
{
    if (g_running)
        return RT_EOK;

    if (!g_sem_inited)
    {
        rt_sem_init(&g_ready_sem, "net_rdy", 0, RT_IPC_FLAG_PRIO);
        g_sem_inited = RT_TRUE;
    }

    g_running = RT_TRUE;

    g_thread = rt_thread_create("net_mgr",
                                net_manager_thread_entry,
                                RT_NULL,
                                NET_MGR_THREAD_STACK_SIZE,
                                NET_MGR_THREAD_PRIORITY,
                                NET_MGR_THREAD_TIMESLICE);
    if (g_thread == RT_NULL)
    {
        g_running = RT_FALSE;
        return -RT_ERROR;
    }
    rt_thread_startup(g_thread);
    return RT_EOK;
}