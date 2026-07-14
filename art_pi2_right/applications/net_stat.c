/**
 * @file net_stat.c
 * @brief MSH command: net_stat -- dump all network-layer counters.
 *
 * Aggregates counters from:
 *   net_manager     -- WiFi connect / DHCP / link-loss
 *   net_keepalive   -- PING / PONG / timeout events
 *   net_scheduler   -- congestion ticks / frame skips
 *   model_queue     -- push / pop / drop / short-write counts
 *   tcp_client      -- frames sent / short writes / rx parse errors
 *
 * Usage from the RT-Thread shell (MSH):
 *   >>> net_stat
 */
#include <rtthread.h>
#include <stdio.h>

#include "net_stat.h"
#include "net_manager.h"
#include "net_keepalive.h"
#include "net_scheduler.h"
#include "model_queue.h"
#include "tcp_client.h"
#include "net_session.h"

void cmd_net_stat(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    net_manager_stats_t   nm;
    net_keepalive_stats_t ka;
    net_scheduler_stats_t  sched;
    model_queue_stats_t   mq;
    tcp_link_stats_t      tcp;

    net_manager_get_stats(&nm);
    net_keepalive_get_stats(-1, &ka);
    net_scheduler_get_stats(&sched);
    model_queue_get_stats(&mq);
    tcp_get_link_stats(&tcp);

    rt_kprintf("\n");
    rt_kprintf("============================================\n");
    rt_kprintf(" ART-Pi2 Network Link Statistics\n");
    rt_kprintf("============================================\n");

    rt_kprintf(" Session (HELLO metadata)\n");
    {
        const net_session_info_t *si = net_session_current();
        rt_tick_t now_tick = rt_tick_get();

        /* started_tick is set once by net_session_init() and is the
         * authoritative "how long has net_session been alive" baseline.
         * Convert ticks -> ms for the operator. */
        rt_uint32_t uptime_ms = 0;
        if (si->started_tick != 0)
            uptime_ms = (rt_uint32_t)((now_tick - si->started_tick)
                                      * 1000u / RT_TICK_PER_SECOND);

        rt_uint32_t since_ms = (si->last_announce_tick == 0)
                               ? 0xFFFFFFFFu
                               : (rt_uint32_t)((now_tick - si->last_announce_tick)
                                                * 1000u / RT_TICK_PER_SECOND);

        rt_kprintf("   side:              %c\n", si->side);
        rt_kprintf("   boot_id:           %08x\n", si->boot_id);
        rt_kprintf("   session_id:        %08x\n", si->session_id);
        rt_kprintf("   uptime_ms:         %u\n", uptime_ms);
        rt_kprintf("   announce_count:    %u\n", si->announce_count);
        rt_kprintf("   announce_fail:     %u\n", si->announce_fail_count);
        if (since_ms == 0xFFFFFFFFu)
            rt_kprintf("   last_hello_age_ms: never\n");
        else
            rt_kprintf("   last_hello_age_ms: %u\n", since_ms);
    }

    rt_kprintf("\n");
    rt_kprintf(" WiFi / net_manager\n");
    rt_kprintf("   State:               %s\n",
               net_manager_state_name(net_manager_get_state()));
    rt_kprintf("   connect_attempts:    %u\n", nm.connect_attempts);
    rt_kprintf("   connect_ok:         %u\n", nm.connect_ok);
    rt_kprintf("   dhcp_acquired:      %u\n", nm.dhcp_acquired);
    rt_kprintf("   link_lost_events:   %u\n", nm.link_lost_events);
    rt_kprintf("   manual_disconn_hints:%u\n", nm.manual_disconnect_hints);
    rt_kprintf("   time_in_ready_ms:  %u\n", nm.time_in_ready_ms);

    rt_kprintf("\n");
    rt_kprintf(" Keepalive (PC TCP path)\n");
    rt_kprintf("   ping_sent:          %u\n", ka.ping_sent);
    rt_kprintf("   pong_sent:          %u\n", ka.pong_sent);
    rt_kprintf("   pong_recv:          %u\n", ka.pong_recv);
    rt_kprintf("   timeout_events:     %u\n", ka.timeout_events);
    if (ka.last_rx_age_ms != 0xFFFFFFFFu)
        rt_kprintf("   last_rx_age_ms:   %u\n", ka.last_rx_age_ms);

    rt_kprintf("\n");
    rt_kprintf(" Scheduler (shared WiFi link)\n");
    rt_kprintf("   congested_ticks:    %u\n", sched.congested_ticks);
    rt_kprintf("   skips_requested:    %u\n", sched.skips_requested);

    rt_kprintf("\n");
    rt_kprintf(" Model Queue (ROCK path)\n");
    rt_kprintf("   pushed:             %u\n", mq.pushed);
    rt_kprintf("   popped:            %u\n", mq.popped);
    rt_kprintf("   dropped:           %u\n", mq.dropped);
    rt_kprintf("   short_writes:      %u\n", mq.short_writes);
    rt_kprintf("   retries:           %u\n", mq.retries);

    rt_kprintf("\n");
    rt_kprintf(" PC TCP Link (tcp_client)\n");
    rt_kprintf("   frames_sent:       %u\n", tcp.frames_sent);
    rt_kprintf("   frames_attempted:  %u\n", tcp.frames_attempted);
    rt_kprintf("   frames_skipped:    %u\n", tcp.frames_skipped);
    rt_kprintf("   short_writes:     %u\n", tcp.short_writes);
    rt_kprintf("   rx_parse_errors:  %u\n", tcp.rx_parse_errors);

    rt_kprintf("\n============================================\n\n");
}
MSH_CMD_EXPORT(cmd_net_stat, Show network link statistics);
