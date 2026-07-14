/**
 * @file net_io.c
 * @brief Reliability primitives shared by tcp_client.c / imu_wifi_sender.c.
 *
 * Why this exists
 * ---------------
 * Both right-hand senders and (eventually) the left-hand senders were
 * writing the same short-write bug: a single send() returning less than
 * the requested length silently dropped the tail. The PC frontend or the
 * ROCK consumer then saw a half-truncated JSON / CSV row and either
 * crashed the parser or, worse, attributed a sibling sensor row to the
 * wrong hand.
 *
 * The fix is intentionally tiny and dependency-free: a loop that
 * observes the per-call timeout, retries with the remainder, and gives
 * the caller a single boolean signal.
 */

#include <rtthread.h>
#include <errno.h>
#include <string.h>
#include <sys/time.h>
#include <sys/socket.h>

#include "net_io.h"

#define DBG_SECTION_NAME    "net_io"
#define DBG_LEVEL           DBG_INFO
#include <rtdbg.h>

/**
 * @brief  Apply a SO_SNDTIMEO override on the socket, returning the
 *         previous timeout so the caller can restore it.
 */
static void apply_send_timeout(int sock, int timeout_ms, struct timeval *saved)
{
    struct timeval tv;
    socklen_t len = sizeof(*saved);

    if (saved != RT_NULL)
    {
        getsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, saved, &len);
    }

    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static void restore_send_timeout(int sock, const struct timeval *saved)
{
    if (saved == RT_NULL)
        return;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, saved, sizeof(*saved));
}

int net_io_send_all(int sock, const void *buf, int len, int timeout_ms)
{
    const char *p = (const char *)buf;
    int total = 0;
    struct timeval saved;

    if (sock < 0 || buf == RT_NULL || len <= 0)
        return -1;

    apply_send_timeout(sock, timeout_ms, &saved);

    while (total < len)
    {
        int n = send(sock, p + total, len - total, 0);
        if (n > 0)
        {
            total += n;
            continue;
        }

        if (n == 0)
        {
            /* Per POSIX send() can't return 0 on stream sockets; treat
             * as no-progress and surface to caller. */
            break;
        }

        /* n < 0 */
        if (errno == EINTR)
        {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            /* SO_SNDTIMEO expired — let caller decide. */
            break;
        }

        LOG_W("send_all hard error: errno=%d", errno);
        restore_send_timeout(sock, &saved);
        return -1;
    }

    restore_send_timeout(sock, &saved);

    if (total < len)
    {
        LOG_W("send_all short write: %d/%d", total, len);
    }

    return total;
}

int net_io_send_all_deadline(int sock,
                             const void *buf,
                             int len,
                             int timeout_ms,
                             rt_tick_t deadline_ms)
{
    const char *p = (const char *)buf;
    int total = 0;
    struct timeval saved;

    if (sock < 0 || buf == RT_NULL || len <= 0)
        return -1;

    if (deadline_ms == 0)
        return net_io_send_all(sock, buf, len, timeout_ms);

    apply_send_timeout(sock, timeout_ms, &saved);

    while (total < len)
    {
        rt_tick_t now = rt_tick_get();
        if (now >= deadline_ms)
        {
            LOG_W("send_all_deadline expired after %d/%d", total, len);
            break;
        }

        int n = send(sock, p + total, len - total, 0);
        if (n > 0)
        {
            total += n;
            continue;
        }

        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;

            LOG_W("send_all_deadline hard error: errno=%d", errno);
            restore_send_timeout(sock, &saved);
            return -1;
        }

        break;
    }

    restore_send_timeout(sock, &saved);

    if (total < len && total > 0)
    {
        LOG_W("send_all_deadline short write: %d/%d", total, len);
    }

    return total;
}

rt_err_t net_io_send_frame(int sock, const void *buf, int len)
{
    int n = net_io_send_all(sock, buf, len, 3000);
    if (n < 0 || n < len)
        return -RT_ERROR;
    return RT_EOK;
}

rt_err_t net_io_send_line(int sock, const char *line)
{
    int len;

    if (sock < 0 || line == RT_NULL)
        return -RT_ERROR;

    len = (int)rt_strlen(line);
    if (len <= 0)
        return -RT_ERROR;

    if (net_io_send_all(sock, line, len, 3000) < len)
        return -RT_ERROR;

    /* Single trailing '\n' if not already present. */
    if (line[len - 1] != '\n')
    {
        const char nl = '\n';
        if (net_io_send_all(sock, &nl, 1, 1000) < 1)
            return -RT_ERROR;
    }

    return RT_EOK;
}

rt_err_t net_io_consume_lines(char *acc,
                              int  *acc_len,
                              int   cap,
                              const char *chunk,
                              int   chunk_len,
                              net_io_line_cb_t on_line,
                              void *user_data,
                              rt_uint32_t *parse_err)
{
    int used;
    int copy_len;

    if (acc == RT_NULL || acc_len == RT_NULL || chunk == RT_NULL || on_line == RT_NULL)
        return -RT_ERROR;
    if (cap <= 0)
        return -RT_ERROR;

    used = *acc_len;
    if (used < 0 || used > cap)
        used = 0;  /* defensive reset if caller handed us garbage */

    /* Append, dropping head bytes if we run out of room. */
    if (chunk_len > 0)
    {
        int space = cap - used;
        if (chunk_len > space)
        {
            /* Drop the oldest bytes so the freshest tail survives. */
            int drop = chunk_len - space;
            rt_memmove(acc, acc + drop, used - drop);
            used -= drop;
            if (parse_err != RT_NULL)
                (*parse_err)++;
        }
        rt_memcpy(acc + used, chunk, chunk_len);
        used += chunk_len;
    }

    /* Extract complete LF-terminated lines from the front. */
    copy_len = used;
    while (copy_len > 0)
    {
        char *nl = memchr(acc, '\n', copy_len);
        if (nl == RT_NULL)
            break;

        int line_len = (int)(nl - acc);
        if (line_len > NET_IO_MAX_LINE)
        {
            if (parse_err != RT_NULL)
                (*parse_err)++;
            /* Still surface the prefix in chunks so the caller at least
             * sees a sane tail; advance past the LF. */
            int sent = 0;
            while (sent < line_len)
            {
                int chunk_size = line_len - sent;
                if (chunk_size > NET_IO_MAX_LINE)
                    chunk_size = NET_IO_MAX_LINE;
                on_line(acc + sent, chunk_size, user_data);
                sent += chunk_size;
            }
        }
        else
        {
            on_line(acc, line_len, user_data);
        }

        copy_len -= (line_len + 1);
        rt_memmove(acc, nl + 1, copy_len);
    }

    *acc_len = copy_len;
    return RT_EOK;
}
