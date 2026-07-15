/**
 * @file server_config.h
 * @brief 服务器IP配置管理 - 运行时内存配置
 * @note  当前使用 RAM 全局变量存储，**不支持持久化**。
 *        开发板重启后，所有 IP/端口恢复为编译期默认值。
 *        通过 MSH 命令可动态调整当前运行配置:
 *          set_server_ip / get_server_ip / set_stt_ip / get_stt_ip
 */

#ifndef __SERVER_CONFIG_H__
#define __SERVER_CONFIG_H__

#include <rtthread.h>

/* 默认配置 (编译期默认值，重启后恢复这些值) */
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

/* ============================================================
 * Thread-safe TCP endpoint — used by pc_discovery to atomically
 * update IP + port + generation.
 * ============================================================ */

int server_config_update_tcp_endpoint(const char *ip, int port);
int server_config_init(void);
rt_uint32_t server_config_get_tcp_generation(void);
void server_config_get_tcp_endpoint(char *ip, int size, int *port,
                                  rt_uint32_t *generation);

#endif /* __SERVER_CONFIG_H__ */
