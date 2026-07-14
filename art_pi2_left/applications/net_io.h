/**
 * @file net_io.h
 * @brief Shared network I/O helpers for ART-Pi2 TCP paths (left-hand build).
 */
#ifndef __NET_IO_H
#define __NET_IO_H

#include <rtthread.h>

/**
 * @brief  Callback invoked once per complete LF-terminated line.
 */
typedef void (*net_io_line_cb_t)(const char *data, int len, void *user_data);

#define NET_IO_MAX_LINE         512

/**
 * @brief  Loop send() until all `len` bytes are written or an error occurs.
 * @return >0: total bytes transmitted; 0: peer closed; -1: unrecoverable error.
 */
int net_io_send_all(int sock, const void *buf, int len, int timeout_ms);

/**
 * @brief  send_all() with an absolute wall-clock deadline.
 * @return Same as net_io_send_all().
 */
int net_io_send_all_deadline(int sock,
                             const void *buf,
                             int len,
                             int timeout_ms,
                             rt_tick_t deadline_ms);

/**
 * @brief  Send entire payload; RT_EOK on complete send, -RT_ERROR on failure.
 */
rt_err_t net_io_send_frame(int sock, const void *buf, int len);

/**
 * @brief  Send a line of text, appending '\n' if absent.
 */
rt_err_t net_io_send_line(int sock, const char *line);

/**
 * @brief  Accumulate socket bytes by '\n', invoke callback per complete line.
 * @param  acc         Caller-owned accumulator buffer.
 * @param  acc_len     [in/out] current accumulator length.
 * @param  cap         Total capacity of `acc`.
 * @param  chunk       Freshly received bytes from socket.
 * @param  chunk_len   Number of bytes in `chunk`.
 * @param  on_line     Callback per complete line.
 * @param  user_data   Forwarded to callback.
 * @param  parse_err   [out] Counter incremented on overflow/truncation.
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
