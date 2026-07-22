/**
 * @file server_config.c
 * @brief 服务器IP配置管理实现（使用全局变量存储）
 */

#include <rtthread.h>
#include <stdlib.h>
#include <string.h>
#include "server_config.h"

#define DBG_TAG "srv_cfg"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* 配置存储（使用全局变量，重启后恢复默认值） */
static char g_tcp_server_ip[16] = DEFAULT_SERVER_IP;
static int  g_tcp_server_port = DEFAULT_SERVER_PORT;
/* STT 没有全局变量存储：host/port 固定复用 ROCK_SERVER_IP /
 * ROCK_STT_SERVER_PORT（rock_config.h），不存在运行时修改路径。 */

/* Thread-safe TCP endpoint update (used by pc_discovery).
 * ip + port + generation are updated atomically inside this mutex.
 * The mutex is initialised once by server_config_init() before any
 * worker threads start — no lazy-init races. */
static struct rt_mutex g_tcp_ep_lock;
static rt_bool_t       g_tcp_ep_lock_inited = RT_FALSE;
static rt_uint32_t     g_tcp_generation = 0;

/* Forward declaration — implementation below; caller must hold g_tcp_ep_lock. */
static int _update_tcp_endpoint_nolock(const char *ip, int port);

/**
 * @brief  Initialise server_config. Must be called once from main()
 *         before any discovery or TCP client threads start.
 */
int server_config_init(void)
{
    if (!g_tcp_ep_lock_inited)
    {
        rt_mutex_init(&g_tcp_ep_lock, "srv_ep", RT_IPC_FLAG_PRIO);
        g_tcp_ep_lock_inited = RT_TRUE;
    }
    return 0;
}

const char* server_config_get_tcp_ip(char *buf, int size)
{
    rt_mutex_take(&g_tcp_ep_lock, RT_WAITING_FOREVER);
    rt_strncpy(buf, g_tcp_server_ip, size - 1);
    buf[size - 1] = '\0';
    rt_mutex_release(&g_tcp_ep_lock);
    return buf;
}

int server_config_get_tcp_port(void)
{
    int port;
    rt_mutex_take(&g_tcp_ep_lock, RT_WAITING_FOREVER);
    port = g_tcp_server_port;
    rt_mutex_release(&g_tcp_ep_lock);
    return port;
}

const char* server_config_get_stt_ip(char *buf, int size)
{
    rt_strncpy(buf, ROCK_SERVER_IP, size - 1);
    buf[size - 1] = '\0';
    return buf;
}

int server_config_get_stt_port(void)
{
    return ROCK_STT_SERVER_PORT;
}

int server_config_set_tcp_ip(const char *ip)
{
    int result;
    if (ip == RT_NULL || ip[0] == '\0')
    {
        LOG_E("Invalid IP address");
        return -1;
    }
    rt_mutex_take(&g_tcp_ep_lock, RT_WAITING_FOREVER);
    result = _update_tcp_endpoint_nolock(ip, g_tcp_server_port);
    rt_mutex_release(&g_tcp_ep_lock);
    if (result >= 0)
        LOG_I("TCP server IP set to: %s", ip);
    return result < 0 ? -1 : 0;
}

int server_config_set_tcp_port(int port)
{
    if (port <= 0 || port > 65535)
    {
        LOG_E("Invalid port number: %d", port);
        return -1;
    }
    rt_mutex_take(&g_tcp_ep_lock, RT_WAITING_FOREVER);
    (void)_update_tcp_endpoint_nolock(g_tcp_server_ip, port);
    rt_mutex_release(&g_tcp_ep_lock);
    LOG_I("TCP server port set to: %d", port);
    return 0;
}

/* ============================================================
 * Internal helper — caller must hold g_tcp_ep_lock.
 * Does NOT acquire the lock itself (allows nested calls).
 * Returns 1 if endpoint changed, 0 if same, negative on error.
 * ============================================================ */
