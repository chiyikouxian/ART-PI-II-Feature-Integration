/**
 * @file model_queue.h
 * @brief Bounded FIFO of model/calibration payloads destined for ROCK.
 *
 * Replaces the legacy single-slot `model_buf` in imu_wifi_sender.c.
 * That buffer had two failure modes:
 *   - a producer could overwrite a still-unsent message (data loss);
 *   - the sender cleared `model_buf_len` even when send() short-wrote,
 *     so the consumer never received the trailing bytes.
 *
 * The new queue is bounded (NET_MODEL_QUEUE_CAP messages) with per-
 * message size cap. When the queue is full, new producers either block
 * briefly or get a "queue full" error and bump a counter; they never
 * silently overwrite existing work.
 *
 * Thread model
 * ------------
 * Push side: any context (typically the calibration ISR or a low-
 * priority producer). Protected by a critical section; messages are
 * copied in (caller retains ownership of the source buffer).
 *
 * Pop side: the imu_wifi_sender worker thread only. Pops the head,
 * tries to send it via net_io_send_all(), and only dequeues on a
 * complete send. On a short-write the message stays in the head slot
 * and the worker retries next cycle.
 *
 * Counters are exposed for the net_stat MSH command (future commit).
 */
#ifndef __MODEL_QUEUE_H
#define __MODEL_QUEUE_H

#include <rtthread.h>

#define NET_MODEL_MSG_MAX     256   /* per-message byte cap           */
#define NET_MODEL_QUEUE_CAP   4     /* messages, ~1 KB total          */

typedef struct {
    rt_uint32_t pushed;
    rt_uint32_t popped;
    rt_uint32_t dropped;          /* producer side, queue was full  */
    rt_uint32_t short_writes;     /* pop side, incomplete send      */
    rt_uint32_t retries;          /* pop side, same msg rescheduled */
} model_queue_stats_t;

/**
 * @brief  Initialise the queue. Idempotent; safe to call from
 *         imu_wifi_sender_start() before the worker is created.
 */
void model_queue_init(void);

/**
 * @brief  Push a message. Returns RT_EOK on success, -RT_EFULL when
 *         the queue is at capacity. Never overwrites existing data.
 */
rt_err_t model_queue_push(const void *data, int len);

/**
 * @brief  Peek at the head message. *data_out / *len_out point at
 *         internal storage; valid until the next pop.
 *
 * @return RT_EOK if a message is available, -RT_EMPTY otherwise.
 */
rt_err_t model_queue_peek(const char **data_out, int *len_out);

/**
 * @brief  Drop the head message. Must only be called after a complete
 *         send. On partial send, leave the message in place and call
 *         model_queue_advance_head() instead.
 */
void model_queue_pop(void);

/**
 * @brief  Advance the head message's sent cursor by @p n bytes.
 *         Called after a partial send to record progress so the next
 *         retry starts from the right offset instead of from 0.
 * @param  n  Number of bytes successfully sent.
 */
void model_queue_advance_head(int n);

/**
 * @brief  Reset any in-progress partial-send offset on the head message.
 *         Called when the underlying socket dies so the next connection
 *         does NOT resume mid-message — a fresh MODEL payload must be
 *         delivered from offset 0 to preserve message framing on the peer.
 */
void model_queue_reset_partial_sends(void);

void model_queue_get_stats(model_queue_stats_t *out);

#endif /* __MODEL_QUEUE_H */