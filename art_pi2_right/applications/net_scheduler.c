/**
 * @file net_scheduler.c
 * @brief Lightweight two-path congestion signal.
 */

#include <rtthread.h>
#include "net_scheduler.h"

#define DBG_SECTION_NAME    "net_sched"
#define DBG_LEVEL           DBG_INFO
#include <rtdbg.h>

/* Silence > 1.5 × nominal cadence on the busy path means the link is
 * actively backing off. This is a heuristic; TCP's own CWD matters more,
 * but we want the application to know before send_all starts blocking. */
#define NET_SCHED_CONGEST_THRESH_MS  160

static rt_uint32_t g_congested_ticks = 0;
static rt_uint32_t g_skips_requested = 0;
static rt_bool_t   g_congested      = RT_FALSE;

void net_scheduler_on_tick(rt_uint32_t busy_ms)
{
    if (busy_ms >= NET_SCHED_CONGEST_THRESH_MS)
    {
        if (!g_congested)
        {
            LOG_W("net_sched: link congested (busy_ms=%u)", busy_ms);
            g_congested = RT_TRUE;
        }
        g_congested_ticks++;
    }
    else
    {
        g_congested = RT_FALSE;
    }
}

rt_bool_t net_scheduler_should_skip(net_link_prio_t prio)
{
    if (prio == NET_PRIO_HIGH)
        return RT_FALSE;  /* ROCK path: never skip */

    /* NORMAL path: skip one frame when the shared link is congested.
     * We count the skip request so the net_stat MSH output shows how
     * often this fires. */
    if (g_congested)
    {
        g_skips_requested++;
        return RT_TRUE;
    }
    return RT_FALSE;
}

void net_scheduler_get_stats(net_scheduler_stats_t *out)
{
    if (out == RT_NULL) return;
    out->congested_ticks = g_congested_ticks;
    out->skips_requested = g_skips_requested;
}