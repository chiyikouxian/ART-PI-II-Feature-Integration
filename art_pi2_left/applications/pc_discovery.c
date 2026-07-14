/**
 * @file pc_discovery.c
 * @brief Left-hand PC UDP Discovery implementation.
 *
 * Protocol: recv "ARTPI_PC,1,<tcp_port>\n" on 0.0.0.0:9108.
 * PC IP is taken from recvfrom() source sockaddr_in — never trusted
 * from inside the packet payload.
 *
 * No dynamic memory. Static thread, static socket, static buffers.
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <wlan_mgnt.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

#include "pc_discovery.h"
#include "server_config.h"

#define DBG_SECTION_NAME    "pc_disc"
#define DBG_LEVEL           DBG_INFO
#include <rtdbg.h>

#define DISCOVERY_PORT      9108
#define DISCOVERY_THREAD_PRIORITY   24
#define DISCOVERY_THREAD_STACK_SIZE  2048
#define DISCOVERY_THREAD_TIMESLICE   10
#define DISCOVERY_THREAD_TICK_MS     500   /* socket recv timeout / poll interval */
#define DISCOVERY_MAGIC             "ARTPI_PC"
#define DISCOVERY_VERSION            1

/* Static state — no dynamic allocation */
static rt_bool_t           g_discovery_running = RT_FALSE;
static rt_thread_t         g_discovery_thread = RT_NULL;
static int                 g_discovery_sock   = -1;

/* Last validated PC endpoint */
static char                g_last_ip[16]      = {0};
static int                 g_last_port        = 0;

/* Event: initialised once by pc_discovery_init() before any thread runs.
 * pc_discovery_start() resets it via rt_event_control(RT_IPC_CMD_RESET)
 * on each fresh run.  wait_server() blocks on this. */
static struct rt_event     g_discovery_event;
static rt_bool_t           g_event_inited = RT_FALSE;

/* Guards stop() vs. the discovery thread to avoid sending on a RESET event. */
static rt_bool_t           g_discovery_stopping = RT_FALSE;

#define DISCOVERY_EVENT_FOUND   (1 << 0)
#define DISCOVERY_EVENT_STOPPED (1 << 1)  /* sent by stop() to wake wait_server */

/* Statistics (no lock needed — single thread updates, MSH reads copy) */
static pc_discovery_stats_t g_stats;
static char g_stats_ip_buf[16] = {0};

/* Forward declaration of the MSH command helper */
static void cmd_pc_disc_stat(int argc, char **argv);

/* ---- Event management ---- */

/** Reset the discovery event (clears FOUND and STOPPED bits).
 *  Called from pc_discovery_start() before each fresh run.
 *  rt_event_control(RT_IPC_CMD_RESET) wakes all waiting threads with
 *  RT_ERROR, so wait_server() returns quickly and re-checks g_last_ip. */
static void reset_event(void)
{
    rt_event_control(&g_discovery_event, RT_IPC_CMD_RESET, RT_NULL);
}

/** Initialise the discovery module.  Called once from main() before any
 *  discovery thread starts. */
int pc_discovery_init(void)
{
    if (!g_event_inited)
    {
        (void)rt_event_init(&g_discovery_event, "pc_disc_evt", RT_IPC_FLAG_PRIO);
        g_event_inited = RT_TRUE;
    }
    return 0;
}

/* ---- server_config integration ---- */

