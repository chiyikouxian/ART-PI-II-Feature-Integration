/**
 * @file net_keepalive.c
 * @brief TCP keepalive + application-level PING/PONG watchdog.
 *
 * Keepalive state is per-socket and stored in a small fixed table. The
 * watchdog ticks on every net_keepalive_tick() call, which the
 * producer loops already do once per frame, so we don't need a
 * dedicated timer thread.
 */

#include <rtthread.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>

#include "net_keepalive.h"
#include "net_io.h"

#define DBG_SECTION_NAME    "net_ka"
#define DBG_LEVEL           DBG_INFO
#include <rtdbg.h>

/* lwIP's setsockopt for TCP_KEEP* is only available when
 * LWIP_TCP_KEEPALIVE is enabled. Guard with ifdef so this module
 * still builds against stacks that lack it. */
#ifdef LWIP_TCP_KEEPALIVE
#define HAVE_TCP_KEEP_TUNABLES 1
#else
#define HAVE_TCP_KEEP_TUNABLES 0
#endif

#define NET_KA_MAX_SOCKETS 4

typedef struct {
    int          sock;
    char         role;            /* 'C' or 'S' */
    rt_tick_t    last_rx_tick;
    rt_tick_t    last_ping_tick;
    rt_uint32_t  ping_sent;
    rt_uint32_t  pong_sent;
    rt_uint32_t  pong_recv;
    rt_uint32_t  timeout_events;
} net_keepalive_slot_t;

static net_keepalive_slot_t g_slots[NET_KA_MAX_SOCKETS];
static rt_bool_t g_inited = RT_FALSE;

static net_keepalive_slot_t *find_slot(int sock)
{
    if (sock <= 0)
        return RT_NULL;

    for (int i = 0; i < NET_KA_MAX_SOCKETS; i++)
    {
        if (g_slots[i].sock == sock)
            return &g_slots[i];
    }
    return RT_NULL;
}

static net_keepalive_slot_t *alloc_slot(int sock, char role)
{
    net_keepalive_slot_t *free_slot = RT_NULL;
    for (int i = 0; i < NET_KA_MAX_SOCKETS; i++)
    {
        if (g_slots[i].sock == sock)
            return &g_slots[i];  /* already tracked */
        if (free_slot == RT_NULL && g_slots[i].sock <= 0)
            free_slot = &g_slots[i];
    }
    if (free_slot == RT_NULL)
    {
        LOG_W("net_ka: slot table full, sock=%d not tracked", sock);
        return RT_NULL;
    }
    rt_memset(free_slot, 0, sizeof(*free_slot));
    free_slot->sock = sock;
    free_slot->role = role;
    free_slot->last_rx_tick  = rt_tick_get();
    free_slot->last_ping_tick = rt_tick_get();
    return free_slot;
}

static void free_slot(int sock)
{
    net_keepalive_slot_t *s = find_slot(sock);
    if (s != RT_NULL)
    {
        rt_memset(s, 0, sizeof(*s));
        s->sock = -1;
    }
}

rt_err_t net_keepalive_enable(int sock)
{
    int on = 1;

    if (sock <= 0)
        return -RT_EINVAL;

    if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on)) < 0)
    {
        LOG_W("net_ka: SO_KEEPALIVE failed errno=%d", errno);
        return -RT_ERROR;
    }

#if HAVE_TCP_KEEP_TUNABLES
    int idle  = NET_KA_TCP_KEEPIDLE_S;
    int intvl = NET_KA_TCP_KEEPINTVL_S;
    int cnt   = NET_KA_TCP_KEEPCNT;
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE,  &idle,  sizeof(idle));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT,   &cnt,   sizeof(cnt));
#endif

    LOG_I("net_ka: sock=%d keepalive on (idle=%ds intvl=%ds cnt=%d)",
          sock,
#if HAVE_TCP_KEEP_TUNABLES
          NET_KA_TCP_KEEPIDLE_S, NET_KA_TCP_KEEPINTVL_S, NET_KA_TCP_KEEPCNT
#else
          0, 0, 0
#endif
    );
    return RT_EOK;
}

rt_err_t net_keepalive_start(int sock, char role)
{
    if (!g_inited)
    {
        rt_memset(g_slots, 0, sizeof(g_slots));
        for (int i = 0; i < NET_KA_MAX_SOCKETS; i++)
            g_slots[i].sock = -1;
        g_inited = RT_TRUE;
    }

    if (alloc_slot(sock, role) == RT_NULL)
        return -RT_ERROR;

    return net_keepalive_enable(sock);
}

