/**
 * @file server_config.h
 * @brief 服务器IP配置管理 - 运行时内存配置
 * @note  当前使用 RAM 全局变量存储，**不支持持久化**。
 *        开发板重启后，TCP 辅助端点恢复为编译期默认值。
 *        通过 MSH 命令可动态调整 TCP 辅助端点: set_server_ip / get_server_ip
 *        STT 不是独立可配置端点：server_config_get_stt_ip/port() 是只读镜像，
 *        始终返回 ROCK_SERVER_IP:ROCK_STT_SERVER_PORT（见 rock_config.h），
 *        没有任何运行时路径可以修改 STT host/port。
 */

#ifndef __SERVER_CONFIG_H__
#define __SERVER_CONFIG_H__

#include <rtthread.h>
#include "rock_config.h"

/* 默认配置 (编译期默认值，重启后恢复这些值) */
#define DEFAULT_SERVER_IP       "192.168.129.50"
#define DEFAULT_SERVER_PORT     9109

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
 * @brief  获取STT服务器IP地址（只读镜像，恒等于 ROCK_SERVER_IP）
 * @param  buf   输出缓冲区
 * @param  size  缓冲区大小
 * @return 实际使用的IP字符串指针
 */
const char* server_config_get_stt_ip(char *buf, int size);

/**
 * @brief  获取STT服务器端口（只读镜像，恒等于 ROCK_STT_SERVER_PORT）
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

/* 注意：STT host/port 没有 setter。STT 固定复用 ROCK_SERVER_IP /
 * ROCK_STT_SERVER_PORT，不允许运行时独立覆盖。 */

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
