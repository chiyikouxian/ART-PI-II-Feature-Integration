/**
 * @file model_queue.c
 * @brief Bounded FIFO of model/calibration payloads for ROCK.
 */

#include <rtthread.h>
#include <string.h>

#include "model_queue.h"

#define DBG_SECTION_NAME    "modelq"
#define DBG_LEVEL           DBG_INFO
#include <rtdbg.h>

typedef struct {
    char     data[NET_MODEL_MSG_MAX];
    uint16_t len;
    uint8_t  in_use;
    uint16_t sent_offset;   /* bytes already sent; resume from here on retry */
} model_slot_t;

static model_slot_t        g_slots[NET_MODEL_QUEUE_CAP];
static rt_uint8_t          g_head = 0;   /* oldest message           */
static rt_uint8_t          g_tail = 0;   /* next free slot           */
static rt_uint8_t          g_count = 0;  /* currently queued         */
static rt_bool_t           g_inited = RT_FALSE;
static model_queue_stats_t g_stats;

void model_queue_init(void)
{
    if (g_inited) return;
    rt_memset(g_slots, 0, sizeof(g_slots));
    g_head = g_tail = g_count = 0;
    rt_memset(&g_stats, 0, sizeof(g_stats));
    g_inited = RT_TRUE;
}

static int next(int i)
{
    int j = i + 1;
    if (j >= NET_MODEL_QUEUE_CAP) j = 0;
    return j;
}

rt_err_t model_queue_push(const void *data, int len)
{
    if (!g_inited) model_queue_init();

    if (data == RT_NULL || len <= 0)
        return -RT_EINVAL;
    if (len > NET_MODEL_MSG_MAX)
    {
        LOG_W("modelq: msg too large (%d > %d), dropped", len, NET_MODEL_MSG_MAX);
        g_stats.dropped++;
        return -RT_EINVAL;
    }

    rt_base_t level = rt_hw_interrupt_disable();
    if (g_count >= NET_MODEL_QUEUE_CAP)
    {
        rt_hw_interrupt_enable(level);
        g_stats.dropped++;
        LOG_W("modelq: full, drop (pushed=%u dropped=%u)",
              g_stats.pushed, g_stats.dropped);
        return -RT_EFULL;
    }

    rt_memcpy(g_slots[g_tail].data, data, len);
    g_slots[g_tail].len = (uint16_t)len;
    g_slots[g_tail].sent_offset = 0;
    g_slots[g_tail].in_use = 1;
    g_tail = next(g_tail);
    g_count++;
    g_stats.pushed++;
    rt_hw_interrupt_enable(level);

    return RT_EOK;
}

rt_err_t model_queue_peek(const char **data_out, int *len_out)
{
    if (!g_inited) model_queue_init();

    if (g_count == 0)
        return -RT_EEMPTY;

    if (data_out != RT_NULL)
        *data_out = g_slots[g_head].data + g_slots[g_head].sent_offset;
    if (len_out != RT_NULL)
        *len_out = (int)(g_slots[g_head].len - g_slots[g_head].sent_offset);
    return RT_EOK;
}

void model_queue_pop(void)
{
    if (!g_inited || g_count == 0) return;

    rt_base_t level = rt_hw_interrupt_disable();
    rt_memset(&g_slots[g_head], 0, sizeof(g_slots[0]));
    g_slots[g_head].in_use = 0;
    g_head = next(g_head);
    g_count--;
    g_stats.popped++;
    rt_hw_interrupt_enable(level);
}

void model_queue_advance_head(int n)
{
    if (!g_inited || g_count == 0) return;

    rt_base_t level = rt_hw_interrupt_disable();
    uint16_t remaining = g_slots[g_head].len - g_slots[g_head].sent_offset;
    if (n >= (int)remaining)
    {
        /* Complete send — pop it. */
        g_slots[g_head].sent_offset = g_slots[g_head].len;
        rt_hw_interrupt_enable(level);
        model_queue_pop();
    }
    else
    {
        g_slots[g_head].sent_offset = (uint16_t)(g_slots[g_head].sent_offset + n);
        g_stats.short_writes++;
        g_stats.retries++;
        rt_hw_interrupt_enable(level);
    }
}

void model_queue_reset_partial_sends(void)
{
    if (!g_inited || g_count == 0) return;

    /* Issue 6 fix: capture the diagnostic info under the lock and emit
     * LOG_W AFTER re-enabling interrupts. Logging inside
     * rt_hw_interrupt_disable() can block on the UART, which is
     * unacceptable in a critical section. */
    uint16_t dropped_offset = 0;
    rt_bool_t did_drop = RT_FALSE;

    rt_base_t level = rt_hw_interrupt_disable();
    if (g_slots[g_head].in_use && g_slots[g_head].sent_offset > 0)
    {
        dropped_offset = g_slots[g_head].sent_offset;
        did_drop = RT_TRUE;
        rt_memset(&g_slots[g_head], 0, sizeof(g_slots[0]));
        g_slots[g_head].in_use = 0;
        g_head = next(g_head);
        g_count--;
        g_stats.dropped++;
    }
    rt_hw_interrupt_enable(level);

    if (did_drop)
    {
        LOG_W("modelq: dropping in-progress partial (offset=%u)",
              dropped_offset);
    }
}

void model_queue_get_stats(model_queue_stats_t *out)
{
    if (out == RT_NULL) return;
    rt_base_t level = rt_hw_interrupt_disable();
    *out = g_stats;
    rt_hw_interrupt_enable(level);
}