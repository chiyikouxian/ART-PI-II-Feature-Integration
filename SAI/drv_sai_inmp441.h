/*
 * Copyright (c) 2006-2025, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Description: SAI2 Hardware I2S Driver for INMP441 Microphone
 *
 * Hardware Connection (ART-PI II using SAI2_Block_B):
 * INMP441       STM32H7R7 (ART-PI II P1 Header)
 * --------      --------------------------------
 * SCK    <-->   PA2  (Pin 12, SAI2_SCK_B)  - Bit Clock (AF8)
 * WS     <-->   PC0  (Pin 33, SAI2_FS_B)   - Word Select / Frame Sync (AF8)
 * SD     <-->   PE3  (Pin 38, SAI2_SD_B)   - Serial Data Input (AF10)  *** CHANGED from PE7 ***
 * L/R    <-->   GND                        - Left Channel
 * VDD    <-->   +3.3V (Pin 1)
 * GND    <-->   GND (Pin 39/40 area)
 *
 * Note: PE7 (Pin 40) is PCM-OUT (output), PE3 (Pin 38) is PCM-IN (input)!
 * Note: MCLK (PE14) is NOT needed for INMP441
 */

#ifndef __DRV_SAI_INMP441_H__
#define __DRV_SAI_INMP441_H__

#include <rtthread.h>
#include <rtdevice.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== Configuration ==================== */

/* Audio Parameters */
#define INMP441_SAMPLE_RATE         16000       /* 16kHz for speech recognition */
#define INMP441_BIT_WIDTH           24          /* INMP441 outputs 24-bit data */
#define INMP441_CHANNEL_NUM         1           /* Mono (Left channel only) */

/* Buffer Configuration */
#define SAI_DMA_BUFFER_SIZE         1024        /* DMA buffer size in samples */
#define AUDIO_BUFFER_COUNT          4           /* Number of audio frame buffers */
#define AUDIO_FRAME_SIZE            512         /* Frame size in samples */

/* SAI2 Pin Definitions (AF8/AF10) */
#define SAI2_SCK_PIN                GPIO_PIN_2  /* PA2 - SAI2_SCK_B (AF8) */
#define SAI2_SCK_PORT               GPIOA
#define SAI2_SCK_AF                 GPIO_AF8_SAI2

#define SAI2_FS_PIN                 GPIO_PIN_0  /* PC0 - SAI2_FS_B (AF8) */
#define SAI2_FS_PORT                GPIOC
#define SAI2_FS_AF                  GPIO_AF8_SAI2

#define SAI2_SD_PIN                 GPIO_PIN_3  /* PE3 - SAI2_SD_B (AF10) - PCM-IN */
#define SAI2_SD_PORT                GPIOE
#define SAI2_SD_AF                  GPIO_AF10_SAI2

/* ==================== Data Structures ==================== */

/**
 * @brief Audio frame structure
 */
typedef struct {
    int32_t *buffer;                /* Audio data buffer (PCM) */
    uint32_t size;                  /* Buffer size in samples */
    uint32_t sample_rate;           /* Sample rate */
    uint8_t channels;               /* Number of channels */
    uint8_t bit_width;              /* Bit width (16/24/32) */
    rt_tick_t timestamp;            /* Timestamp */
} audio_frame_t;

/**
 * @brief INMP441 device structure
 */
typedef struct {
    rt_sem_t buffer_sem;            /* Buffer semaphore */
    rt_mutex_t lock;                /* Device lock */

    /* Audio Frame Buffers */
    audio_frame_t frames[AUDIO_BUFFER_COUNT];
    volatile uint32_t write_idx;    /* Write index */
    volatile uint32_t read_idx;     /* Read index */
    volatile uint32_t frame_count;  /* Available frame count */

    /* Statistics */
    uint32_t total_frames;          /* Total frames captured */
    uint32_t overrun_count;         /* Buffer overrun count */
    uint32_t dma_errors;            /* DMA error count */

    rt_bool_t is_initialized;       /* Initialization state */
    rt_bool_t is_running;           /* Running state */
} inmp441_device_t;

/* ==================== Function Prototypes ==================== */

/**
 * @brief Initialize INMP441 SAI2 driver
 * @return RT_EOK on success, error code otherwise
 */
rt_err_t inmp441_init(void);

/**
 * @brief Deinitialize INMP441 SAI2 driver
 * @return RT_EOK on success, error code otherwise
 */
rt_err_t inmp441_deinit(void);

/**
 * @brief Start audio capture
 * @return RT_EOK on success, error code otherwise
 */
rt_err_t inmp441_start(void);

/**
 * @brief Stop audio capture
 * @return RT_EOK on success, error code otherwise
 */
rt_err_t inmp441_stop(void);

/**
 * @brief Read audio frame (blocking)
 * @param frame Pointer to audio frame structure (buffer will be allocated)
 * @param timeout Timeout in ticks (RT_WAITING_FOREVER for blocking)
 * @return RT_EOK on success, error code otherwise
 * @note Caller must free frame->buffer after use
 */
rt_err_t inmp441_read_frame(audio_frame_t *frame, rt_int32_t timeout);

/**
 * @brief Get device statistics
 * @param total_frames Total frames captured
 * @param overrun_count Buffer overrun count
 */
void inmp441_get_stats(uint32_t *total_frames, uint32_t *overrun_count);

/**
 * @brief Reset device statistics
 */
void inmp441_reset_stats(void);

/**
 * @brief Check if device is running
 * @return RT_TRUE if running, RT_FALSE otherwise
 */
rt_bool_t inmp441_is_running(void);

/**
 * @brief Get device handle (for advanced operations)
 * @return Pointer to device structure
 */
inmp441_device_t *inmp441_get_device(void);

/**
 * @brief Direct SAI register debug - diagnose hardware issues
 * @note Temporarily disables DMA to read SAI data register directly
 */
void inmp441_debug_direct_read(void);

#ifdef __cplusplus
}
#endif

#endif /* __DRV_SAI_INMP441_H__ */
