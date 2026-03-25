/*
 * Copyright (c) 2006-2025, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Description: Audio Processing Module Implementation
 *              Integrated from I2S_test-main project.
 *              Modified for voice assistant integration:
 *              - Thread is created/destroyed on each start/stop cycle
 *              - Max recording duration enforced
 *              - Noise reduction filter state reset on each recording
 */

#include "audio_process.h"
#include <string.h>
#include <stdlib.h>

#define DBG_TAG "audio.process"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* Audio Processing Context */
typedef struct {
    rt_thread_t process_thread;         /* Processing thread */
    rt_bool_t running;                  /* Running flag */

    audio_state_t state;                /* Current state */
    audio_recording_t recording;        /* Current recording */

    uint32_t vad_hangover_count;        /* VAD hangover counter */
    uint32_t speech_frame_count;        /* Frames with actual speech during recording */
    speech_data_callback_t callback;    /* User callback */

    audio_stats_t stats;                /* Statistics */
    rt_mutex_t lock;                    /* Mutex for state protection */

    /* Noise filter state (reset on each recording cycle) */
    int32_t nr_prev_sample;
    int32_t nr_prev_output;
} audio_process_ctx_t;

static audio_process_ctx_t g_audio_ctx = {0};

/* Forward declarations */
static void audio_process_thread_entry(void *parameter);
static rt_bool_t vad_detect_speech(audio_frame_t *frame);

/**
 * @brief Initialize audio processing module
 */
rt_err_t audio_process_init(speech_data_callback_t callback)
{
    audio_process_ctx_t *ctx = &g_audio_ctx;

    LOG_I("Initializing audio processing module...");

    ctx->callback = callback;
    ctx->state = AUDIO_STATE_IDLE;
    ctx->running = RT_FALSE;
    ctx->process_thread = RT_NULL;

    /* Create mutex */
    if (ctx->lock == RT_NULL)
    {
        ctx->lock = rt_mutex_create("audio_lock", RT_IPC_FLAG_FIFO);
        if (ctx->lock == RT_NULL)
        {
            LOG_E("Failed to create mutex");
            return -RT_ENOMEM;
        }
    }

    /* Allocate recording buffer (max duration at 16kHz) */
    if (ctx->recording.data == RT_NULL)
    {
        ctx->recording.capacity = INMP441_SAMPLE_RATE * AUDIO_PROCESS_MAX_RECORD_SEC;
        uint32_t buffer_size = ctx->recording.capacity * sizeof(int32_t);

        LOG_I("Allocating %d bytes (%d KB) for recording buffer (%d sec max)",
               buffer_size, buffer_size / 1024, AUDIO_PROCESS_MAX_RECORD_SEC);

        ctx->recording.data = rt_malloc(buffer_size);
        if (ctx->recording.data == RT_NULL)
        {
            LOG_E("Failed to allocate recording buffer");
            rt_mutex_delete(ctx->lock);
            ctx->lock = RT_NULL;
            return -RT_ENOMEM;
        }
    }

    ctx->recording.size = 0;
    ctx->recording.sample_rate = INMP441_SAMPLE_RATE;

    /* Compute VAD energy threshold from dB SPL setting */
    LOG_I("VAD threshold: %u (energy)", VAD_THRESHOLD);
    LOG_I("Audio processing module initialized");
    return RT_EOK;
}

/**
 * @brief Deinitialize audio processing module
 */
rt_err_t audio_process_deinit(void)
{
    audio_process_ctx_t *ctx = &g_audio_ctx;

    /* Stop processing if running */
    if (ctx->running)
    {
        audio_process_stop();
    }

    /* Free recording buffer */
    if (ctx->recording.data != RT_NULL)
    {
        rt_free(ctx->recording.data);
        ctx->recording.data = RT_NULL;
    }

    /* Delete mutex */
    if (ctx->lock != RT_NULL)
    {
        rt_mutex_delete(ctx->lock);
        ctx->lock = RT_NULL;
    }

    LOG_I("Audio processing deinitialized");
    return RT_EOK;
}

/**
 * @brief Start audio processing - creates a new thread each time
 */