/** Called when a new valid PC endpoint is found. */
static rt_err_t on_pc_endpoint_found(const char *ip, int port)
{
    int changed = 0;
    int ret;

    /* Step 1: update server_config FIRST — this commits the endpoint atomically
     * and increments the generation counter.  TCP client will only see a
     * consistent IP+port+generation snapshot from this point on. */
    ret = server_config_update_tcp_endpoint(ip, port);
    if (ret < 0)
    {
        LOG_E("failed to update server_config: %d", ret);
        return ret;
    }
    if (ret == 1)
        changed = 1;  /* generation incremented */

    /* Step 2: now that server_config is ready, publish g_last_ip so that
     * pc_discovery_has_server() returns true and wait_server() can unblock.
     * All subsequent reads of g_last_ip are guaranteed to see a server_config
     * that has already been updated. */
    if (g_last_ip[0] == '\0')
    {
        /* First discovery ever */
        rt_strncpy(g_last_ip, ip, sizeof(g_last_ip) - 1);
        g_last_ip[sizeof(g_last_ip) - 1] = '\0';
        g_last_port = port;
        g_stats.endpoint_changes = 1;
        g_stats.ip_changed = 1;
        g_stats.port_changed = 1;
        g_stats.discovered_port = port;
        rt_strncpy(g_stats.discovered_ip, ip, sizeof(g_stats.discovered_ip) - 1);
        g_stats.discovered_ip[sizeof(g_stats.discovered_ip) - 1] = '\0';
        LOG_I("PC discovered first time: %s:%d", ip, port);
        /* Signal the event — wait_server() may be blocked.
         * Do NOT send if stop() is in progress (avoids send-on-detached-event). */
        if (!g_discovery_stopping)
            rt_event_send(&g_discovery_event, DISCOVERY_EVENT_FOUND);
    }
    else
    {
        /* Subsequent change */
        if (rt_strncmp(g_last_ip, ip, sizeof(g_last_ip)) != 0)
        {
            g_stats.ip_changed++;
            rt_strncpy(g_last_ip, ip, sizeof(g_last_ip) - 1);
            g_last_ip[sizeof(g_last_ip) - 1] = '\0';
            rt_strncpy(g_stats.discovered_ip, ip, sizeof(g_stats.discovered_ip) - 1);
            g_stats.discovered_ip[sizeof(g_stats.discovered_ip) - 1] = '\0';
            LOG_I("PC IP changed: -> %s", ip);
        }
        if (g_last_port != port)
        {
            g_stats.port_changed++;
            g_last_port = port;
            g_stats.discovered_port = port;
            LOG_I("PC port changed: -> %d", port);
        }
        if (changed)
            g_stats.endpoint_changes++;
    }

    return RT_EOK;
}

/* ---- Packet validation ---- */

/**
 * @return RT_EOK if valid, negative RT-Thread error code otherwise.
 */
/**
 * @brief  Strict discovery packet parser.
 *
 * Valid format: "ARTPI_PC,<version>,<port>\n" (exactly)
 * "ARTPI_PC" = 8 bytes; comma at index 8;
 * minimum after stripping \n: "ARTPI_PC,1,1" = 11 bytes
 *
 * Rules:
 *   - magic    : bytes[0..7] must equal "ARTPI_PC"  (8 bytes)
 *   - byte[8]  : must be ','                              ← index 8, NOT 7
 *   - version  : decimal digits only; strtol endptr must reach end of field
 *   - byte after version field : must be ',' (not null or digit)
 *   - port     : decimal digits only; endptr must reach end of payload
 *   - port range: 1..65535
 *   - no trailing bytes after port number
 *
 * @return RT_EOK on success; negative RT-Thread error code on failure.
 */
