/*
 * Copyright (c) 2006-2025, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Description: Audio Processing Module Implementation
 */

#include "audio_process.h"
#include <string.h>
#include <stdlib.h>

#ifdef RT_USING_DFS
#include <dfs_file.h>
#ifdef RT_USING_POSIX_FS
#include <unistd.h>
#include <fcntl.h>
#else
#include <dfs_posix.h>
#endif
#endif

/* Audio Processing Context */
typedef struct {
    rt_thread_t process_thread;         /* Processing thread */
    rt_bool_t running;                  /* Running flag */

    audio_state_t state;                /* Current state */
    audio_recording_t recording;        /* Current recording */

    uint32_t vad_hangover_count;        /* VAD hangover counter */
    speech_data_callback_t callback;    /* User callback */

    audio_stats_t stats;                /* Statistics */
    rt_mutex_t lock;                    /* Mutex for state protection */
} audio_process_ctx_t;

/* ============== Enhanced VAD Context ============== */
typedef struct {
    /* 自适应阈值 */
    float noise_floor;                  /* 噪声底部 (自动校准) */
    float energy_threshold;             /* 当前能量阈值 */

    /* 能量平滑 */
    float smoothed_energy;              /* 平滑后的能量 */

    /* 连续检测计数 */
    uint32_t speech_frame_count;        /* 连续检测到语音的帧数 */
    uint32_t silence_frame_count;       /* 连续静音帧数 */

    /* 校准状态 */
    uint32_t calibration_count;         /* 校准帧计数 */
    rt_bool_t calibrated;               /* 是否已校准 */

    /* 调试信息 */
    uint32_t last_zcr;                  /* 上一帧过零率 */
    float last_energy;                  /* 上一帧能量 */
} vad_context_t;

static audio_process_ctx_t g_audio_ctx = {0};
static vad_context_t g_vad_ctx = {0};

/* Forward declarations */
static void audio_process_thread_entry(void *parameter);
static rt_bool_t vad_detect_speech(audio_frame_t *frame);
static rt_bool_t vad_detect_speech_enhanced(audio_frame_t *frame);
static uint32_t vad_calculate_zcr(audio_frame_t *frame);
static void vad_init(void);
static void vad_update_noise_floor(float energy);

/**
 * @brief Initialize audio processing module
 */