rt_err_t audio_process_start(void)
{
    audio_process_ctx_t *ctx = &g_audio_ctx;

    if (ctx->running)
    {
        LOG_W("Already running");
        return RT_EOK;
    }

    /* Reset state for new recording cycle */
    ctx->state = AUDIO_STATE_IDLE;
    ctx->recording.size = 0;
    ctx->vad_hangover_count = 0;
    ctx->nr_prev_sample = 0;
    ctx->nr_prev_output = 0;
    ctx->running = RT_TRUE;

    /* Create a new processing thread */
    ctx->process_thread = rt_thread_create("audio_proc",
                                           audio_process_thread_entry,
                                           RT_NULL,
                                           AUDIO_PROCESS_STACK_SIZE,
                                           AUDIO_PROCESS_PRIORITY,
                                           20);
    if (ctx->process_thread == RT_NULL)
    {
        LOG_E("Failed to create processing thread");
        ctx->running = RT_FALSE;
        return -RT_ENOMEM;
    }

    rt_thread_startup(ctx->process_thread);

    LOG_D("Audio processing started");
    return RT_EOK;
}

/**
 * @brief Stop audio processing - waits for thread to exit
 */
rt_err_t audio_process_stop(void)
{
    audio_process_ctx_t *ctx = &g_audio_ctx;

    if (!ctx->running)
        return RT_EOK;

    ctx->running = RT_FALSE;

    /* Wait for thread to exit */
    if (ctx->process_thread != RT_NULL)
    {
        rt_thread_mdelay(100);
        ctx->process_thread = RT_NULL;
    }

    ctx->state = AUDIO_STATE_IDLE;
    LOG_D("Audio processing stopped");
    return RT_EOK;
}

/**
 * @brief Audio processing thread
 */
static void audio_process_thread_entry(void *parameter)
{
    audio_process_ctx_t *ctx = &g_audio_ctx;
    audio_frame_t frame;

    LOG_D("Audio processing thread started");

    while (ctx->running)
    {
        /* Read frame from I2S driver with timeout to allow checking running flag */
        if (inmp441_read_frame(&frame, rt_tick_from_millisecond(500)) != RT_EOK)
        {
            continue;
        }

        /* Update statistics */
        ctx->stats.frames_processed++;

        /* Apply noise reduction */
        audio_noise_reduction(&frame);

        /* Calculate energy */
        uint32_t energy = audio_calculate_energy(&frame);

        /* Update energy statistics */
        ctx->stats.avg_energy = (ctx->stats.avg_energy * 0.9f) + (energy * 0.1f);
        if (energy > ctx->stats.max_energy)
            ctx->stats.max_energy = energy;

        /* VAD - Voice Activity Detection */
        rt_bool_t speech_detected = vad_detect_speech(&frame);

        rt_mutex_take(ctx->lock, RT_WAITING_FOREVER);

        switch (ctx->state)
        {
            case AUDIO_STATE_IDLE:
                if (speech_detected)
                {
                    /* Start recording */
                    ctx->state = AUDIO_STATE_RECORDING;
                    ctx->recording.size = 0;
                    ctx->recording.start_time = rt_tick_get();
                    ctx->vad_hangover_count = VAD_HANGOVER_FRAMES;
                    ctx->speech_frame_count = 1;  /* This frame has speech */

                    LOG_D("Recording started (energy=%u)", energy);

                    /* Copy frame to recording buffer */
                    if (ctx->recording.size + frame.size <= ctx->recording.capacity)
                    {
                        rt_memcpy(&ctx->recording.data[ctx->recording.size],
                                 frame.buffer,
                                 frame.size * sizeof(int32_t));
                        ctx->recording.size += frame.size;
                    }
                }
                break;

            case AUDIO_STATE_RECORDING:
                if (speech_detected)
                {
                    /* Continue recording */
                    ctx->vad_hangover_count = VAD_HANGOVER_FRAMES;
                    ctx->speech_frame_count++;

                    /* Copy frame to recording buffer */
                    if (ctx->recording.size + frame.size <= ctx->recording.capacity)
                    {
                        rt_memcpy(&ctx->recording.data[ctx->recording.size],
                                 frame.buffer,
                                 frame.size * sizeof(int32_t));
                        ctx->recording.size += frame.size;
                    }
                    else
                    {
                        LOG_I("Recording buffer full");
                        goto finish_recording;
                    }
                }
                else
                {
                    /* No speech - decrement hangover */
                    if (ctx->vad_hangover_count > 0)
                    {
                        ctx->vad_hangover_count--;

                        /* Still in hangover - continue recording */
                        if (ctx->recording.size + frame.size <= ctx->recording.capacity)
                        {
                            rt_memcpy(&ctx->recording.data[ctx->recording.size],
                                     frame.buffer,
                                     frame.size * sizeof(int32_t));
                            ctx->recording.size += frame.size;
                        }
                    }
                    else
                    {
finish_recording:
                        /* Hangover expired or buffer full - finish recording */
                        ctx->recording.end_time = rt_tick_get();

                        uint32_t duration_ms = (ctx->recording.end_time - ctx->recording.start_time)
                                               * 1000 / RT_TICK_PER_SECOND;

                        /* Check if enough actual speech frames were recorded */
                        if (ctx->speech_frame_count < VAD_MIN_SPEECH_FRAMES)
                        {
                            LOG_D("Noise rejected: only %d speech frames (need %d), %d ms",
                                  ctx->speech_frame_count, VAD_MIN_SPEECH_FRAMES, duration_ms);
                            /* Reset to idle, do NOT call callback */
                            ctx->state = AUDIO_STATE_IDLE;
                            ctx->recording.size = 0;
                            break;
                        }

                        ctx->stats.total_duration_ms += duration_ms;
                        ctx->stats.speech_detected++;

                        LOG_I("Recording finished: %d ms, %d samples, %d speech frames",
                              duration_ms, ctx->recording.size, ctx->speech_frame_count);

                        /* Call user callback */
                        if (ctx->callback != RT_NULL)
                        {
                            ctx->callback(&ctx->recording);
                        }

                        /* Reset to idle */
                        ctx->state = AUDIO_STATE_IDLE;
                        ctx->recording.size = 0;

                        /* After delivering speech, stop the thread
                         * (voice_assistant will restart when needed) */
                        ctx->running = RT_FALSE;
                    }
                }
                break;

            default:
                break;
        }

        rt_mutex_release(ctx->lock);

        /* Free frame buffer allocated by driver */
        if (frame.buffer != RT_NULL)
        {
            rt_free(frame.buffer);
        }
    }

    LOG_D("Audio processing thread exited");
}

