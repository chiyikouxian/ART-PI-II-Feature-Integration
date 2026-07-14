/**
 * @file net_session.c
 * @brief Per-TCP-connection session identity.
 *
 * Design
 * ------
 * Two TCP paths share the same module:
 *   - PC path  (role 'C'): announces session, sends HELLO so the PC can
 *                          flush stale cache. Does NOT reset frame_seq.
 *   - ROCK path (role 'S'): announces session, sends HELLO so the ROCK
 *                           consumer can flush stale cache. DOES reset
 *                           frame_seq to prevent new-session seqs colliding
 *                           with any cache the peer still holds.
 *
 * A single global slot is fine because the two paths never call
 * net_session_announce() concurrently (each is its own thread, and the
 * sockets are different file descriptors). Concurrent calls from
 * net_manager / MSH commands are gated by the socket lifecycle.
 */

#include <rtthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "net_session.h"
#include "net_io.h"

#define DBG_SECTION_NAME    "net_sess"
#define DBG_LEVEL           DBG_INFO
#include <rtdbg.h>

static net_session_info_t  g_info;
static rt_bool_t           g_inited = RT_FALSE;

/* Issue 4 fix: protect g_info updates. Both tcp_client (PC) and
 * imu_wifi_sender (ROCK) call net_session_announce() from their own
 * threads, and net_io_send_line() can block on a slow socket. Without
 * this lock, a second thread's call would observe a partially-written
 * g_info.side / g_info.session_id. */
static struct rt_mutex      g_info_lock;

/* Per-role seq_reset callbacks — set by the respective workers. */
static net_session_seq_reset_t g_seq_reset_rock = RT_NULL;
static net_session_seq_reset_t g_seq_reset_pc  = RT_NULL;

static rt_bool_t            g_lock_inited = RT_FALSE;

/* -------- boot_id derivation ----------------------------------------
 * We don't want to depend on a particular vendor's UID_BASE register,
 * so boot_id is derived once from RT_TICK + a tiny PRNG seed. It's
 * stable across the boot (call it once and cache), and "different
 * enough" across boots for cache-key purposes.
 */
static rt_uint32_t derive_boot_id(void)
{
    static rt_uint32_t cached = 0;
    if (cached != 0)
        return cached;

    /* Mix a few low-entropy sources. RT_TICK at boot is a fine nonce. */
    rt_uint32_t s = (rt_uint32_t)rt_tick_get();
    s ^= (rt_uint32_t)rt_strlen("art_pi2") * 2654435761u;
    s ^= 0xA5A5A5A5u;
    if (s == 0) s = 1;
    cached = s;
    return s;
}

static rt_uint32_t next_session_id(void)
{
    /* 32-bit LCG seeded by boot_id. */
    static rt_uint32_t s = 0;
    if (s == 0)
    {
        s = derive_boot_id();
        if (s == 0) s = 1;
    }
    s = s * 1664525u + 1013904223u;
    if (s == 0) s = 1;
    return s;
}

void net_session_init(void)
{
    /* Issue 4 fix: initialise the mutex once, up front, so the two
     * worker threads can never race on the lazy-init. Must be called
     * from the main thread before either tcp_client or imu_wifi_sender
     * start. */
    if (g_lock_inited)
        return;
    rt_mutex_init(&g_info_lock, "net_sess", RT_IPC_FLAG_PRIO);
    g_lock_inited = RT_TRUE;

    rt_mutex_take(&g_info_lock, RT_WAITING_FOREVER);
    if (!g_inited || g_info.boot_id == 0)
    {
        g_info.boot_id     = derive_boot_id();
        g_info.started_tick = rt_tick_get();
        g_info.side        = '?';
        g_info.session_id  = 0;
        g_info.last_announce_tick = 0;
        g_info.announce_count     = 0;
        g_info.announce_fail_count = 0;
        g_inited = RT_TRUE;
    }
    rt_mutex_release(&g_info_lock);

    LOG_I("net_sess: init OK boot_id=%08x", g_info.boot_id);
}

void net_session_register_seq_reset_rock(net_session_seq_reset_t fn)
{
    g_seq_reset_rock = fn;
}

void net_session_register_seq_reset_pc(net_session_seq_reset_t fn)
{
    g_seq_reset_pc = fn;
}

const net_session_info_t *net_session_current(void)
{
    if (!g_inited)
    {
        /* Defensive: if a caller asks before init, return a stub but
         * don't write through the (possibly uninitialised) mutex. */
        static net_session_info_t stub;
        stub.boot_id = derive_boot_id();
        stub.side = '?';
        stub.session_id = 0;
        stub.started_tick = 0;
        stub.last_announce_tick = 0;
        stub.announce_count = 0;
        stub.announce_fail_count = 0;
        return &stub;
    }
    rt_mutex_take(&g_info_lock, RT_WAITING_FOREVER);
    static net_session_info_t copy;
    copy = g_info;
    rt_mutex_release(&g_info_lock);
    return &copy;
}

