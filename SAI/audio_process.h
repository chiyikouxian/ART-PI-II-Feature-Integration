/*
 * Copyright (c) 2006-2025, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Description: Audio Processing Module for Speech Recognition
 */

#ifndef __AUDIO_PROCESS_H__
#define __AUDIO_PROCESS_H__

#include <rtthread.h>
/* Use SAI2 driver instead of SPI-based driver */
#include "drv_sai_inmp441.h"

/* Audio Processing Configuration */
#define AUDIO_PROCESS_STACK_SIZE        2048
#define AUDIO_PROCESS_PRIORITY          10

/* VAD (Voice Activity Detection) Configuration */
#define VAD_THRESHOLD                   1000        /* Energy threshold for speech detection */
#define VAD_HANGOVER_FRAMES             10          /* Frames to keep after speech ends */

/* Audio Processing State */
typedef enum {
    AUDIO_STATE_IDLE = 0,           /* Idle - no speech */
    AUDIO_STATE_DETECTING,          /* Detecting speech */
    AUDIO_STATE_RECORDING,          /* Recording speech */
    AUDIO_STATE_PROCESSING          /* Processing recorded speech */
} audio_state_t;

/* Audio Buffer for Recording */
typedef struct {
    int32_t *data;                  /* Audio data buffer */
    uint32_t size;                  /* Current size in samples */
    uint32_t capacity;              /* Maximum capacity in samples */
    uint32_t sample_rate;           /* Sample rate */
    rt_tick_t start_time;           /* Recording start time */
    rt_tick_t end_time;             /* Recording end time */
} audio_recording_t;

/* Audio Statistics */
typedef struct {
    uint32_t frames_processed;      /* Total frames processed */
    uint32_t speech_detected;       /* Number of speech segments detected */
    uint32_t total_duration_ms;     /* Total speech duration in ms */
    float avg_energy;               /* Average energy level */
    uint32_t max_energy;            /* Maximum energy level */
} audio_stats_t;

/* Callback function type for speech data */
typedef void (*speech_data_callback_t)(audio_recording_t *recording);

/**
 * @brief Initialize audio processing module
 * @param callback Callback function to handle speech data
 * @return RT_EOK on success, error code otherwise
 */
rt_err_t audio_process_init(speech_data_callback_t callback);

/**
 * @brief Deinitialize audio processing module
 * @return RT_EOK on success, error code otherwise
 */
rt_err_t audio_process_deinit(void);

/**
 * @brief Start audio processing
 * @return RT_EOK on success, error code otherwise
 */
rt_err_t audio_process_start(void);

/**
 * @brief Stop audio processing
 * @return RT_EOK on success, error code otherwise
 */
rt_err_t audio_process_stop(void);

/**
 * @brief Get current audio processing state
 * @return Current state
 */
audio_state_t audio_process_get_state(void);

/**
 * @brief Get audio processing statistics
 * @param stats Pointer to statistics structure
 */
void audio_process_get_stats(audio_stats_t *stats);

/**
 * @brief Reset audio processing statistics
 */
void audio_process_reset_stats(void);

/**
 * @brief Calculate audio frame energy
 * @param frame Audio frame
 * @return Energy value
 */
uint32_t audio_calculate_energy(audio_frame_t *frame);

/**
 * @brief Apply simple noise reduction
 * @param frame Audio frame to process (in-place)
 */
void audio_noise_reduction(audio_frame_t *frame);

/**
 * @brief Convert 32-bit PCM to 16-bit PCM
 * @param input Input buffer (32-bit)
 * @param output Output buffer (16-bit)
 * @param samples Number of samples
 */
void audio_convert_32to16(int32_t *input, int16_t *output, uint32_t samples);

/**
 * @brief Save audio recording to file (optional)
 * @param recording Audio recording structure
 * @param filename Output filename
 * @return RT_EOK on success, error code otherwise
 */
rt_err_t audio_save_to_file(audio_recording_t *recording, const char *filename);

#endif /* __AUDIO_PROCESS_H__ */
