/*
 * Copyright (c) 2006-2025, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Description: Audio Capture Adapter for INMP441 SAI Microphone
 *              Implements audio_capture.h interface using drv_sai_inmp441 driver.
 *              Replaces MAX4466 ADC capture with INMP441 I2S/SAI capture.
 *
 * Audio Format Conversion:
 *   INMP441 outputs 24-bit I2S data (left-aligned in 32-bit word)
 *   SAI driver delivers data as int32_t (already right-shifted by 8)
 *   This adapter converts to 16-bit PCM (>>8 again) for Xunfei API
 *   Final format: 16kHz, 16-bit, mono, little-endian PCM
 */

#include <rtthread.h>
#include "audio_capture.h"
#include "drv_sai_inmp441.h"

#define DBG_TAG "audio.inmp441"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* Audio capture state */
static audio_capture_state_t g_capture_state = AUDIO_CAPTURE_IDLE;
static audio_capture_callback g_capture_callback = RT_NULL;

/**
 * @brief Initialize audio capture (INMP441 SAI driver)
 */
int audio_capture_init(void)
{
    rt_err_t ret;

    LOG_I("Initializing INMP441 audio capture...");

    ret = inmp441_init();
    if (ret != RT_EOK)
    {
        LOG_E("INMP441 init failed: %d", ret);
        return ret;
    }

    g_capture_state = AUDIO_CAPTURE_STOPPED;
    LOG_I("INMP441 audio capture initialized");

    return RT_EOK;
}

/**
 * @brief Start audio capture
 */
int audio_capture_start(void)
{
    rt_err_t ret;

    if (g_capture_state == AUDIO_CAPTURE_RECORDING)
    {
        LOG_W("Already recording");
        return RT_EOK;
    }

    ret = inmp441_start();
    if (ret != RT_EOK)
    {
        LOG_E("INMP441 start failed: %d", ret);
        return ret;
    }

    g_capture_state = AUDIO_CAPTURE_RECORDING;
    LOG_D("Audio capture started (INMP441 SAI)");

    return RT_EOK;
}

/**
 * @brief Stop audio capture
 */
int audio_capture_stop(void)
{
    if (g_capture_state != AUDIO_CAPTURE_RECORDING)
    {
        return RT_EOK;
    }

    inmp441_stop();
    g_capture_state = AUDIO_CAPTURE_STOPPED;
    LOG_D("Audio capture stopped");

    return RT_EOK;
}

/**
 * @brief Set audio capture callback (not used in INMP441 adapter)
 */
int audio_capture_set_callback(audio_capture_callback callback)
{
    g_capture_callback = callback;
    return RT_EOK;
}

/**
 * @brief Get audio capture state
 */
audio_capture_state_t audio_capture_get_state(void)
{
    return g_capture_state;
}

/**
 * @brief Read audio data from INMP441 and convert to 16-bit PCM
 *
 * This function reads audio frames from the INMP441 SAI driver,
 * converts the 24-bit data to 16-bit PCM, and fills the output buffer.
 *
 * Data flow:
 *   INMP441 (24-bit I2S) → SAI DMA → int32_t (>>8 in driver) → int16_t (>>8 here) → uint8_t buffer
 *
 * @param buffer  Output buffer for 16-bit PCM data (little-endian bytes)
 * @param size    Buffer size in bytes
 * @param timeout Timeout in milliseconds
 * @return Number of bytes read, or negative error code
 */
int audio_capture_read(uint8_t *buffer, uint32_t size, uint32_t timeout)
{
    uint32_t bytes_read = 0;
    rt_tick_t start_tick = rt_tick_get();
    rt_tick_t timeout_ticks = rt_tick_from_millisecond(timeout);

    if (buffer == RT_NULL || size == 0)
    {
        return -RT_EINVAL;
    }

    if (g_capture_state != AUDIO_CAPTURE_RECORDING)
    {
        LOG_E("Not recording");
        return -RT_ERROR;
    }

    while (bytes_read < size)
    {
        audio_frame_t frame;
        rt_tick_t elapsed = rt_tick_get() - start_tick;
        rt_int32_t remaining_ticks;

        /* Check timeout */
        if (elapsed >= timeout_ticks)
        {
            break;
        }

        remaining_ticks = timeout_ticks - elapsed;

        /* Read one frame from INMP441 SAI driver */
        if (inmp441_read_frame(&frame, remaining_ticks) != RT_EOK)
        {
            break;
        }

        /* Convert 32-bit (24-bit effective) samples to 16-bit PCM */
        for (uint32_t i = 0; i < frame.size && bytes_read + 2 <= size; i++)
        {
            /*
             * SAI driver already right-shifted by 8 (24-bit → int32_t range)
             * Now shift by 8 more to get 16-bit range
             * Total: 24-bit → 16-bit = >>8 in driver + >>8 here = >>16 from raw
             */
            int16_t pcm16 = (int16_t)(frame.buffer[i] >> 8);

            /* Store as little-endian bytes */
            buffer[bytes_read]     = (uint8_t)(pcm16 & 0xFF);
            buffer[bytes_read + 1] = (uint8_t)((pcm16 >> 8) & 0xFF);
            bytes_read += 2;
        }

        /* Free the frame buffer allocated by the driver */
        if (frame.buffer != RT_NULL)
        {
            rt_free(frame.buffer);
        }
    }

    return (int)bytes_read;
}