/**
 * @brief Voice Activity Detection
 */
static rt_bool_t vad_detect_speech(audio_frame_t *frame)
{
    uint32_t energy = audio_calculate_energy(frame);
    return (energy > VAD_THRESHOLD);
}

/**
 * @brief Calculate audio frame energy
 */
uint32_t audio_calculate_energy(audio_frame_t *frame)
{
    if (frame == RT_NULL || frame->buffer == RT_NULL)
        return 0;

    uint64_t sum = 0;
    for (uint32_t i = 0; i < frame->size; i++)
    {
        int32_t sample = frame->buffer[i] >> 8;  /* Scale down to prevent overflow */
        sum += (uint64_t)(sample * sample);
    }

    return (uint32_t)(sum / frame->size);
}

/**
 * @brief Simple noise reduction (high-pass filter)
 */
void audio_noise_reduction(audio_frame_t *frame)
{
    if (frame == RT_NULL || frame->buffer == RT_NULL)
        return;

    audio_process_ctx_t *ctx = &g_audio_ctx;

    /* Simple high-pass filter: y[n] = x[n] - x[n-1] + 0.95 * y[n-1] */
    const float alpha = 0.95f;

    for (uint32_t i = 0; i < frame->size; i++)
    {
        int32_t current = frame->buffer[i];
        int32_t output = current - ctx->nr_prev_sample + (int32_t)(alpha * ctx->nr_prev_output);

        ctx->nr_prev_sample = current;
        ctx->nr_prev_output = output;

        frame->buffer[i] = output;
    }
}

/**
 * @brief Convert 32-bit PCM to 16-bit PCM
 */
void audio_convert_32to16(int32_t *input, int16_t *output, uint32_t samples)
{
    for (uint32_t i = 0; i < samples; i++)
    {
        /* Scale down from 24-bit to 16-bit */
        output[i] = (int16_t)(input[i] >> 8);
    }
}

/**
 * @brief Get current state
 */
audio_state_t audio_process_get_state(void)
{
    return g_audio_ctx.state;
}

/**
 * @brief Get statistics
 */
void audio_process_get_stats(audio_stats_t *stats)
{
    if (stats != RT_NULL)
    {
        rt_mutex_take(g_audio_ctx.lock, RT_WAITING_FOREVER);
        rt_memcpy(stats, &g_audio_ctx.stats, sizeof(audio_stats_t));
        rt_mutex_release(g_audio_ctx.lock);
    }
}

/**
 * @brief Reset statistics
 */
void audio_process_reset_stats(void)
{
    rt_mutex_take(g_audio_ctx.lock, RT_WAITING_FOREVER);
    rt_memset(&g_audio_ctx.stats, 0, sizeof(audio_stats_t));
    rt_mutex_release(g_audio_ctx.lock);
}