rt_err_t net_session_announce(int sock, char side)
{
    char line[96];
    rt_uint32_t boot_id_snapshot;
    rt_uint32_t session_id_snapshot;

    if (sock <= 0)
        return -RT_ERROR;

    if (side != 'C' && side != 'S')
    {
        LOG_W("net_sess: invalid role '%c' (expected 'C' or 'S')", side);
        return -RT_ERROR;
    }

    /* Issue 4 fix: the mutex must be initialised by net_session_init()
     * before any worker thread runs. We no longer lazily create it
     * here because two workers could simultaneously see g_lock_inited
     * == RT_FALSE and double-init the same object. */
    if (!g_lock_inited)
    {
        LOG_E("net_sess: net_session_init() was not called");
        return -RT_ERROR;
    }

    /* Build the HELLO payload under the lock so a concurrent caller
     * doesn't observe an in-progress g_info write. */
    rt_mutex_take(&g_info_lock, RT_WAITING_FOREVER);

    if (!g_inited || g_info.boot_id == 0)
    {
        g_info.boot_id   = derive_boot_id();
        g_info.started_tick = rt_tick_get();
        g_inited = RT_TRUE;
    }

    g_info.side       = side;
    g_info.session_id = next_session_id();
    boot_id_snapshot  = g_info.boot_id;
    session_id_snapshot = g_info.session_id;

    rt_snprintf(line, sizeof(line),
                "HELLO,%c,%08x,%08x\n",
                side, boot_id_snapshot, session_id_snapshot);

    /* net_io_send_line() can block; the peer may be slow. We deliberately
     * do NOT hold the lock across the send so the other worker can
     * prepare its own announce. The HELLO string we just composed
     * contains all the values it needs and is independent of g_info. */
    rt_mutex_release(&g_info_lock);

    if (net_io_send_line(sock, line) != RT_EOK)
    {
        /* Capture the failure under the lock so MSH/observability can see it.
         * We still return error so the worker can tear down the socket. */
        rt_mutex_take(&g_info_lock, RT_WAITING_FOREVER);
        g_info.announce_fail_count++;
        rt_mutex_release(&g_info_lock);

        LOG_W("net_sess: HELLO send failed");
        return -RT_ERROR;
    }

    /* Issue: stamp HELLO metadata for observability. announce_count and
     * last_announce_tick let net_stat / MSH show "last HELLO X ms ago"
     * and "N successful announces so far" without polling the link. */
    {
        rt_tick_t now_tick = rt_tick_get();
        rt_mutex_take(&g_info_lock, RT_WAITING_FOREVER);
        g_info.last_announce_tick = now_tick;
        g_info.announce_count++;
        rt_mutex_release(&g_info_lock);
    }

    LOG_I("net_sess: HELLO sent role=%c boot=%08x session=%08x",
          side, boot_id_snapshot, session_id_snapshot);

    /* Issue 4 fix: branch on the LOCAL parameter `side`, never on the
     * shared g_info.side. The previous code re-read g_info.side after
     * the send — if the other worker announced in between, this would
     * dispatch to the wrong callback and reset the wrong frame_seq. */
    if (side == 'S' && g_seq_reset_rock != RT_NULL)
    {
        g_seq_reset_rock();
    }
    else if (side == 'C' && g_seq_reset_pc != RT_NULL)
    {
        g_seq_reset_pc();
    }

    return RT_EOK;
}

rt_bool_t net_session_parse_hello(const char *data, int len,
                                  net_session_info_t *out)
{
    if (data == RT_NULL || len <= 0)
        return RT_FALSE;

    /* Match "HELLO,<side>,<boot>,<session>" with optional trailing
     * whitespace. We don't pull in sscanf because the field widths
     * are tiny and we want deterministic behaviour. */
    static const char prefix[] = "HELLO,";
    int prefix_len = (int)sizeof(prefix) - 1;
    if (len < prefix_len) return RT_FALSE;
    if (rt_strncmp(data, prefix, prefix_len) != 0) return RT_FALSE;

    const char *p = data + prefix_len;
    int rem = len - prefix_len;

    /* side */
    if (rem < 1) return RT_FALSE;
    char side = p[0];
    /* Accept 'L'/'R' (legacy) and 'C'/'S' (current role identifiers). */
    if (side != 'L' && side != 'R' && side != 'C' && side != 'S') return RT_FALSE;
    p += 1; rem -= 1;
    if (rem < 1 || p[0] != ',') return RT_FALSE;
    p += 1; rem -= 1;

    /* boot_id (up to 8 hex chars) */
    char buf[16];
    int  i = 0;
    while (rem > 0 && i < (int)sizeof(buf) - 1 && p[0] != ',')
    {
        buf[i++] = p[0];
        p += 1; rem -= 1;
    }
    buf[i] = '\0';
    if (i == 0) return RT_FALSE;
    if (rem < 1 || p[0] != ',') return RT_FALSE;
    p += 1; rem -= 1;

    char *endp = RT_NULL;
    unsigned long boot = strtoul(buf, &endp, 16);
    if (endp == buf) return RT_FALSE;

    /* session_id */
    i = 0;
    while (rem > 0 && i < (int)sizeof(buf) - 1 &&
           p[0] != '\r' && p[0] != '\n' && p[0] != ' ' && p[0] != '\t')
    {
        buf[i++] = p[0];
        p += 1; rem -= 1;
    }
    buf[i] = '\0';
    if (i == 0) return RT_FALSE;

    unsigned long session = strtoul(buf, &endp, 16);
    if (endp == buf) return RT_FALSE;

    if (out != RT_NULL)
    {
        out->side      = side;
        out->boot_id   = (rt_uint32_t)boot;
        out->session_id = (rt_uint32_t)session;
        out->started_tick = rt_tick_get();
    }
    return RT_TRUE;
}