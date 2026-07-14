/**
 * @file net_io.h
 * @brief Shared network I/O helpers for ART-Pi2 TCP paths.
 *
 * Centralises the reliability primitives that used to be copy-pasted into
 * tcp_client.c / imu_wifi_sender.c on both hands:
 *
 *   - send_all()        : loop send() until the whole buffer is drained,
 *                         honouring SO_SNDTIMEO. Returns the number of
 *                         bytes actually transmitted, or -1 on
 *                         unrecoverable error (so callers can decide
 *                         whether to reconnect vs. just retry).
 *   - send_all_deadline : same as above, but bounds the total wall time
 *                         so a slow path can't block the producer.
 *   - send_frame()      : send a framed text/CSV payload and only signal
 *                         success when every byte made it through.
 *
 * Future commits add parse_accumulate(), ping/pong bookkeeping, link
 * counters and the model queue on top of these primitives.
 */
#ifndef __NET_IO_H
#define __NET_IO_H

#include <rtthread.h>

/**
 * @brief  Callback invoked once per complete LF-terminated line.
 *
 * @param  data       Pointer into the caller's buffer (NOT NUL-terminated).
 * @param  len        Number of bytes in the line, excluding the trailing LF.
 * @param  user_data  Forwarded from net_io_consume_lines().
 *
 * Lines that exceed the accumulator are surfaced in chunks of MAX_LINE
 * so the callback can decide whether to drop, log, or partially parse.
 */
typedef void (*net_io_line_cb_t)(const char *data, int len, void *user_data);

/**
 * @brief  Maximum line length that will be surfaced to callbacks. Anything
 *         longer is split into MAX_LINE-sized chunks to prevent one rogue
 *         publisher from starving the parser.
 */
#define NET_IO_MAX_LINE         512

/**
 * @brief  Loop send() until all `len` bytes are written or an error occurs.
 *
 * @param  sock     Connected socket (must be > 0).
 * @param  buf      Data to transmit.
 * @param  len      Byte count.
 * @param  timeout_ms  Per-send() SO_SNDTIMEO override applied for the
 *                     duration of this call (preserves caller intent if
 *                     the socket was opened with a longer timeout).
 * @return >0  : total bytes transmitted (< len means short write, treat as
 *              error and reconnect — see net_io.c for policy).
 *         0  : peer closed.
 *         -1 : unrecoverable error, errno preserved via lwIP.
 */
int net_io_send_all(int sock, const void *buf, int len, int timeout_ms);

/**
 * @brief  send_all() with an absolute wall-clock deadline.
 *
 * @param  deadline_ms  rt_tick_get()-based upper bound. If 0, falls back
 *                      to net_io_send_all() with the supplied per-call
 *                      timeout.
 * @return See net_io_send_all().
 */
int net_io_send_all_deadline(int sock,
                             const void *buf,
                             int len,
                             int timeout_ms,
                             rt_tick_t deadline_ms);

/**
 * @brief  Convenience wrapper that guarantees the whole payload landed.
 *
 *         Returns RT_EOK on success, -RT_ERROR otherwise. Useful for
 *         "if this fails, drop the message" producers.
 */
rt_err_t net_io_send_frame(int sock, const void *buf, int len);

/**
 * @brief  Frame a single line of text and send it (caller owns NUL term).
 *
 *         Appends '\n' if absent, then calls net_io_send_frame(). Returns
 *         RT_EOK only when the whole line + LF landed.
 */
rt_err_t net_io_send_line(int sock, const char *line);

/**
 * @brief  In-place consume of an accumulator with a newline-framed producer.
 *
 *         The producer passes the bytes it just recv()'d; this function:
 *           1. Appends them to the accumulator (caller-owned storage).
 *           2. Extracts every LF-terminated line and invokes @p on_line.
 *           3. Compacts the accumulator, leaving the trailing partial
 *              fragment (if any) intact for the next call.
 *           4. If the accumulator grows past @p cap, the oldest bytes are
 *              dropped to keep memory bounded; on_line is invoked with the
 *              truncated chunk and the caller can decide what to do.
 *
 * @param  acc         Caller-owned accumulator (state is preserved across calls).
 * @param  acc_len     [in/out] Current length on entry, new length on exit.
 * @param  cap         Total capacity of `acc`.
 * @param  chunk       Bytes freshly received from the socket.
 * @param  chunk_len   Number of bytes in `chunk`.
 * @param  on_line     Callback invoked once per complete line.
 * @param  user_data   Forwarded to on_line.
 * @param  parse_err   [out] Counter incremented when the accumulator was
 *                      truncated or a line exceeded NET_IO_MAX_LINE.
 * @return RT_EOK on success; -RT_ERROR only on programmer error.
 */
rt_err_t net_io_consume_lines(char *acc,
                              int  *acc_len,
                              int   cap,
                              const char *chunk,
                              int   chunk_len,
                              net_io_line_cb_t on_line,
                              void *user_data,
                              rt_uint32_t *parse_err);

#endif /* __NET_IO_H */
