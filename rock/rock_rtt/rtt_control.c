#include "rtt_control.h"
#include <string.h>

typedef enum
{
    CONTROL_STATE_IDLE = 0,
    CONTROL_STATE_RUNNING,
    CONTROL_STATE_ERROR,
} control_state_t;

static control_state_t g_state = CONTROL_STATE_IDLE;
static rt_mutex_t g_lock = RT_NULL;

static rt_uint32_t g_rx_count = 0;
static rt_uint32_t g_start_count = 0;
static rt_uint32_t g_stop_count = 0;
static rt_uint32_t g_error_count = 0;
static rt_uint32_t g_work_count = 0;

static rt_uint64_t uptime_ms(void)
{
    return ((rt_uint64_t)rt_tick_get() * 1000ULL) / RT_TICK_PER_SECOND;
}

static const char *state_name(control_state_t state)
{
    switch (state)
    {
    case CONTROL_STATE_IDLE:
        return "idle";
    case CONTROL_STATE_RUNNING:
        return "running";
    case CONTROL_STATE_ERROR:
        return "error";
    default:
        return "unknown";
    }
}

static void lock_control(void)
{
    if (g_lock)
    {
        rt_mutex_take(g_lock, RT_WAITING_FOREVER);
    }
}

static void unlock_control(void)
{
    if (g_lock)
    {
        rt_mutex_release(g_lock);
    }
}

static void write_status(char *tx, rt_size_t tx_size, rt_bool_t ok)
{
    rt_snprintf(tx, tx_size,
                "{\"ok\":%s,\"state\":\"%s\",\"uptime_ms\":%llu,"
                "\"rx_count\":%u,\"start_count\":%u,\"stop_count\":%u,"
                "\"error_count\":%u,\"work_count\":%u}\n",
                ok ? "true" : "false",
                state_name(g_state),
                uptime_ms(),
                g_rx_count,
                g_start_count,
                g_stop_count,
                g_error_count,
                g_work_count);
}

static void rtt_control_worker(void *parameter)
{
    while (1)
    {
        rt_thread_mdelay(1000);

        lock_control();
        if (g_state == CONTROL_STATE_RUNNING)
        {
            g_work_count++;
        }
        unlock_control();
    }
}

int rtt_control_init(void)
{
    rt_thread_t tid;

    g_lock = rt_mutex_create("ctl_lock", RT_IPC_FLAG_PRIO);
    if (!g_lock)
    {
        rt_kprintf("[rtt_control] create mutex failed\n");
        return -1;
    }

    tid = rt_thread_create("ctl_work",
                           rtt_control_worker,
                           RT_NULL,
                           4096,
                           21,
                           20);
    if (!tid)
    {
        rt_kprintf("[rtt_control] create worker failed\n");
        return -1;
    }

    rt_thread_startup(tid);
    rt_kprintf("[rtt_control] worker started\n");
    return 0;
}

int rtt_control_handle_command(const char *rx, char *tx, rt_size_t tx_size)
{
    if (!rx || !tx || tx_size == 0)
    {
        return -1;
    }

    lock_control();

    g_rx_count++;

    if (strstr(rx, "\"cmd\":\"status\"") || strstr(rx, "status"))
    {
        write_status(tx, tx_size, RT_TRUE);
        unlock_control();
        return 0;
    }

    if (strstr(rx, "\"cmd\":\"ping\"") || strstr(rx, "ping"))
    {
        rt_snprintf(tx, tx_size,
                    "{\"ok\":true,\"reply\":\"pong\",\"state\":\"%s\","
                    "\"uptime_ms\":%llu,\"work_count\":%u}\n",
                    state_name(g_state),
                    uptime_ms(),
                    g_work_count);
        unlock_control();
        return 0;
    }

    if (strstr(rx, "\"cmd\":\"start\"") || strstr(rx, "start"))
    {
        if (g_state != CONTROL_STATE_RUNNING)
        {
            g_state = CONTROL_STATE_RUNNING;
            g_start_count++;
        }

        write_status(tx, tx_size, RT_TRUE);
        unlock_control();
        return 0;
    }

    if (strstr(rx, "\"cmd\":\"stop\"") || strstr(rx, "stop"))
    {
        if (g_state != CONTROL_STATE_IDLE)
        {
            g_state = CONTROL_STATE_IDLE;
            g_stop_count++;
        }

        write_status(tx, tx_size, RT_TRUE);
        unlock_control();
        return 0;
    }

    if (strstr(rx, "\"cmd\":\"reset\"") || strstr(rx, "reset"))
    {
        g_state = CONTROL_STATE_IDLE;
        g_rx_count = 0;
        g_start_count = 0;
        g_stop_count = 0;
        g_error_count = 0;
        g_work_count = 0;
        write_status(tx, tx_size, RT_TRUE);
        unlock_control();
        return 0;
    }

    g_error_count++;
    rt_snprintf(tx, tx_size,
                "{\"ok\":false,\"error\":\"unknown_cmd\",\"state\":\"%s\","
                "\"work_count\":%u}\n",
                state_name(g_state),
                g_work_count);
    unlock_control();
    return -1;
}