void net_keepalive_stop(int sock)
{
    free_slot(sock);
}

void net_keepalive_on_rx(int sock)
{
    net_keepalive_slot_t *s = find_slot(sock);
    if (s != RT_NULL)
        s->last_rx_tick = rt_tick_get();
}

rt_err_t net_keepalive_tick(int sock)
{
    net_keepalive_slot_t *s = find_slot(sock);
    if (s == RT_NULL)
        return RT_EOK;

    rt_tick_t now = rt_tick_get();

    /* Stale check: any silence > NET_KA_PONG_TIMEOUT_MS is treated as
     * half-open regardless of whether we are the PINGer. */
    rt_tick_t silence_ms = (now - s->last_rx_tick) * 1000 / RT_TICK_PER_SECOND;
    if (silence_ms > NET_KA_PONG_TIMEOUT_MS)
    {
        s->timeout_events++;
        LOG_W("net_ka: sock=%d stale %u ms (no rx), disconnect", sock, silence_ms);
        return -RT_ETIMEOUT;
    }

    if (s->role != 'C')
        return RT_EOK;  /* server-side: no PINGs of our own */

    rt_tick_t since_ping = (now - s->last_ping_tick) * 1000 / RT_TICK_PER_SECOND;
    if (since_ping < NET_KA_PING_PERIOD_MS)
        return RT_EOK;

    if (net_io_send_frame(sock, "PING\n", 5) == RT_EOK)
    {
        s->ping_sent++;
        s->last_ping_tick = now;
        LOG_D("net_ka: PING -> sock=%d", sock);
    }
    else
    {
        LOG_W("net_ka: PING send failed, will retry next tick");
    }

    return RT_EOK;
}

rt_bool_t net_keepalive_handle_line(int sock, const char *data, int len)
{
    if (data == RT_NULL || len <= 0)
        return RT_FALSE;

    /* Match "PING", "PING\r", "PING\n", "PING\r\n" etc. */
    int ping_len = 4;
    if (len >= ping_len && rt_strncmp(data, "PING", ping_len) == 0)
    {
        int tail = len - ping_len;
        while (tail > 0 && (data[len - tail] == '\r' || data[len - tail] == '\n' ||
                            data[len - tail] == ' '  || data[len - tail] == '\t'))
            tail--;
        if (tail == 0)
        {
            net_keepalive_on_rx(sock);
            net_io_send_frame(sock, "PONG\n", 5);
            net_keepalive_slot_t *s = find_slot(sock);
            if (s != RT_NULL) s->pong_sent++;
            LOG_D("net_ka: PING -> PONG (sock=%d)", sock);
            return RT_TRUE;
        }
    }

    /* Match "PONG" so the client can mark pong_recv. */
    if (len >= 4 && rt_strncmp(data, "PONG", 4) == 0)
    {
        int tail = len - 4;
        while (tail > 0 && (data[len - tail] == '\r' || data[len - tail] == '\n' ||
                            data[len - tail] == ' '  || data[len - tail] == '\t'))
            tail--;
        if (tail == 0)
        {
            net_keepalive_on_rx(sock);
            net_keepalive_slot_t *s = find_slot(sock);
            if (s != RT_NULL) s->pong_recv++;
            LOG_D("net_ka: PONG received (sock=%d)", sock);
            return RT_TRUE;
        }
    }

    return RT_FALSE;
}

void net_keepalive_get_stats(int sock, net_keepalive_stats_t *out)
{
    if (out == RT_NULL)
        return;

    rt_memset(out, 0, sizeof(*out));
    out->last_rx_age_ms = 0xFFFFFFFFu;

    if (sock <= 0)
    {
        for (int i = 0; i < NET_KA_MAX_SOCKETS; i++)
        {
            if (g_slots[i].sock <= 0) continue;
            out->ping_sent      += g_slots[i].ping_sent;
            out->pong_sent      += g_slots[i].pong_sent;
            out->pong_recv      += g_slots[i].pong_recv;
            out->timeout_events += g_slots[i].timeout_events;
        }
        return;
    }

    net_keepalive_slot_t *s = find_slot(sock);
    if (s == RT_NULL) return;
    out->ping_sent       = s->ping_sent;
    out->pong_sent       = s->pong_sent;
    out->pong_recv       = s->pong_recv;
    out->timeout_events  = s->timeout_events;
    rt_tick_t now = rt_tick_get();
    out->last_rx_age_ms  = (now - s->last_rx_tick) * 1000 / RT_TICK_PER_SECOND;
}