static rt_err_t parse_discovery_packet(const char *data, int len,
                                       int *out_port)
{
    /* Strip trailing \n or \r\n */
    while (len > 0 && (data[len - 1] == '\n' || data[len - 1] == '\r'))
        len--;

    if (len < 11)  /* "ARTPI_PC,1,1" = 11 bytes after stripping \n */
        return -RT_ERROR;

    /* ---- magic: bytes[0..7] ---- */
    if (rt_memcmp(data, DISCOVERY_MAGIC, 8) != 0)
        return -RT_ERROR;

    /* ---- byte[8] must be ',' — blocks "ARTPI_PCXYZ" ---- */
    if (data[8] != ',')
        return -RT_ERROR;

    /* ---- version field: starts at index 9 ---- */
    const char *ver_start = &data[9];
    const char *ver_end   = ver_start;
    while ((size_t)(ver_end - data) < (size_t)len && *ver_end != ',')
        ver_end++;

    if (ver_end == ver_start)          /* empty version */
        return -RT_ERROR;
    if ((size_t)(ver_end - data) >= (size_t)len || *ver_end != ',')
        return -RT_ERROR;              /* no comma after version */

    {
        char ver_buf[8] = {0};
        int vlen = (int)(ver_end - ver_start);
        if (vlen >= (int)sizeof(ver_buf))
            return -RT_ERROR;
        rt_memcpy(ver_buf, ver_start, vlen);
        char *endptr = RT_NULL;
        long ver = strtol(ver_buf, &endptr, 10);
        if (endptr == ver_buf || *endptr != '\0')  /* non-numeric or trailing garbage */
            return -RT_ERROR;
        if (ver != DISCOVERY_VERSION)
            return -RT_ERROR;
    }

    /* ---- port field: starts after the version comma ---- */
    const char *port_start = ver_end + 1;
    if ((size_t)(port_start - data) >= (size_t)len)
        return -RT_ERROR;

    {
        char port_buf[8] = {0};
        int plen = (int)(len - (port_start - data));
        if (plen <= 0 || plen >= (int)sizeof(port_buf))
            return -RT_ERROR;
        rt_memcpy(port_buf, port_start, plen);
        char *endptr = RT_NULL;
        long port = strtol(port_buf, &endptr, 10);
        if (endptr == port_buf || *endptr != '\0')  /* non-numeric or trailing garbage */
            return -RT_ERROR;
        if (port < 1 || port > 65535)
            return -RT_ERROR;
        *out_port = (int)port;
    }

    return RT_EOK;
}

/* ---- Discovery thread ---- */

