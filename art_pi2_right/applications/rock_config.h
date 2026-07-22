/**
 * @file rock_config.h
 * @brief ROCK 内部网络编译期常量（Plan B 固定内部网络）- 右手
 *
 * 单一真值来源：ROCK 自建热点（热点接口 wlan1，热点侧固定 IP
 * 192.168.1.1/24；已通过真机验证的热点 SSID/密码见各自 main.c /
 * net_manager.c，本文件只放 ROCK 服务地址，不重复堆放 WiFi 凭据）。
 * 右手对 ROCK 的所有链路都从此处取地址：
 *   - IMU 原始 CSV 上行：ROCK_SERVER_IP:9102（端口在 imu_wifi_sender.h）
 *   - STT 代理：http://ROCK_SERVER_IP:ROCK_STT_SERVER_PORT/stt
 *
 * 该地址为固定值，不随 DHCP 默认网关或 ROCK 外部 WiFi 接口地址变化。
 * 本头文件只放网络常量，不依赖任何其它应用模块，供 imu_wifi_sender.h
 * 与 server_config.c 共同引用，避免模块间反向依赖。
 */

#ifndef __ROCK_CONFIG_H__
#define __ROCK_CONFIG_H__

/* ROCK 热点侧固定服务地址（左右手一致；右手端口见 imu_wifi_sender.h） */
#define ROCK_SERVER_IP          "192.168.1.1"

/* ROCK 上部署的 STT 代理端口，右手 STT 与 ROCK_SERVER_IP 共用主机 */
#define ROCK_STT_SERVER_PORT    8080

#endif /* __ROCK_CONFIG_H__ */
