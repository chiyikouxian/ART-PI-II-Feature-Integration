/**
 * @file model_queue.h
 * @brief Bounded FIFO of model/calibration payloads for the left-hand ROCK path.
 *
 * Identical semantics to the right-hand model_queue, but maintained separately
 * to avoid pulling the full right-hand network stack into the left-hand build.
 */
#ifndef __MODEL_QUEUE_H__
#define __MODEL_QUEUE_H__

#include <rtthread.h>

#define NET_MODEL_MSG_MAX     256
#define NET_MODEL_QUEUE_CAP   4

typedef struct {
    rt_uint32_t pushed;
    rt_uint32_t popped;
    rt_uint32_t dropped;
    rt_uint32_t short_writes;
    rt_uint32_t retries;
} model_queue_stats_t;

void model_queue_init(void);
rt_err_t model_queue_push(const void *data, int len);
rt_err_t model_queue_peek(const char **data_out, int *len_out);
void model_queue_pop(void);
void model_queue_advance_head(int n);
void model_queue_reset_partial_sends(void);
void model_queue_get_stats(model_queue_stats_t *out);

#endif /* __MODEL_QUEUE_H__ */