rt_err_t audio_process_init(speech_data_callback_t callback)
{
    audio_process_ctx_t *ctx = &g_audio_ctx;

    rt_kprintf("[AudioProcess] Initializing audio processing module...\n");

    /* Clear context */
    rt_memset(ctx, 0, sizeof(audio_process_ctx_t));

    ctx->callback = callback;
    ctx->state = AUDIO_STATE_IDLE;

    /* Initialize enhanced VAD */
    vad_init();

    /* Create mutex */
    ctx->lock = rt_mutex_create("audio_lock", RT_IPC_FLAG_FIFO);
    if (ctx->lock == RT_NULL)
    {
        rt_kprintf("[AudioProcess] Failed to create mutex\n");
        return -RT_ENOMEM;
    }

    /* Allocate recording buffer (max 1.5 seconds at 16kHz to save memory for WAV encoding) */
    ctx->recording.capacity = INMP441_SAMPLE_RATE * 3 / 2;  /* 1.5 seconds = 24000 samples */
    uint32_t buffer_size = ctx->recording.capacity * sizeof(int32_t);

    rt_kprintf("[AudioProcess] Allocating %d bytes (%d KB) for recording buffer\n",
               buffer_size, buffer_size / 1024);

    ctx->recording.data = rt_malloc(buffer_size);
    if (ctx->recording.data == RT_NULL)
    {
        rt_kprintf("[AudioProcess] Failed to allocate recording buffer\n");
        rt_kprintf("[AudioProcess] Try to free some memory or reduce buffer size\n");
        rt_mutex_delete(ctx->lock);
        return -RT_ENOMEM;
    }

    rt_kprintf("[AudioProcess] Recording buffer allocated successfully\n");
    ctx->recording.size = 0;
    ctx->recording.sample_rate = INMP441_SAMPLE_RATE;

    /* Create processing thread */
    ctx->process_thread = rt_thread_create("audio_proc",
                                          audio_process_thread_entry,
                                          RT_NULL,
                                          AUDIO_PROCESS_STACK_SIZE,
                                          AUDIO_PROCESS_PRIORITY,
                                          20);
    if (ctx->process_thread == RT_NULL)
    {
        rt_kprintf("[AudioProcess] Failed to create thread\n");
        rt_free(ctx->recording.data);
        rt_mutex_delete(ctx->lock);
        return -RT_ENOMEM;
    }

    rt_kprintf("[AudioProcess] Initialization successful\n");
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

    /* Delete thread */
    if (ctx->process_thread != RT_NULL)
    {
        rt_thread_delete(ctx->process_thread);
        ctx->process_thread = RT_NULL;
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

    rt_kprintf("[AudioProcess] Deinitialized\n");
    return RT_EOK;
}

/**
 * @brief Start audio processing
 */
rt_err_t audio_process_start(void)
{
    audio_process_ctx_t *ctx = &g_audio_ctx;

    if (ctx->running)
        return RT_EOK;

    ctx->running = RT_TRUE;

    /* Start processing thread */
    rt_thread_startup(ctx->process_thread);

    rt_kprintf("[AudioProcess] Processing started\n");
    return RT_EOK;
}

/**
 * @brief Stop audio processing
 */
rt_err_t audio_process_stop(void)
{
    audio_process_ctx_t *ctx = &g_audio_ctx;

    ctx->running = RT_FALSE;

    rt_kprintf("[AudioProcess] Processing stopped\n");
    return RT_EOK;
}

/**
 * @brief Audio processing thread
 */
static void audio_process_thread_entry(void *parameter)
{
    audio_process_ctx_t *ctx = &g_audio_ctx;
    audio_frame_t frame;

    rt_kprintf("[AudioProcess] Processing thread started\n");

    while (ctx->running)
    {
        /* Read frame from I2S driver */
        if (inmp441_read_frame(&frame, RT_WAITING_FOREVER) != RT_EOK)
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

        /* VAD - Enhanced Voice Activity Detection */
        rt_bool_t speech_detected = vad_detect_speech_enhanced(&frame);

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
                    ctx->vad_hangover_count = VAD_HANGOVER_FRAMES;  /* Initialize hangover */

                    rt_kprintf("[AudioProcess] Speech detected - Recording started\n");

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
                        rt_kprintf("[AudioProcess] Recording buffer full\n");
                        /* Buffer full - finish recording */
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
                        /* Hangover expired - finish recording */
                        ctx->recording.end_time = rt_tick_get();

                        uint32_t duration_ms = (ctx->recording.end_time - ctx->recording.start_time) * 1000 / RT_TICK_PER_SECOND;

                        /* Check minimum recording duration */
                        if (duration_ms < VAD_MIN_RECORD_MS)
                        {
                            rt_kprintf("[AudioProcess] Recording too short (%d ms), discarding\n", duration_ms);
                            ctx->state = AUDIO_STATE_IDLE;
                            ctx->recording.size = 0;
                            break;
                        }

                        ctx->stats.total_duration_ms += duration_ms;
                        ctx->stats.speech_detected++;

                        rt_kprintf("[AudioProcess] Recording finished - Duration: %d ms, Samples: %d\n",
                                  duration_ms, ctx->recording.size);

                        /* Call user callback */
                        if (ctx->callback != RT_NULL)
                        {
                            ctx->callback(&ctx->recording);
                        }

                        /* Reset to idle */
                        ctx->state = AUDIO_STATE_IDLE;
                        ctx->recording.size = 0;
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

    rt_kprintf("[AudioProcess] Processing thread exited\n");
}

/**
 * @brief Voice Activity Detection
 */
static rt_bool_t vad_detect_speech(audio_frame_t *frame)
{
    uint32_t energy = audio_calculate_energy(frame);
    return (energy > VAD_THRESHOLD);
}

/* ============== Enhanced VAD Implementation ============== */

/**
 * @brief Initialize VAD context
 */
static void vad_init(void)
{
    vad_context_t *vad = &g_vad_ctx;

    rt_memset(vad, 0, sizeof(vad_context_t));
    vad->noise_floor = VAD_ENERGY_THRESHOLD_INIT;
    vad->energy_threshold = VAD_ENERGY_THRESHOLD_INIT;
    vad->smoothed_energy = 0;
    vad->calibrated = RT_FALSE;
    vad->calibration_count = 0;

    rt_kprintf("[VAD] Enhanced VAD initialized (adaptive threshold enabled)\n");
}

/**
 * @brief Calculate Zero Crossing Rate (过零率)
 * @note ZCR helps distinguish speech from noise:
 *       - Speech: moderate ZCR (voiced sounds have low ZCR, unvoiced have high)
 *       - Noise: typically high and random ZCR
 *       - Silence: very low ZCR
 */
static uint32_t vad_calculate_zcr(audio_frame_t *frame)
{
    if (frame == RT_NULL || frame->buffer == RT_NULL || frame->size < 2)
        return 0;

    uint32_t zcr = 0;
    int32_t prev_sign = (frame->buffer[0] >= 0) ? 1 : -1;

    for (uint32_t i = 1; i < frame->size; i++)
    {
        int32_t curr_sign = (frame->buffer[i] >= 0) ? 1 : -1;
        if (curr_sign != prev_sign)
        {
            zcr++;
        }
        prev_sign = curr_sign;
    }

    return zcr;
}

/**
 * @brief Update noise floor estimate (自适应噪声底部)
 * @note Only update when no speech detected, uses slow adaptation
 */
static void vad_update_noise_floor(float energy)
{
    vad_context_t *vad = &g_vad_ctx;

#if VAD_ADAPTIVE_ENABLED
    /* 校准阶段：快速收集噪声样本 */
    if (!vad->calibrated)
    {
        vad->calibration_count++;

        /* 使用较快的更新速度进行初始校准 */
        if (vad->calibration_count == 1)
        {
            vad->noise_floor = energy;
        }
        else
        {
            vad->noise_floor = vad->noise_floor * 0.9f + energy * 0.1f;
        }

        if (vad->calibration_count >= VAD_CALIBRATION_FRAMES)
        {
            vad->calibrated = RT_TRUE;
            vad->energy_threshold = vad->noise_floor * VAD_THRESHOLD_RATIO;

            rt_kprintf("[VAD] Calibration complete: noise_floor=%.0f, threshold=%.0f\n",
                      vad->noise_floor, vad->energy_threshold);
        }
        else if (vad->calibration_count % 10 == 0)
        {
            /* 每10帧打印一次校准进度 */
            rt_kprintf("[VAD] Calibrating %d/%d, noise_floor=%.0f\n",
                      vad->calibration_count, VAD_CALIBRATION_FRAMES, vad->noise_floor);
        }
        return;
    }

    /* 运行阶段：缓慢更新噪声底部 */
    /* 只有当能量低于当前阈值时才更新（避免语音时错误更新） */
    if (energy < vad->energy_threshold * 0.5f)
    {
        vad->noise_floor = vad->noise_floor * (1.0f - VAD_NOISE_FLOOR_ALPHA)
                         + energy * VAD_NOISE_FLOOR_ALPHA;

        /* 更新阈值 */
        vad->energy_threshold = vad->noise_floor * VAD_THRESHOLD_RATIO;
    }
#endif
}

/**
 * @brief Enhanced Voice Activity Detection (增强版VAD)
 * @note Combines energy detection, ZCR, and adaptive threshold
 */
static rt_bool_t vad_detect_speech_enhanced(audio_frame_t *frame)
{
    vad_context_t *vad = &g_vad_ctx;

    /* 计算当前帧能量 */
    float energy = (float)audio_calculate_energy(frame);

    /* 能量平滑 (减少抖动) */
    vad->smoothed_energy = vad->smoothed_energy * (1.0f - VAD_ENERGY_SMOOTH_ALPHA)
                         + energy * VAD_ENERGY_SMOOTH_ALPHA;

    /* 计算过零率 */
    uint32_t zcr = vad_calculate_zcr(frame);
    vad->last_zcr = zcr;
    vad->last_energy = vad->smoothed_energy;

    /* 校准阶段：收集噪声样本，不检测语音 */
    if (!vad->calibrated)
    {
        vad_update_noise_floor(energy);
        return RT_FALSE;
    }

    /* ============ 综合判断 ============ */

    rt_bool_t energy_ok = RT_FALSE;
    rt_bool_t zcr_ok = RT_FALSE;

    /* 条件1: 能量超过自适应阈值 */
    if (vad->smoothed_energy > vad->energy_threshold)
    {
        energy_ok = RT_TRUE;
    }

    /* 条件2: 过零率在合理范围内 (排除纯噪声) */
    if (zcr >= VAD_ZCR_MIN && zcr <= VAD_ZCR_MAX)
    {
        zcr_ok = RT_TRUE;
    }

    /* 综合判断：能量OK且过零率OK才认为是语音 */
    rt_bool_t is_speech = (energy_ok && zcr_ok);

    /* 连续帧计数 (防抖) */
    if (is_speech)
    {
        vad->speech_frame_count++;
        vad->silence_frame_count = 0;
    }
    else
    {
        vad->silence_frame_count++;
        vad->speech_frame_count = 0;

        /* 静音时更新噪声底部 */
        if (vad->silence_frame_count > 10)
        {
            vad_update_noise_floor(energy);
        }
    }

    /* 需要连续检测到多帧语音才确认 */
    if (vad->speech_frame_count >= VAD_MIN_SPEECH_FRAMES)
    {
        return RT_TRUE;
    }

    /* 每50帧打印一次调试信息 */
    static uint32_t debug_counter = 0;
    if (++debug_counter >= 50)
    {
        debug_counter = 0;
        rt_kprintf("[VAD] E=%d T=%d ZCR=%d | energy_ok=%d zcr_ok=%d\n",
                  (int)vad->smoothed_energy, (int)vad->energy_threshold, zcr,
                  energy_ok, zcr_ok);
    }

    return RT_FALSE;
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

    static int32_t prev_sample = 0;
    static int32_t prev_output = 0;

    /* Simple high-pass filter: y[n] = x[n] - x[n-1] + 0.95 * y[n-1] */
    const float alpha = 0.95f;

    for (uint32_t i = 0; i < frame->size; i++)
    {
        int32_t current = frame->buffer[i];
        int32_t output = current - prev_sample + (int32_t)(alpha * prev_output);

        prev_sample = current;
        prev_output = output;

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

/**
 * @brief Save audio to file (WAV format)
 */
rt_err_t audio_save_to_file(audio_recording_t *recording, const char *filename)
{
#ifdef RT_USING_DFS
    int fd;

    if (recording == RT_NULL || filename == RT_NULL)
        return -RT_EINVAL;

    /* Open file for writing */
    fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
    {
        rt_kprintf("[AudioProcess] Failed to open file: %s\n", filename);
        return -RT_ERROR;
    }

    /* Convert 32-bit to 16-bit */
    int16_t *pcm16 = rt_malloc(recording->size * sizeof(int16_t));
    if (pcm16 == RT_NULL)
    {
        close(fd);
        return -RT_ENOMEM;
    }

    audio_convert_32to16(recording->data, pcm16, recording->size);

    /* Write WAV header */
    struct {
        /* RIFF Chunk */
        char riff[4];           /* "RIFF" */
        uint32_t file_size;     /* File size - 8 */
        char wave[4];           /* "WAVE" */

        /* Format Chunk */
        char fmt[4];            /* "fmt " */
        uint32_t fmt_size;      /* Format chunk size (16) */
        uint16_t audio_format;  /* PCM = 1 */
        uint16_t num_channels;  /* Mono = 1 */
        uint32_t sample_rate;   /* Sample rate */
        uint32_t byte_rate;     /* Byte rate */
        uint16_t block_align;   /* Block align */
        uint16_t bits_per_sample; /* Bits per sample */

        /* Data Chunk */
        char data[4];           /* "data" */
        uint32_t data_size;     /* Data size */
    } wav_header;

    uint32_t data_size = recording->size * sizeof(int16_t);

    rt_memcpy(wav_header.riff, "RIFF", 4);
    wav_header.file_size = data_size + 36;
    rt_memcpy(wav_header.wave, "WAVE", 4);
    rt_memcpy(wav_header.fmt, "fmt ", 4);
    wav_header.fmt_size = 16;
    wav_header.audio_format = 1;
    wav_header.num_channels = 1;
    wav_header.sample_rate = recording->sample_rate;
    wav_header.byte_rate = recording->sample_rate * 2;
    wav_header.block_align = 2;
    wav_header.bits_per_sample = 16;
    rt_memcpy(wav_header.data, "data", 4);
    wav_header.data_size = data_size;

    write(fd, &wav_header, sizeof(wav_header));
    write(fd, pcm16, data_size);

    close(fd);
    rt_free(pcm16);

    rt_kprintf("[AudioProcess] Audio saved to: %s\n", filename);
    return RT_EOK;
#else
    rt_kprintf("[AudioProcess] DFS not enabled\n");
    return -RT_ENOSYS;
#endif
}
