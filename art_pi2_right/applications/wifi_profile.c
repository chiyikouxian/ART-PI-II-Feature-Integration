/**
 * @file wifi_profile.c
 * @brief WiFi配置文件管理实现 (运行时内存，重启后丢失)
 */

#include <rtthread.h>
#include <string.h>
#include <stdio.h>
#include "wifi_profile.h"
#include "server_config.h"

#define DBG_TAG "wifi_prof"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* 配置文件存储（运行时内存，重启后丢失） */
static wifi_profile_t profiles[MAX_PROFILES];

/* 当前使用的配置索引 (-1表示无) */
static int current_profile_idx = -1;

/* 外部函数声明 */
extern int tcp_client_stop(void);
extern int tcp_client_start(int argc, char **argv);

/**
 * @brief  查找配置文件索引
 */
static int find_profile(const char *name)
{
    for (int i = 0; i < MAX_PROFILES; i++)
    {
        if (profiles[i].valid &&
            rt_strcmp(profiles[i].name, name) == 0)
        {
            return i;
        }
    }
    return -1;
}

/**
 * @brief  查找空闲配置槽
 */
static int find_free_slot(void)
{
    for (int i = 0; i < MAX_PROFILES; i++)
    {
        if (!profiles[i].valid)
        {
            return i;
        }
    }
    return -1;
}

int wifi_profile_add(const char *name, const char *ssid,
                     const char *password, const char *server_ip)
{
    int idx;

    if (name == RT_NULL || ssid == RT_NULL ||
        password == RT_NULL || server_ip == RT_NULL)
    {
        LOG_E("Invalid parameters");
        return -1;
    }

    /* 检查是否已存在 */
    idx = find_profile(name);
    if (idx >= 0)
    {
        LOG_W("Profile '%s' already exists, updating", name);
    }
    else
    {
        /* 查找空闲槽 */
        idx = find_free_slot();
        if (idx < 0)
        {
            LOG_E("No free slot, max %d profiles", MAX_PROFILES);
            return -1;
        }
    }

    /* 保存配置 */
    rt_strncpy(profiles[idx].name, name, PROFILE_NAME_LEN - 1);
    profiles[idx].name[PROFILE_NAME_LEN - 1] = '\0';

    rt_strncpy(profiles[idx].ssid, ssid, WIFI_SSID_LEN - 1);
    profiles[idx].ssid[WIFI_SSID_LEN - 1] = '\0';

    rt_strncpy(profiles[idx].password, password, WIFI_PASSWORD_LEN - 1);
    profiles[idx].password[WIFI_PASSWORD_LEN - 1] = '\0';

    rt_strncpy(profiles[idx].server_ip, server_ip, IP_ADDR_LEN - 1);
    profiles[idx].server_ip[IP_ADDR_LEN - 1] = '\0';

    profiles[idx].valid = RT_TRUE;

    LOG_I("Profile '%s' saved (in-memory only)", name);
    return 0;
}

int wifi_profile_delete(const char *name)
{
    int idx = find_profile(name);

    if (idx < 0)
    {
        LOG_E("Profile '%s' not found", name);
        return -1;
    }

    profiles[idx].valid = RT_FALSE;

    if (current_profile_idx == idx)
    {
        current_profile_idx = -1;
    }

    LOG_I("Profile '%s' deleted", name);
    return 0;
}

int wifi_profile_use(const char *name)
{
    int idx = find_profile(name);

    if (idx < 0)
    {
        LOG_E("Profile '%s' not found", name);
        return -1;
    }

    wifi_profile_t *p = &profiles[idx];

    rt_kprintf("\n=== Switching to profile: %s ===\n", p->name);
    rt_kprintf("WiFi: %s\n", p->ssid);
    rt_kprintf("Server IP: %s\n\n", p->server_ip);

    /* WiFi连接所有权归 net_manager，本函数不调用 rt_wlan_connect。
     * 如尚未连接到该网络，需手动执行下面的命令。 */
    rt_kprintf("[1/2] WiFi connection is owned by net_manager.\n");
    rt_kprintf("      If not already on this network, run manually:\n");
    rt_kprintf("      wifi join %s %s\n", p->ssid, p->password);

    /* 设置PC TCP端点（PC辅助端点，与 ROCK/STT 无关）并重启TCP客户端 */
    rt_kprintf("[2/2] Setting PC TCP endpoint and restarting TCP client...\n");
    server_config_set_tcp_ip(p->server_ip);
    tcp_client_stop();
    rt_thread_mdelay(500);
    tcp_client_start(0, RT_NULL);

    current_profile_idx = idx;

    rt_kprintf("\n=== Profile switched successfully ===\n\n");
    return 0;
}