static void discovery_thread_entry(void *parameter)
{
    (void)parameter;

    char recv_buf[128];
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);

    LOG_I("discovery thread started, listening on UDP :%d", DISCOVERY_PORT);

    /* Outer loop: manage socket lifecycle across Wi-Fi up/down cycles */
    while (g_discovery_running)
    {
        /* Wait for Wi-Fi to be ready before opening a socket */
        while (g_discovery_running && !rt_wlan_is_ready())
        {
            rt_thread_mdelay(DISCOVERY_THREAD_TICK_MS);
        }
        if (!g_discovery_running)
            break;

        /* Create UDP socket */
        g_discovery_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (g_discovery_sock < 0)
        {
            LOG_E("failed to create discovery socket");
            rt_thread_mdelay(2000);
            continue;
        }

        /* Allow address reuse so we can rebind quickly after restart */
        {
            int reuse = 1;
            setsockopt(g_discovery_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        }

        /* Set recv timeout so the loop can also check g_discovery_running
         * and Wi-Fi state without blocking forever. */
        {
            struct timeval tv;
            tv.tv_sec  = DISCOVERY_THREAD_TICK_MS / 1000;
            tv.tv_usec = (DISCOVERY_THREAD_TICK_MS % 1000) * 1000;
            setsockopt(g_discovery_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        }

        /* Bind to INADDR_ANY so we receive broadcasts on the Wi-Fi interface */
        {
            struct sockaddr_in local;
            rt_memset(&local, 0, sizeof(local));
            local.sin_family      = AF_INET;
            local.sin_port        = htons(DISCOVERY_PORT);
            local.sin_addr.s_addr = INADDR_ANY;
            rt_memset(&(local.sin_zero), 0, sizeof(local.sin_zero));

            if (bind(g_discovery_sock, (struct sockaddr *)&local, sizeof(local)) < 0)
            {
                LOG_E("discovery socket bind failed");
                closesocket(g_discovery_sock);
                g_discovery_sock = -1;
                rt_thread_mdelay(2000);
                continue;
            }
        }

        LOG_I("discovery socket ready on :%d", DISCOVERY_PORT);

        /* Inner loop: recvfrom with timeout */
        while (g_discovery_running && rt_wlan_is_ready())
        {
            rt_memset(&from, 0, sizeof(from));
            from_len = sizeof(from);

            int r = recvfrom(g_discovery_sock, recv_buf, sizeof(recv_buf) - 1,
                             0, (struct sockaddr *)&from, &from_len);

            if (!g_discovery_running || !rt_wlan_is_ready())
                break;

            if (r < 0)
            {
                /* Timeout or non-fatal error — poll again */
                continue;
            }

            g_stats.pkts_received++;

            /* Extract sender IP */
            char sender_ip[16] = {0};
            inet_ntop(AF_INET, &from.sin_addr, sender_ip, sizeof(sender_ip));

            /* Parse payload */
            int port = 0;
            rt_err_t pr = parse_discovery_packet(recv_buf, r, &port);

            if (pr == RT_EOK)
            {
                g_stats.pkts_valid++;
                LOG_D("valid broadcast from %s, TCP port %d", sender_ip, port);
                on_pc_endpoint_found(sender_ip, port);
            }
            else
            {
                g_stats.pkts_invalid++;
                LOG_W("invalid broadcast from %s: drop", sender_ip);
            }
        }

        /* Wi-Fi went down or stop requested — close socket and reconnect */
        if (g_discovery_sock >= 0)
        {
            closesocket(g_discovery_sock);
            g_discovery_sock = -1;
            LOG_W("Wi-Fi down, closing discovery socket");
        }
    }

    LOG_I("discovery thread exiting");
}

/* ---- Public API ---- */

/** Increments each time start() is called so callers can detect
 *  "stats from a previous run" by comparing discovery_epoch. */
static rt_uint32_t g_pc_discovery_epoch = 0;

rt_err_t pc_discovery_start(void)
{
    if (g_discovery_running)
        return -RT_EFULL;

    /* Clear the event's pending bits so a fresh stop/start cycle starts clean.
     * rt_event_control(RT_IPC_CMD_RESET) wakes all waiting threads with
     * RT_ERROR so they return quickly before we proceed. */
    reset_event();

    /* Reset state so that a fresh stop/start cycle does not treat the old
     * discovered IP as valid (wait_server must wait for a NEW broadcast). */
    g_last_ip[0] = '\0';
    g_last_port  = 0;
    g_discovery_stopping = RT_FALSE;

    /* Increment the epoch so callers can detect "stats from a previous run". */
    g_pc_discovery_epoch++;
    g_discovery_running = RT_TRUE;
    rt_strncpy(g_stats.device_name, "left", sizeof(g_stats.device_name) - 1);
    g_stats.device_name[sizeof(g_stats.device_name) - 1] = '\0';
    g_stats.discovery_epoch = g_pc_discovery_epoch;

    g_discovery_thread = rt_thread_create("pc_disc",
                                          discovery_thread_entry,
                                          RT_NULL,
                                          DISCOVERY_THREAD_STACK_SIZE,
                                          DISCOVERY_THREAD_PRIORITY,
                                          DISCOVERY_THREAD_TIMESLICE);
    if (g_discovery_thread == RT_NULL)
    {
        g_discovery_running = RT_FALSE;
        return -RT_ERROR;
    }
    if (rt_thread_startup(g_discovery_thread) != RT_EOK)
    {
        /* Startup failed — the thread object is still registered; delete it. */
        rt_thread_delete(g_discovery_thread);
        g_discovery_thread = RT_NULL;
        g_discovery_running = RT_FALSE;
        return -RT_ERROR;
    }
    return RT_EOK;
}

rt_err_t pc_discovery_stop(void)
{
    if (!g_discovery_running)
        return RT_EOK;

    /* Set the flag FIRST.  The discovery thread will see this before it
     * can call rt_event_send(), preventing send on a RESET event. */
    g_discovery_stopping = RT_TRUE;
    g_discovery_running  = RT_FALSE;

    /* Wake any thread waiting on the event (forces wait_server to exit
     * with an error — wait_server checks if FOUND bit was actually set). */
    rt_event_send(&g_discovery_event, DISCOVERY_EVENT_STOPPED);

    /* Interrupt the recvfrom timeout by waking the socket if it is blocked */
    if (g_discovery_sock >= 0)
    {
        closesocket(g_discovery_sock);
        g_discovery_sock = -1;
    }

    if (g_discovery_thread != RT_NULL)
    {
        rt_thread_delete(g_discovery_thread);
        g_discovery_thread = RT_NULL;
    }

    /* Event stays initialised for the process lifetime — no detach. */
    return RT_EOK;
}

rt_bool_t pc_discovery_has_server(void)
{
    return (g_last_ip[0] != '\0') ? RT_TRUE : RT_FALSE;
}

rt_err_t pc_discovery_wait_server(rt_int32_t timeout_ms)
{
    rt_uint32_t recved;

    /* Already have a server — g_last_ip was set only after server_config
     * was successfully updated, so the endpoint is ready. */
    if (g_last_ip[0] != '\0')
        return RT_EOK;

    /* No server AND discovery is not running — cannot wait for one. */
    if (!g_discovery_running)
        return -RT_ERROR;

    /* Use OR mode so we return on either FOUND (success) or STOPPED (stop
     * was called while waiting).  This guarantees g_last_ip and server_config
     * are consistent on return. */
    rt_tick_t ticks = (timeout_ms < 0) ? RT_WAITING_FOREVER
                                        : (rt_tick_t)timeout_ms * RT_TICK_PER_SECOND / 1000;
    if (ticks == 0 && timeout_ms > 0)
        ticks = 1;

    rt_err_t ret = rt_event_recv(&g_discovery_event,
                                  DISCOVERY_EVENT_FOUND | DISCOVERY_EVENT_STOPPED,
                                  RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                                  ticks,
                                  &recved);

    /* FOUND bit means the discovery thread committed the endpoint to
     * server_config before sending the event.  Return success. */
    if (ret == RT_EOK && (recved & DISCOVERY_EVENT_FOUND))
        return RT_EOK;

    /* STOPPED: stop() was called while waiting — return error (not timeout). */
    if (ret == RT_EOK && (recved & DISCOVERY_EVENT_STOPPED))
        return -RT_ERROR;

    /* Timeout: no server was found within the deadline. */
    if (ret == -RT_ETIMEOUT)
        return -RT_ETIMEOUT;

    /* Any other error: return generic error. */
    return -RT_ERROR;
}

void pc_discovery_get_stats(pc_discovery_stats_t *out)
{
    if (out == RT_NULL)
        return;
    rt_base_t level = rt_hw_interrupt_disable();
    *out = g_stats;
    rt_hw_interrupt_enable(level);
}

/* ---- MSH command ---- */

static void cmd_pc_disc_stat(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    pc_discovery_stats_t s;
    pc_discovery_get_stats(&s);

    rt_kprintf("\n=== PC Discovery (left) ===\n");
    rt_kprintf(" running:     %s\n", g_discovery_running ? "yes" : "no");
    rt_kprintf(" device:      %s\n", s.device_name);
    rt_kprintf(" has_server:  %s\n", pc_discovery_has_server() ? "yes" : "no");
    rt_kprintf(" pkts_rcvd:   %u\n", s.pkts_received);
    rt_kprintf(" pkts_valid:  %u\n", s.pkts_valid);
    rt_kprintf(" pkts_invalid:%u\n", s.pkts_invalid);
    rt_kprintf(" ip_changed:  %u\n", s.ip_changed);
    rt_kprintf(" port_changed:%u\n", s.port_changed);
    rt_kprintf(" ep_changes:  %u\n", s.endpoint_changes);
    rt_kprintf(" pc_ip:       %s\n", s.discovered_ip[0] ? s.discovered_ip : "(none)");
    rt_kprintf(" pc_port:     %d\n", s.discovered_port);
    rt_kprintf("===========================\n");
}
MSH_CMD_EXPORT(cmd_pc_disc_stat, Show PC discovery statistics);
