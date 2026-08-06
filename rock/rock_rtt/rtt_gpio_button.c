#include <rtthread.h>
#include <rtdevice.h>
#include <sal_socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <dt-bindings-pinctrl.h>

#define ROCK_IP "10.10.10.31"
#define ROCK_PORT 9103

#define BTN_PIN RK_GPIO3_A4
#define BTN_NAME "GPIO3_A4"

#define POLL_MS 10
#define DEBOUNCE_MS 30
#define REPORT_EVERY_MS 500

static int udp_send_json(const char *msg)
{
    int fd;
    struct sockaddr_in addr;

    fd = sal_socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        rt_kprintf("[btn] sal_socket failed\n");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ROCK_PORT);
    addr.sin_addr.s_addr = inet_addr(ROCK_IP);

    if (sal_sendto(fd, msg, rt_strlen(msg), 0,
                   (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        rt_kprintf("[btn] sal_sendto failed\n");
        sal_closesocket(fd);
        return -1;
    }

    sal_closesocket(fd);
    return 0;
}

static void button_probe_thread(void *parameter)
{
    int last_level;
    int stable_level;
    int seq = 0;
    int elapsed_ms = 0;
    char msg[192];

    rt_kprintf("[btn] wait network ready\n");
    rt_thread_mdelay(20000);

    rt_pin_mode(BTN_PIN, PIN_MODE_INPUT_PULLDOWN);

    stable_level = rt_pin_read(BTN_PIN);
    last_level = stable_level;

    rt_kprintf("[btn] probe start: pin=%s init_level=%d target=%s:%d\n",
               BTN_NAME, stable_level, ROCK_IP, ROCK_PORT);

    while (1)
    {
        int now = rt_pin_read(BTN_PIN);
        elapsed_ms += POLL_MS;

        if (elapsed_ms >= REPORT_EVERY_MS)
        {
            elapsed_ms = 0;
            rt_snprintf(msg, sizeof(msg),
                        "{\"cmd\":\"gpio_state\",\"source\":\"rtt_gpio_probe\",\"pin\":\"%s\",\"level\":%d,\"seq\":%d}",
                        BTN_NAME, now, seq);
            udp_send_json(msg);
        }

        if (now != last_level)
        {
            rt_thread_mdelay(DEBOUNCE_MS);
            now = rt_pin_read(BTN_PIN);

            if (now != stable_level)
            {
                stable_level = now;
                seq++;

                rt_snprintf(msg, sizeof(msg),
                            "{\"cmd\":\"gpio_state\",\"source\":\"rtt_gpio_probe\",\"pin\":\"%s\",\"level\":%d,\"seq\":%d}",
                            BTN_NAME, stable_level, seq);
                udp_send_json(msg);

                if (1)
                {
                    rt_snprintf(msg, sizeof(msg),
                                "{\"cmd\":\"toggle\",\"source\":\"rtt_gpio\",\"pin\":\"%s\",\"level\":%d,\"seq\":%d}",
                                BTN_NAME, stable_level, seq);
                    udp_send_json(msg);
                }
            }
        }

        last_level = now;
        rt_thread_mdelay(POLL_MS);
    }
}

int rtt_gpio_button_init(void)
{
    rt_thread_t tid;

    tid = rt_thread_create("btn_probe",
                           button_probe_thread,
                           RT_NULL,
                           2048,
                           20,
                           10);
    if (tid == RT_NULL)
    {
        rt_kprintf("[btn] thread create failed\n");
        return -RT_ERROR;
    }

    rt_thread_startup(tid);
    return RT_EOK;
}
