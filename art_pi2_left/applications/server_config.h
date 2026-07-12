/**
 * @file server_config.h
 * @brief 服务器IP配置管理 - 使用环境变量持久化存储
 * @note  通过MSH命令动态设置IP，无需重新烧录
 */

#ifndef __SERVER_CONFIG_H__
#define __SERVER_CONFIG_H__

#include <rtthread.h>

/* 环境变量名称 */
#define ENV_SERVER_IP       "server_ip"
#define ENV_SERVER_PORT     "server_port"
#define ENV_STT_SERVER_IP   "stt_server_ip"
#define ENV_STT_SERVER_PORT "stt_server_port"

/* 默认配置 (当环境变量未设置时使用) */
#define DEFAULT_SERVER_IP       "192.168.157.217"
#define DEFAULT_SERVER_PORT     9109
#define DEFAULT_STT_SERVER_PORT 8080

/**
 * @brief  获取TCP服务器IP地址
 * @param  buf   输出缓冲区
 * @param  size  缓冲区大小
 * @return 实际使用的IP字符串指针
 */
const char* server_config_get_tcp_ip(char *buf, int size);

/**
 * @brief  获取TCP服务器端口
 * @return 端口号
 */
int server_config_get_tcp_port(void);

/**
 * @brief  获取STT服务器IP地址
 * @param  buf   输出缓冲区
 * @param  size  缓冲区大小
 * @return 实际使用的IP字符串指针
 */
const char* server_config_get_stt_ip(char *buf, int size);

/**
 * @brief  获取STT服务器端口
 * @return 端口号
 */
int server_config_get_stt_port(void);

/**
 * @brief  设置TCP服务器IP地址
 * @param  ip  IP地址字符串
 * @return 0成功, -1失败
 */
int server_config_set_tcp_ip(const char *ip);

/**
 * @brief  设置TCP服务器端口
 * @param  port  端口号
 * @return 0成功, -1失败
 */
int server_config_set_tcp_port(int port);

/**
 * @brief  设置STT服务器IP地址
 * @param  ip  IP地址字符串
 * @return 0成功, -1失败
 */
int server_config_set_stt_ip(const char *ip);

/**
 * @brief  设置STT服务器端口
 * @param  port  端口号
 * @return 0成功, -1失败
 */
int server_config_set_stt_port(int port);

#endif /* __SERVER_CONFIG_H__ */
