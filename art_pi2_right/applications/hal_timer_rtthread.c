/*
 * hal_timer_rtthread.c — NimBLE HAL timer back-end for RT-Thread
 *
 * NimBLE's os_cputime module drives its host-side protocol timers through
 * five HAL timer primitives.  This file maps them to RT-Thread software
 * timers and RT-Thread's millisecond tick counter.
 *
 * Frequency contract:
 *   syscfg.h sets OS_CPUTIME_FREQ = 1 000 000 (1 MHz).
 *   os_cputime.h therefore defines OS_CPUTIME_FREQ_1MHZ and turns
 *   os_cputime_usecs_to_ticks / ticks_to_usecs into identity macros.
 *   We provide the remaining two non-pwr2 helpers here:
 *     os_cputime_nsecs_to_ticks   (nsecs / 1000)
 *     os_cputime_ticks_to_nsecs   (ticks * 1000)
 *
 * Resolution:
 *   hal_timer_read() returns rt_tick_get() * 1000, giving microsecond-
 *   scale "ticks" with 1 ms actual granularity.  For a host-only BLE
 *   stack (all protocol timing is 10 ms or more) this is sufficient.
 */

#include <rtthread.h>
#include "hal/hal_timer.h"
#include "os/os_cputime.h"

/* ── Internal RT-Thread timer callback ───────────────────────────────────── */

static void _hal_timer_rtt_cb(void *arg)
{
    struct hal_timer *tmr = (struct hal_timer *)arg;
    if (tmr != RT_NULL && tmr->cb_func != RT_NULL)
    {
        tmr->cb_func(tmr->cb_arg);
    }
}

/* ── HAL timer API ───────────────────────────────────────────────────────── */

/**
 * hal_timer_config — accept the requested frequency; no real HW timer needed.
 * The host-only path in nimble_port.c calls this only when
 * NIMBLE_CFG_CONTROLLER == 1, which is 0 in our config.  The linker still
 * pulls the function in because os_cputime_init() references it.
 */
int hal_timer_config(int timer_num, uint32_t freq_hz)
{
    (void)timer_num;
    (void)freq_hz;
    return 0;
}

/**
 * hal_timer_read — return current time in "os_cputime ticks" (≈ microseconds).
 * We scale RT-Thread's millisecond tick by 1000 to stay in the 1-MHz domain.
 */
uint32_t hal_timer_read(int timer_num)
{
    (void)timer_num;
    return (uint32_t)(rt_tick_get() * 1000U);
}

/**
 * hal_timer_set_cb — initialise a hal_timer struct with callback info.
 * The bsp_timer field (our RT-Thread timer handle) starts NULL.
 */
int hal_timer_set_cb(int timer_num, struct hal_timer *tmr,
                     hal_timer_cb cb_func, void *arg)
{
    (void)timer_num;
    if (tmr == RT_NULL)
        return -1;

    tmr->cb_func  = cb_func;
    tmr->cb_arg   = arg;
    tmr->bsp_timer = RT_NULL;
    return 0;
}

/**
 * hal_timer_start_at — schedule tmr to fire at absolute tick value 'tick'.
 *
 * We create a one-shot soft timer on first use and reuse it on subsequent
 * calls (stop → change timeout → start).
 */
int hal_timer_start_at(struct hal_timer *tmr, uint32_t tick)
{
    if (tmr == RT_NULL)
        return -1;

    /* Calculate remaining microseconds; clamp to at least 1 ms */
    uint32_t now     = hal_timer_read(0);
    int32_t  diff_us = (int32_t)(tick - now);

    rt_tick_t delay_ticks = (diff_us > 0)
        ? (rt_tick_t)((diff_us + 999U) / 1000U)   /* round up µs → ms */
        : (rt_tick_t)1;                             /* already expired: fire ASAP */

    rt_timer_t rtt = (rt_timer_t)tmr->bsp_timer;

    if (rtt == RT_NULL)
    {
        /* First call: create the RT-Thread one-shot soft timer */
        rtt = rt_timer_create("hlt", _hal_timer_rtt_cb, tmr,
                              delay_ticks,
                              RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_SOFT_TIMER);
        if (rtt == RT_NULL)
            return -1;
        tmr->bsp_timer = (void *)rtt;
    }
    else
    {
        /* Subsequent call: reuse existing timer object */
        rt_timer_stop(rtt);
        rt_timer_control(rtt, RT_TIMER_CTRL_SET_TIME, &delay_ticks);
    }

    rt_timer_start(rtt);
    return 0;
}

/**
 * hal_timer_stop — cancel a pending timer without firing its callback.
 */
int hal_timer_stop(struct hal_timer *tmr)
{
    if (tmr == RT_NULL)
        return -1;

    rt_timer_t rtt = (rt_timer_t)tmr->bsp_timer;
    if (rtt != RT_NULL)
        rt_timer_stop(rtt);

    return 0;
}

/* ── os_cputime helpers for the !OS_CPUTIME_FREQ_PWR2 build path ─────────── */
/* At 1 MHz, 1 tick = 1 µs = 1 000 ns.                                       */

uint32_t os_cputime_nsecs_to_ticks(uint32_t nsecs)
{
    return nsecs / 1000U;
}

uint32_t os_cputime_ticks_to_nsecs(uint32_t ticks)
{
    return ticks * 1000U;
}
