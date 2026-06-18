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
static char g_stt_server_ip[16] = DEFAULT_SERVER_IP;
static int  g_stt_server_port = DEFAULT_STT_SERVER_PORT;

const char* server_config_get_tcp_ip(char *buf, int size)
{
    rt_strncpy(buf, g_tcp_server_ip, size - 1);
    buf[size - 1] = '\0';
    return buf;
}

int server_config_get_tcp_port(void)
{
    return g_tcp_server_port;
}

const char* server_config_get_stt_ip(char *buf, int size)
{
    rt_strncpy(buf, g_stt_server_ip, size - 1);
    buf[size - 1] = '\0';
    return buf;
}

int server_config_get_stt_port(void)
{
    return g_stt_server_port;
}

int server_config_set_tcp_ip(const char *ip)
{
    if (ip == RT_NULL || ip[0] == '\0')
    {
        LOG_E("Invalid IP address");
        return -1;
    }

    rt_strncpy(g_tcp_server_ip, ip, sizeof(g_tcp_server_ip) - 1);
    g_tcp_server_ip[sizeof(g_tcp_server_ip) - 1] = '\0';

    LOG_I("TCP server IP set to: %s", g_tcp_server_ip);
    return 0;
}

int server_config_set_tcp_port(int port)
{
    if (port <= 0 || port > 65535)
    {
        LOG_E("Invalid port number: %d", port);
        return -1;
    }

    g_tcp_server_port = port;

    LOG_I("TCP server port set to: %d", port);
    return 0;
}

int server_config_set_stt_ip(const char *ip)
{
    if (ip == RT_NULL || ip[0] == '\0')
    {
        LOG_E("Invalid IP address");
        return -1;
    }

    rt_strncpy(g_stt_server_ip, ip, sizeof(g_stt_server_ip) - 1);
    g_stt_server_ip[sizeof(g_stt_server_ip) - 1] = '\0';

    LOG_I("STT server IP set to: %s", g_stt_server_ip);
    return 0;
}

int server_config_set_stt_port(int port)
{
    if (port <= 0 || port > 65535)
    {
        LOG_E("Invalid port number: %d", port);
        return -1;
    }

    g_stt_server_port = port;

    LOG_I("STT server port set to: %d", port);
    return 0;
}

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

/* MSH命令: 设置STT服务器IP */
static int cmd_set_stt_ip(int argc, char **argv)
{
    if (argc < 2)
    {
        rt_kprintf("Usage: set_stt_ip <ip_address>\n");
        rt_kprintf("Example: set_stt_ip 192.168.1.100\n");
        return -1;
    }

    if (server_config_set_stt_ip(argv[1]) == 0)
    {
        rt_kprintf("STT server IP set to: %s\n", argv[1]);
        return 0;
    }

    return -1;
}

/* MSH命令: 获取STT服务器IP */
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
MSH_CMD_EXPORT_ALIAS(cmd_set_stt_ip, set_stt_ip, Set STT server IP);
MSH_CMD_EXPORT_ALIAS(cmd_get_stt_ip, get_stt_ip, Get STT server IP);
