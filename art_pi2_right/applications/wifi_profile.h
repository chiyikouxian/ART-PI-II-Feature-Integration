/**
 * @file wifi_profile.h
 * @brief WiFi配置文件管理 - 运行时内存管理多个 WiFi 配置
 * @note  当前使用静态数组 profiles[]，不写入 Flash / 文件系统 /
 *        环境变量，**不支持持久化**。开发板重启后所有配置丢失。
 */

#ifndef __WIFI_PROFILE_H__
#define __WIFI_PROFILE_H__

#include <rtthread.h>

/* 最大配置文件数量 */
#define MAX_PROFILES        5

/* 配置名称最大长度 */
#define PROFILE_NAME_LEN    16

/* SSID最大长度 */
#define WIFI_SSID_LEN       32

/* 密码最大长度 */
#define WIFI_PASSWORD_LEN   64

/* IP地址最大长度 */
#define IP_ADDR_LEN         16

/**
 * @brief WiFi配置文件结构
 */
typedef struct {
    char name[PROFILE_NAME_LEN];            /* 配置名称 */
    char ssid[WIFI_SSID_LEN];               /* WiFi SSID */
    char password[WIFI_PASSWORD_LEN];       /* WiFi密码 */
    char server_ip[IP_ADDR_LEN];            /* TCP服务器IP */
    char stt_ip[IP_ADDR_LEN];               /* STT服务器IP */
    rt_bool_t valid;                        /* 是否有效 */
} wifi_profile_t;

/**
 * @brief  添加WiFi配置文件
 * @param  name       配置名称
 * @param  ssid       WiFi SSID
 * @param  password   WiFi密码
 * @param  server_ip  TCP服务器IP
 * @param  stt_ip     STT服务器IP（可选，传NULL则使用server_ip）
 * @return 0成功, -1失败
 */
int wifi_profile_add(const char *name, const char *ssid,
                     const char *password, const char *server_ip,
                     const char *stt_ip);

/**
 * @brief  删除WiFi配置文件
 * @param  name  配置名称
 * @return 0成功, -1失败
 */
int wifi_profile_delete(const char *name);

/**
 * @brief  切换到指定配置（自动连接WiFi并设置服务器IP）
 * @param  name  配置名称
 * @return 0成功, -1失败
 */
int wifi_profile_use(const char *name);

/**
 * @brief  列出所有配置文件
 */
void wifi_profile_list(void);

/**
 * @brief  显示当前使用的配置
 */
void wifi_profile_current(void);

#endif /* __WIFI_PROFILE_H__ */
