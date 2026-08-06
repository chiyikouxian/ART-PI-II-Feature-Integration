#include <rtthread.h>
#include <sal_socket.h>
#include <arpa/inet.h>
#include <string.h>
#include "rtt_control.h"

#define RTT_LISTEN_PORT 9100

static void rtt_linux_link_thread(void *parameter)
{
    int sock;
    struct sockaddr_in local;
    char rx[256];
    char tx[256];

    rt_kprintf("[rtt_link] udp server wait network ready\n");
    rt_thread_mdelay(20000);

    while (1)
    {
        sock = sal_socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0)
        {
            rt_kprintf("[rtt_link] udp socket failed\n");
            rt_thread_mdelay(1000);
            continue;
        }

        memset(&local, 0, sizeof(local));
        local.sin_family = AF_INET;
        local.sin_port = htons(RTT_LISTEN_PORT);
        local.sin_addr.s_addr = INADDR_ANY;

        if (sal_bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0)
        {
            rt_kprintf("[rtt_link] udp bind %d failed\n", RTT_LISTEN_PORT);
            sal_closesocket(sock);
            rt_thread_mdelay(1000);
            continue;
        }

        rt_kprintf("[rtt_link] udp server listening on %d\n", RTT_LISTEN_PORT);

        while (1)
        {
            struct sockaddr_in peer;
            socklen_t peer_len = sizeof(peer);
            int n = sal_recvfrom(sock, rx, sizeof(rx) - 1, 0,
                                 (struct sockaddr *)&peer, &peer_len);
            if (n <= 0)
            {
                rt_kprintf("[rtt_link] udp recv failed\n");
                break;
            }

            rx[n] = '\0';
            rt_kprintf("[rtt_link] recv: %s\n", rx);

            rtt_control_handle_command(rx, tx, sizeof(tx));
	    sal_sendto(sock, tx, strlen(tx), 0,
                       (struct sockaddr *)&peer, peer_len);

        }

        sal_closesocket(sock);
        rt_thread_mdelay(1000);
    }
}

int rtt_linux_link_init(void)
{
    rt_thread_t tid = rt_thread_create("rtt_link",
                                      rtt_linux_link_thread,
                                      RT_NULL,
                                      4096,
                                      20,
                                      20);
    if (tid)
    {
        rt_thread_startup(tid);
    }
    else
    {
        rt_kprintf("[rtt_link] create thread failed\n");
    }

    return 0;
}
