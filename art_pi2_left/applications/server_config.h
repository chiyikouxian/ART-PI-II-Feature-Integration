/**
 * @file server_config.h
 * @brief 服务器IP/端口配置管理 - 运行时内存配置
 * @note  - 配置使用 RAM 全局变量存储，**不支持持久化**（未实现
 *          getenv/EasyFlash/Flash/文件系统等接口）。开发板重启后，
 *          所有 IP/端口恢复为编译期默认值。
 *        - TCP endpoint 可由 PC UDP discovery（pc_discovery）在
 *          运行时更新；TCP 客户端通过 generation 计数器识别变更。
 *        - 左手没有 STT 数据消费者，本文件不提供 STT 相关接口。
 *        - 通过 MSH 命令可动态调整当前运行配置:
 *          set_server_ip / get_server_ip
 */

#ifndef __SERVER_CONFIG_H__
#define __SERVER_CONFIG_H__

#include <rtthread.h>

/* 默认配置 (编译期默认值，重启后恢复这些值) */
#define DEFAULT_SERVER_IP       "192.168.157.217"
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

/* ============================================================
 * Thread-safe TCP endpoint — used by pc_discovery to atomically
 * update IP + port + generation. tcp_client reads generation to
 * detect remote endpoint changes without any cross-thread races.
 * ============================================================ */

/**
 * @brief  Atomically update the TCP endpoint.
 * @param  ip    New PC IP (null-terminated).
 * @param  port  New TCP port.
 * @return 1 if endpoint actually changed, 0 if same as current,
 *         negative on error.
 * @note   Incrementing the generation signals tcp_client to reconnect.
 */
int server_config_update_tcp_endpoint(const char *ip, int port);

/**
 * @brief  Initialise server_config. Call once from main() before
 *         starting any discovery or TCP client threads.
 */
int server_config_init(void);

/**
 * @brief  Current generation counter. Increments each time
 *         update_tcp_endpoint() is called with a different endpoint.
 * @return Generation value.
 */
rt_uint32_t server_config_get_tcp_generation(void);

/**
 * @brief  Get a consistent snapshot of the current TCP endpoint.
 * @param  ip         Output buffer for IP string.
 * @param  size       Size of @p ip buffer (recommend >= 16).
 * @param  port       [out] Current TCP port.
 * @param  generation [out] Current generation value; pass RT_NULL if not needed.
 */
void server_config_get_tcp_endpoint(char *ip, int size, int *port,
                                  rt_uint32_t *generation);

#endif /* __SERVER_CONFIG_H__ */
