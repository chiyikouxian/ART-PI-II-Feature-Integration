/**
 * @file net_stat.h
 * @brief Statistics export for the net_stat MSH command.
 *
 * Exposes counters from:
 *   net_manager     -- WiFi connect / DHCP / link-loss
 *   net_keepalive   -- PING / PONG / timeout events
 *   net_scheduler   -- congestion ticks / frame skips
 *   model_queue     -- push / pop / drop / short-write / retry counts
 *   tcp_client      -- frames sent / short writes / rx parse errors
 *
 * Usage from the RT-Thread shell (MSH):
 *   >>> net_stat
 */
#ifndef __NET_STAT_H
#define __NET_STAT_H

#include <rtthread.h>

/**
 * @brief  Gather and print all network-layer counters to the shell.
 *         Registered as an MSH command via MSH_CMD_EXPORT in net_stat.c.
 */
void cmd_net_stat(int argc, char **argv);

#endif /* __NET_STAT_H */