void wifi_profile_list(void)
{
    int count = 0;

    rt_kprintf("\n=== WiFi Profiles ===\n");
    rt_kprintf("%-4s %-12s %-20s %-16s\n",
               "No.", "Name", "SSID", "Server IP");
    rt_kprintf("----------------------------------------------------\n");

    for (int i = 0; i < MAX_PROFILES; i++)
    {
        if (profiles[i].valid)
        {
            rt_kprintf("%-4d %-12s %-20s %-16s %s\n",
                       i + 1,
                       profiles[i].name,
                       profiles[i].ssid,
                       profiles[i].server_ip,
                       (current_profile_idx == i) ? "[CURRENT]" : "");
            count++;
        }
    }

    if (count == 0)
    {
        rt_kprintf("No profiles saved\n");
    }

    rt_kprintf("\nTotal: %d/%d (in-memory only, lost on reboot)\n\n", count, MAX_PROFILES);
}

void wifi_profile_current(void)
{
    if (current_profile_idx < 0)
    {
        rt_kprintf("No profile is currently active\n");
        return;
    }

    wifi_profile_t *p = &profiles[current_profile_idx];

    rt_kprintf("\n=== Current Profile ===\n");
    rt_kprintf("Name:      %s\n", p->name);
    rt_kprintf("SSID:      %s\n", p->ssid);
    rt_kprintf("Password:  %s\n", p->password);
    rt_kprintf("Server IP: %s\n\n", p->server_ip);
}

/* ==================== MSH命令 ==================== */

/* profile add <name> <ssid> <password> <server_ip> */
static int cmd_profile_add(int argc, char **argv)
{
    if (argc < 5)
    {
        rt_kprintf("Usage: profile add <name> <ssid> <password> <server_ip>\n");
        rt_kprintf("Example: profile add home MyWiFi pass123 192.168.1.100\n");
        return -1;
    }

    return wifi_profile_add(argv[2], argv[3], argv[4], argv[5]);
}

/* profile del <name> */
static int cmd_profile_del(int argc, char **argv)
{
    if (argc < 3)
    {
        rt_kprintf("Usage: profile del <name>\n");
        rt_kprintf("Example: profile del home\n");
        return -1;
    }

    return wifi_profile_delete(argv[2]);
}

/* profile use <name> */
static int cmd_profile_use(int argc, char **argv)
{
    if (argc < 3)
    {
        rt_kprintf("Usage: profile use <name>\n");
        rt_kprintf("Example: profile use home\n");
        return -1;
    }

    return wifi_profile_use(argv[2]);
}

/* profile list */
static int cmd_profile_list(int argc, char **argv)
{
    wifi_profile_list();
    return 0;
}

/* profile current */
static int cmd_profile_current(int argc, char **argv)
{
    wifi_profile_current();
    return 0;
}

/* 主命令分发 */
static int cmd_profile(int argc, char **argv)
{
    if (argc < 2)
    {
        rt_kprintf("WiFi Profile Manager (in-memory only, lost on reboot)\n");
        rt_kprintf("Usage:\n");
        rt_kprintf("  profile add <name> <ssid> <pwd> <ip>\n");
        rt_kprintf("  profile del <name>\n");
        rt_kprintf("  profile use <name>\n");
        rt_kprintf("  profile list\n");
        rt_kprintf("  profile current\n");
        return 0;
    }

    if (rt_strcmp(argv[1], "add") == 0)
    {
        return cmd_profile_add(argc, argv);
    }
    else if (rt_strcmp(argv[1], "del") == 0)
    {
        return cmd_profile_del(argc, argv);
    }
    else if (rt_strcmp(argv[1], "use") == 0)
    {
        return cmd_profile_use(argc, argv);
    }
    else if (rt_strcmp(argv[1], "list") == 0)
    {
        return cmd_profile_list(argc, argv);
    }
    else if (rt_strcmp(argv[1], "current") == 0)
    {
        return cmd_profile_current(argc, argv);
    }
    else
    {
        rt_kprintf("Unknown command: %s\n", argv[1]);
        rt_kprintf("Run 'profile' for help\n");
        return -1;
    }
}

MSH_CMD_EXPORT(profile, WiFi profile manager);