static int _update_tcp_endpoint_nolock(const char *ip, int port)
{
    rt_uint32_t old_gen = g_tcp_generation;

    if (rt_strncmp(g_tcp_server_ip, ip, sizeof(g_tcp_server_ip)) == 0
        && g_tcp_server_port == port)
    {
        return 0;  /* no-op: same endpoint */
    }

    rt_strncpy(g_tcp_server_ip, ip, sizeof(g_tcp_server_ip) - 1);
    g_tcp_server_ip[sizeof(g_tcp_server_ip) - 1] = '\0';
    g_tcp_server_port = port;
    g_tcp_generation++;

    LOG_I("TCP endpoint updated: %s:%d (gen %u -> %u)",
          ip, port, old_gen, g_tcp_generation);
    return 1;  /* positive = changed */
}

/* ============================================================
 * Thread-safe TCP endpoint (for auto-discovery via pc_discovery).
 * ============================================================ */

int server_config_update_tcp_endpoint(const char *ip, int port)
{
    int result;
    /* Validate BEFORE acquiring the lock so we don't hold the mutex
     * for an error path. */
    if (ip == RT_NULL || ip[0] == '\0')
        return -RT_ERROR;
    if (port < 1 || port > 65535)
        return -RT_ERROR;
    rt_mutex_take(&g_tcp_ep_lock, RT_WAITING_FOREVER);
    result = _update_tcp_endpoint_nolock(ip, port);
    rt_mutex_release(&g_tcp_ep_lock);
    return result;
}

rt_uint32_t server_config_get_tcp_generation(void)
{
    rt_uint32_t gen;
    rt_mutex_take(&g_tcp_ep_lock, RT_WAITING_FOREVER);
    gen = g_tcp_generation;
    rt_mutex_release(&g_tcp_ep_lock);
    return gen;
}

void server_config_get_tcp_endpoint(char *ip, int size, int *port,
                                   rt_uint32_t *generation)
{
    rt_mutex_take(&g_tcp_ep_lock, RT_WAITING_FOREVER);
    rt_strncpy(ip, g_tcp_server_ip, size - 1);
    ip[size - 1] = '\0';
    if (port)
        *port = g_tcp_server_port;
    if (generation)
        *generation = g_tcp_generation;
    rt_mutex_release(&g_tcp_ep_lock);
}

/* ============================================================
 * Legacy API
 * ============================================================ */

/* MSH命令: 设置服务器IP */
static int cmd_set_server_ip(int argc, char **argv)
{
    if (argc < 2)
    {
        rt_kprintf("Usage: set_server_ip <ip_address>\n");
        rt_kprintf("Example: set_server_ip 192.168.1.100\n");
        return -1;
    }

    if (server_config_set_tcp_ip(argv[1]) == 0)
    {
        rt_kprintf("TCP server IP set to: %s\n", argv[1]);
        rt_kprintf("Note: Config will reset after reboot\n");
        rt_kprintf("Restart TCP client to apply\n");
        return 0;
    }

    return -1;
}

/* MSH命令: 获取服务器IP */
static int cmd_get_server_ip(int argc, char **argv)
{
    char ip_buf[16];

    server_config_get_tcp_ip(ip_buf, sizeof(ip_buf));
    rt_kprintf("TCP server IP: %s\n", ip_buf);
    rt_kprintf("TCP server port: %d\n", server_config_get_tcp_port());

    return 0;
}

/* MSH命令: 获取STT服务器IP（只读，恒等于 ROCK_SERVER_IP:ROCK_STT_SERVER_PORT） */
static int cmd_get_stt_ip(int argc, char **argv)
{
    char ip_buf[16];

    server_config_get_stt_ip(ip_buf, sizeof(ip_buf));
    rt_kprintf("STT server IP: %s\n", ip_buf);
    rt_kprintf("STT server port: %d\n", server_config_get_stt_port());

    return 0;
}

MSH_CMD_EXPORT_ALIAS(cmd_set_server_ip, set_server_ip, Set TCP server IP);
MSH_CMD_EXPORT_ALIAS(cmd_get_server_ip, get_server_ip, Get TCP server IP);
MSH_CMD_EXPORT_ALIAS(cmd_get_stt_ip, get_stt_ip, Get STT server IP (read-only));
