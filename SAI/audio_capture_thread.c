/*
 * Copyright (c) 2006-2025, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Description: Audio Capture Thread - Main Application Logic
 *              Updated for SAI2 Hardware I2S Driver
 */

#include <rtthread.h>
#include <rtdevice.h>
/* Use SAI2 hardware I2S driver */
#include "drv_sai_inmp441.h"
#include "audio_process.h"
#include "stm32h7rsxx_hal.h"
#include <math.h>

#define AUDIO_CAPTURE_THREAD_STACK_SIZE     2048
#define AUDIO_CAPTURE_THREAD_PRIORITY       15

/* Global control flags */
static rt_bool_t g_audio_system_initialized = RT_FALSE;

/* ==================== Helper Functions ==================== */

/**
 * @brief Calculate RMS (Root Mean Square) of audio samples
 * @note Data from SAI driver is already shifted (24-bit value in int32_t)
 *       24-bit range: -8388608 to +8388607
 */
static float calculate_rms(int32_t *samples, uint32_t count)
{
    if (count == 0 || samples == RT_NULL)
        return 0.0f;

    double sum_squares = 0.0;
    for (uint32_t i = 0; i < count; i++)
    {
        double value = (double)samples[i];
        sum_squares += value * value;
    }

    return sqrtf((float)(sum_squares / count));
}

/**
 * @brief Calculate peak value of audio samples
 */
static int32_t calculate_peak(int32_t *samples, uint32_t count)
{
    if (count == 0 || samples == RT_NULL)
        return 0;

    int32_t peak = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        int32_t abs_val = (samples[i] < 0) ? -samples[i] : samples[i];
        if (abs_val > peak)
            peak = abs_val;
    }

    return peak;
}

/**
 * @brief Print audio level bar (matching ESP32 version)
 * @note Bar width is 50 characters, matching ESP32 BAR_WIDTH
 * @note RT-Thread rt_kprintf doesn't support %f, use integer
 */
#define BAR_WIDTH 50

static void print_level_bar(float level, float max_level)
{
    int bar_length = (int)((level / max_level) * BAR_WIDTH);
    if (bar_length > BAR_WIDTH) bar_length = BAR_WIDTH;
    if (bar_length < 0) bar_length = 0;

    char bar[BAR_WIDTH + 1];
    rt_memset(bar, '#', bar_length);
    rt_memset(bar + bar_length, ' ', BAR_WIDTH - bar_length);
    bar[BAR_WIDTH] = '\0';

    /* RT-Thread rt_kprintf doesn't support %f, cast to int */
    rt_kprintf("\r[%s] %d    ", bar, (int32_t)level);
}

/* ==================== Callbacks ==================== */

/**
 * @brief Speech data callback - called when speech segment is captured
 */
static void speech_data_callback(audio_recording_t *recording)
{
    rt_kprintf("\n=== Speech Segment Captured ===\n");
    rt_kprintf("  Samples: %d\n", recording->size);
    rt_kprintf("  Duration: %d ms\n",
               (recording->end_time - recording->start_time) * 1000 / RT_TICK_PER_SECOND);
    rt_kprintf("  Sample Rate: %d Hz\n", recording->sample_rate);
    rt_kprintf("================================\n\n");

#ifdef RT_USING_DFS
    /* Save to file with timestamp */
    static uint32_t file_count = 0;
    char filename[64];
    rt_snprintf(filename, sizeof(filename), "/audio_%03d.wav", file_count++);

    if (audio_save_to_file(recording, filename) == RT_EOK)
    {
        rt_kprintf("[AudioCapture] Saved to: %s\n", filename);
    }
#endif
}

/* ==================== System Functions ==================== */

/**
 * @brief Audio capture system initialization
 */
static rt_err_t audio_capture_system_init(void)
{
    rt_err_t result;

    rt_kprintf("\n");
    rt_kprintf("========================================\n");
    rt_kprintf("  INMP441 Audio Capture System\n");
    rt_kprintf("  STM32H7R7 + RT-Thread + SAI2\n");
    rt_kprintf("========================================\n\n");

    /* Initialize SAI2 I2S driver */
    result = inmp441_init();
    if (result != RT_EOK)
    {
        rt_kprintf("[AudioCapture] Failed to initialize INMP441 driver\n");
        return result;
    }

    /* Initialize audio processing module */
    result = audio_process_init(speech_data_callback);
    if (result != RT_EOK)
    {
        rt_kprintf("[AudioCapture] Failed to initialize audio processing\n");
        inmp441_deinit();
        return result;
    }

    g_audio_system_initialized = RT_TRUE;

    rt_kprintf("[AudioCapture] System initialized successfully\n");
    rt_kprintf("[AudioCapture] Hardware: PA2(SCK), PC0(WS), PE7(SD)\n");
    rt_kprintf("[AudioCapture] Ready to capture audio\n\n");

    return RT_EOK;
}

/**
 * @brief Audio capture system deinitialization
 */
static void audio_capture_system_deinit(void)
{
    if (!g_audio_system_initialized)
        return;

    /* Stop and deinitialize */
    inmp441_stop();
    audio_process_stop();

    audio_process_deinit();
    inmp441_deinit();

    g_audio_system_initialized = RT_FALSE;

    rt_kprintf("[AudioCapture] System deinitialized\n");
}

/**
 * @brief Start audio capture
 */
static rt_err_t audio_capture_start(void)
{
    rt_err_t result;

    if (!g_audio_system_initialized)
    {
        rt_kprintf("[AudioCapture] System not initialized\n");
        return -RT_ERROR;
    }

    /* Start SAI2 I2S capture */
    result = inmp441_start();
    if (result != RT_EOK)
    {
        rt_kprintf("[AudioCapture] Failed to start SAI2 I2S\n");
        return result;
    }

    /* Start audio processing */
    result = audio_process_start();
    if (result != RT_EOK)
    {
        rt_kprintf("[AudioCapture] Failed to start processing\n");
        inmp441_stop();
        return result;
    }

    rt_kprintf("[AudioCapture] Audio capture started\n");
    rt_kprintf("[AudioCapture] Speak into the microphone...\n\n");

    return RT_EOK;
}

/**
 * @brief Stop audio capture
 */
static rt_err_t audio_capture_stop(void)
{
    if (!g_audio_system_initialized)
    {
        rt_kprintf("[AudioCapture] System not initialized\n");
        return -RT_ERROR;
    }

    /* Stop processing first */
    audio_process_stop();
    rt_thread_mdelay(50);

    /* Stop SAI2 capture */
    inmp441_stop();
    rt_thread_mdelay(50);

    rt_kprintf("\n[AudioCapture] Audio capture stopped\n");

    return RT_EOK;
}

/**
 * @brief Print statistics
 */
static void audio_capture_print_stats(void)
{
    if (!g_audio_system_initialized)
    {
        rt_kprintf("[AudioCapture] System not initialized\n");
        return;
    }

    /* Get SAI2 statistics */
    uint32_t total_frames, overrun_count;
    inmp441_get_stats(&total_frames, &overrun_count);

    /* Get processing statistics */
    audio_stats_t audio_stats;
    audio_process_get_stats(&audio_stats);

    rt_kprintf("\n=== Audio Capture Statistics ===\n");
    rt_kprintf("SAI2 I2S Driver:\n");
    rt_kprintf("  Total Frames: %d\n", total_frames);
    rt_kprintf("  Overrun Count: %d\n", overrun_count);
    rt_kprintf("  Running: %s\n", inmp441_is_running() ? "Yes" : "No");
    rt_kprintf("\nAudio Processing:\n");
    rt_kprintf("  Frames Processed: %d\n", audio_stats.frames_processed);
    rt_kprintf("  Speech Segments: %d\n", audio_stats.speech_detected);
    rt_kprintf("  Total Duration: %d ms\n", audio_stats.total_duration_ms);
    rt_kprintf("  Avg Energy: %d\n", (uint32_t)audio_stats.avg_energy);
    rt_kprintf("  Max Energy: %d\n", audio_stats.max_energy);
    rt_kprintf("  State: ");
    switch (audio_process_get_state())
    {
        case AUDIO_STATE_IDLE:
            rt_kprintf("IDLE\n");
            break;
        case AUDIO_STATE_RECORDING:
            rt_kprintf("RECORDING\n");
            break;
        case AUDIO_STATE_PROCESSING:
            rt_kprintf("PROCESSING\n");
            break;
        default:
            rt_kprintf("UNKNOWN\n");
            break;
    }
    rt_kprintf("================================\n\n");
}

/* ==================== MSH Commands ==================== */

/**
 * @brief MSH command: Initialize audio system
 */
static int audio_init(int argc, char **argv)
{
    /* Print memory information before init */
    rt_size_t total, used, max_used;
    rt_memory_info(&total, &used, &max_used);

    rt_kprintf("\n[Memory Info Before Init]\n");
    rt_kprintf("  Total: %d bytes (%d KB)\n", total, total / 1024);
    rt_kprintf("  Used: %d bytes (%d KB)\n", used, used / 1024);
    rt_kprintf("  Free: %d bytes (%d KB)\n", total - used, (total - used) / 1024);
    rt_kprintf("  Max Used: %d bytes (%d KB)\n\n", max_used, max_used / 1024);

    return audio_capture_system_init();
}
MSH_CMD_EXPORT(audio_init, Initialize audio capture system);

/**
 * @brief MSH command: Start audio capture
 */
static int audio_start(int argc, char **argv)
{
    return audio_capture_start();
}
MSH_CMD_EXPORT(audio_start, Start audio capture);

/**
 * @brief MSH command: Stop audio capture
 */
static int audio_stop(int argc, char **argv)
{
    return audio_capture_stop();
}
MSH_CMD_EXPORT(audio_stop, Stop audio capture);

/**
 * @brief MSH command: Print statistics
 */
static int audio_stats(int argc, char **argv)
{
    audio_capture_print_stats();
    return 0;
}
MSH_CMD_EXPORT(audio_stats, Print audio capture statistics);

/**
 * @brief MSH command: Reset statistics
 */
static int audio_reset(int argc, char **argv)
{
    if (!g_audio_system_initialized)
    {
        rt_kprintf("[AudioCapture] System not initialized\n");
        return -1;
    }

    inmp441_reset_stats();
    audio_process_reset_stats();
    rt_kprintf("[AudioCapture] Statistics reset\n");
    return 0;
}
MSH_CMD_EXPORT(audio_reset, Reset audio statistics);

/**
 * @brief MSH command: Deinitialize audio system
 */
static int audio_deinit(int argc, char **argv)
{
    audio_capture_system_deinit();
    return 0;
}
MSH_CMD_EXPORT(audio_deinit, Deinitialize audio capture system);

/**
 * @brief MSH command: Show memory information
 */
static int audio_meminfo(int argc, char **argv)
{
    rt_size_t total, used, max_used;
    rt_memory_info(&total, &used, &max_used);

    rt_kprintf("\n=== System Memory Information ===\n");
    rt_kprintf("Total Heap: %d bytes (%d KB)\n", total, total / 1024);
    rt_kprintf("Used:       %d bytes (%d KB)\n", used, used / 1024);
    rt_kprintf("Free:       %d bytes (%d KB)\n", total - used, (total - used) / 1024);
    rt_kprintf("Max Used:   %d bytes (%d KB)\n", max_used, max_used / 1024);
    rt_kprintf("================================\n\n");

    if (total - used < 200 * 1024)
    {
        rt_kprintf("WARNING: Less than 200KB free memory!\n");
        rt_kprintf("Audio system needs ~200KB for initialization.\n\n");
    }

    return 0;
}
MSH_CMD_EXPORT(audio_meminfo, Show memory information);

/**
 * @brief MSH command: Debug raw audio data
 */
static int audio_debug(int argc, char **argv)
{
    if (!g_audio_system_initialized || !inmp441_is_running())
    {
        rt_kprintf("[AudioCapture] System not running. Use audio_init and audio_start first.\n");
        return -1;
    }

    rt_kprintf("\n=== Raw Audio Data Debug ===\n");

    /* Read one frame and display samples */
    audio_frame_t frame;
    if (inmp441_read_frame(&frame, 1000) == RT_EOK)
    {
        rt_kprintf("Frame size: %d samples\n", frame.size);
        rt_kprintf("First 32 samples (hex):\n");

        int32_t min_val = 0x7FFFFFFF;
        int32_t max_val = (int32_t)0x80000000;
        int64_t sum = 0;
        int non_zero = 0;

        /* Print in rows of 8 for easier reading */
        for (uint32_t i = 0; i < frame.size && i < 32; i++)
        {
            if (i % 8 == 0) rt_kprintf("  ");
            rt_kprintf("%08X ", frame.buffer[i]);
            if (i % 8 == 7) rt_kprintf("\n");
        }

        /* Calculate statistics for all samples */
        for (uint32_t i = 0; i < frame.size; i++)
        {
            int32_t sample = frame.buffer[i];
            if (sample < min_val) min_val = sample;
            if (sample > max_val) max_val = sample;
            sum += sample;
            if (sample != 0) non_zero++;
        }

        rt_kprintf("\nStatistics:\n");
        rt_kprintf("  Min: %d (0x%08X)\n", min_val, min_val);
        rt_kprintf("  Max: %d (0x%08X)\n", max_val, max_val);
        rt_kprintf("  Avg: %d\n", (int32_t)(sum / frame.size));
        rt_kprintf("  Non-zero samples: %d / %d\n", non_zero, frame.size);
        rt_kprintf("  RMS: %.0f\n", calculate_rms(frame.buffer, frame.size));
        rt_kprintf("  Peak: %d\n", calculate_peak(frame.buffer, frame.size));
        rt_kprintf("=============================\n\n");

        rt_free(frame.buffer);
    }
    else
    {
        rt_kprintf("Failed to read frame\n");
    }

    return 0;
}
MSH_CMD_EXPORT(audio_debug, Debug raw audio data);

/**
 * @brief MSH command: Real-time audio level display (matching ESP32 version)
 * @note Uses same scaling and display format as ESP32 Demo
 */
static int audio_level(int argc, char **argv)
{
    int duration = 10;  /* Default 10 seconds */

    if (argc >= 2)
    {
        duration = atoi(argv[1]);
        if (duration <= 0) duration = 10;
        if (duration > 300) duration = 300;  /* Max 5 minutes */
    }

    if (!g_audio_system_initialized)
    {
        rt_kprintf("[AudioCapture] System not initialized, initializing...\n");
        if (audio_capture_system_init() != RT_EOK)
        {
            rt_kprintf("ERROR: Failed to initialize\n");
            return -1;
        }
    }

    if (!inmp441_is_running())
    {
        rt_kprintf("[AudioCapture] Starting audio capture...\n");
        if (audio_capture_start() != RT_EOK)
        {
            rt_kprintf("ERROR: Failed to start capture\n");
            return -1;
        }
    }

    rt_kprintf("\n=== Audio Level Monitor ===\n");
    rt_kprintf("Duration: %d seconds\n", duration);
    rt_kprintf("Hardware: PA2(SCK), PC0(WS), PE7(SD)\n");
    rt_kprintf("Speak into the microphone to see audio levels\n\n");

    /* Dynamic auto-scaling - start small and scale up */
    float max_rms = 100.0f;
    uint32_t start_tick = rt_tick_get();
    uint32_t end_tick = start_tick + duration * RT_TICK_PER_SECOND;
    uint32_t frame_count = 0;
    int detail_counter = 0;

    while (rt_tick_get() < end_tick)
    {
        audio_frame_t frame;
        if (inmp441_read_frame(&frame, 100) == RT_EOK)
        {
            float rms = calculate_rms(frame.buffer, frame.size);
            int32_t peak = calculate_peak(frame.buffer, frame.size);

            /* Dynamic auto-scale based on current RMS */
            if (rms > max_rms)
            {
                max_rms = rms * 1.2f;  /* Add 20% headroom */
            }
            else
            {
                max_rms *= 0.998f;  /* Slow decay */
            }
            if (max_rms < 100.0f)
                max_rms = 100.0f;

            /* Print level bar */
            print_level_bar(rms, max_rms);

            /* Periodically print detailed info (like ESP32) */
            if (++detail_counter >= 20)
            {
                detail_counter = 0;
                rt_kprintf("\n");
                /* RT-Thread rt_kprintf doesn't support %f, cast to int */
                rt_kprintf("[SAI] RMS: %d, Peak: %d, MaxRMS: %d\n",
                           (int32_t)rms, peak, (int32_t)max_rms);
            }

            frame_count++;
            rt_free(frame.buffer);
        }

        rt_thread_mdelay(20);  /* ~50 updates per second */
    }

    rt_kprintf("\n\n=== Level Monitor Complete ===\n");
    rt_kprintf("Frames displayed: %d\n", frame_count);
    rt_kprintf("==============================\n\n");

    return 0;
}
MSH_CMD_EXPORT(audio_level, Display real-time audio level [duration_seconds]);

/**
 * @brief MSH command: Hardware diagnostic for SAI2 pins
 */
static int audio_hw_diag(int argc, char **argv)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    int high_count, low_count;

    rt_kprintf("\n========== SAI2 INMP441 Hardware Diagnostic ==========\n\n");
    rt_kprintf("SAI2 Pin Configuration:\n");
    rt_kprintf("  PA2  -> SCK (SAI2_SCK_B, AF8)  - Bit Clock      [P1 Pin 12]\n");
    rt_kprintf("  PC0  -> WS  (SAI2_FS_B, AF8)   - Word Select    [P1 Pin 33]\n");
    rt_kprintf("  PE7  -> SD  (SAI2_SD_B, AF10)  - Serial Data    [P1 Pin 40, PCM-OUT]\n");
    rt_kprintf("  GND  -> L/R (Left Channel)\n");
    rt_kprintf("  3.3V -> VDD                                     [P1 Pin 1]\n\n");

    /* Enable GPIO clocks */
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();

    /* Stop audio if running */
    rt_bool_t was_running = inmp441_is_running();
    if (was_running)
    {
        rt_kprintf("[Diag] Stopping audio capture for testing...\n\n");
        audio_capture_stop();
        rt_thread_mdelay(100);
    }

    /* ========== Test 1: Check PA2 (SCK) ========== */
    rt_kprintf("--- Test 1: PA2 (SCK) Pin Check ---\n");

    GPIO_InitStruct.Pin = GPIO_PIN_2;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
    rt_thread_mdelay(5);

    high_count = 0; low_count = 0;
    for (int i = 0; i < 100; i++)
    {
        if (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_2) == GPIO_PIN_SET)
            high_count++;
        else
            low_count++;
    }
    rt_kprintf("  PA2 as input (pulldown): HIGH=%d, LOW=%d\n", high_count, low_count);

    /* Manual toggle test */
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    rt_kprintf("  Testing PA2 manual toggle...\n");
    for (int i = 0; i < 5; i++)
    {
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_2, GPIO_PIN_SET);
        rt_thread_mdelay(1);
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_2, GPIO_PIN_RESET);
        rt_thread_mdelay(1);
    }
    rt_kprintf("  PA2 toggle OK (verify with oscilloscope on INMP441 SCK)\n\n");

    /* ========== Test 2: Check PC0 (WS/FS) ========== */
    rt_kprintf("--- Test 2: PC0 (WS) Pin Check ---\n");

    GPIO_InitStruct.Pin = GPIO_PIN_0;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    rt_kprintf("  Testing PC0 manual toggle...\n");
    for (int i = 0; i < 5; i++)
    {
        HAL_GPIO_WritePin(GPIOC, GPIO_PIN_0, GPIO_PIN_SET);
        rt_thread_mdelay(1);
        HAL_GPIO_WritePin(GPIOC, GPIO_PIN_0, GPIO_PIN_RESET);
        rt_thread_mdelay(1);
    }
    rt_kprintf("  PC0 toggle OK (verify with oscilloscope on INMP441 WS)\n\n");

    /* ========== Test 3: Check PE7 (SD/Data) ========== */
    rt_kprintf("--- Test 3: PE7 (SD/Data) Pin Check ---\n");

    GPIO_InitStruct.Pin = GPIO_PIN_7;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);
    rt_thread_mdelay(5);

    high_count = 0; low_count = 0;
    for (int i = 0; i < 100; i++)
    {
        if (HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_7) == GPIO_PIN_SET)
            high_count++;
        else
            low_count++;
    }
    rt_kprintf("  PE7 (no pull): HIGH=%d, LOW=%d\n", high_count, low_count);

    /* Test with pull-up */
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);
    rt_thread_mdelay(5);

    high_count = 0; low_count = 0;
    for (int i = 0; i < 100; i++)
    {
        if (HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_7) == GPIO_PIN_SET)
            high_count++;
        else
            low_count++;
    }
    rt_kprintf("  PE7 (pull-up): HIGH=%d, LOW=%d\n", high_count, low_count);

    /* Test with pull-down */
    GPIO_InitStruct.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);
    rt_thread_mdelay(5);

    high_count = 0; low_count = 0;
    for (int i = 0; i < 100; i++)
    {
        if (HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_7) == GPIO_PIN_SET)
            high_count++;
        else
            low_count++;
    }
    rt_kprintf("  PE7 (pull-down): HIGH=%d, LOW=%d\n\n", high_count, low_count);

    /* ========== Test 4: Manual clock generation and data check ========== */
    rt_kprintf("--- Test 4: Manual Clock Test ---\n");
    rt_kprintf("  Generating 1000 clock pulses on PA2...\n");

    /* Set PA2 as output for SCK */
    GPIO_InitStruct.Pin = GPIO_PIN_2;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /* Set PC0 as output for WS - keep LOW for left channel */
    GPIO_InitStruct.Pin = GPIO_PIN_0;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_0, GPIO_PIN_RESET);

    /* Set PE7 as input for SD */
    GPIO_InitStruct.Pin = GPIO_PIN_7;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

    high_count = 0; low_count = 0;

    /* Generate clock pulses and sample SD */
    for (int i = 0; i < 1000; i++)
    {
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_2, GPIO_PIN_SET);   /* SCK HIGH */
        for (volatile int d = 0; d < 10; d++);  /* ~1us delay */

        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_2, GPIO_PIN_RESET); /* SCK LOW */
        if (HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_7) == GPIO_PIN_SET)
            high_count++;
        else
            low_count++;

        for (volatile int d = 0; d < 10; d++);
    }

    rt_kprintf("  PE7 during clock: HIGH=%d, LOW=%d\n\n", high_count, low_count);

    /* ========== Analysis ========== */
    rt_kprintf("========== Analysis ==========\n");

    if (high_count == 0 && low_count == 1000)
    {
        rt_kprintf("PROBLEM: PE7 stays LOW during clock generation\n\n");
        rt_kprintf("Possible causes:\n");
        rt_kprintf("  1. INMP441 SD not connected to PE7\n");
        rt_kprintf("  2. INMP441 not powered (check VDD = 3.3V)\n");
        rt_kprintf("  3. INMP441 L/R not connected to GND\n");
        rt_kprintf("  4. Broken INMP441 module\n");
        rt_kprintf("  5. Wrong wiring (check PA2->SCK, PC0->WS, PE7->SD)\n\n");
        rt_kprintf("Hardware checks:\n");
        rt_kprintf("  - Multimeter: INMP441 VDD should be 3.3V\n");
        rt_kprintf("  - Oscilloscope: Check INMP441 SD pin for data\n");
        rt_kprintf("  - Verify all 6 connections\n");
    }
    else if (high_count >= 100 && low_count >= 100)
    {
        rt_kprintf("GOOD: PE7 is receiving data from INMP441!\n");
        rt_kprintf("Hardware connection appears correct.\n\n");
        rt_kprintf("Next step: Run audio_init and audio_start\n");
    }
    else if (high_count == 1000 && low_count == 0)
    {
        rt_kprintf("WARNING: PE7 stays HIGH\n");
        rt_kprintf("Possible short to VDD or incorrect wiring\n\n");
    }
    else
    {
        rt_kprintf("PARTIAL: PE7 shows some activity (HIGH=%d, LOW=%d)\n",
                   high_count, low_count);
        rt_kprintf("Connection might be marginal - check power supply\n\n");
    }

    /* ========== Restore SAI2 GPIO configuration ========== */
    rt_kprintf("Restoring SAI2 GPIO configuration...\n");

    /* PA2 - SAI2_SCK_B (AF8) */
    GPIO_InitStruct.Pin = GPIO_PIN_2;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF8_SAI2;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /* PC0 - SAI2_FS_B (AF8) */
    GPIO_InitStruct.Pin = GPIO_PIN_0;
    GPIO_InitStruct.Alternate = GPIO_AF8_SAI2;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    /* PE7 - SAI2_SD_B (AF10) */
    GPIO_InitStruct.Pin = GPIO_PIN_7;
    GPIO_InitStruct.Alternate = GPIO_AF10_SAI2;
    HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

    rt_kprintf("\n========== Diagnostic Complete ==========\n\n");

    /* Restart if was running */
    if (was_running)
    {
        rt_kprintf("Restarting audio capture...\n");
        audio_capture_start();
    }

    return 0;
}
MSH_CMD_EXPORT(audio_hw_diag, Full hardware diagnostic for SAI2 INMP441);

/**
 * @brief MSH command: Check SAI2 register status
 */
static int audio_sai_reg(int argc, char **argv)
{
    rt_kprintf("\n=== SAI2 Register Status ===\n");
    rt_kprintf("SAI2 Base: 0x%08X\n", (uint32_t)SAI2);
    rt_kprintf("SAI2_Block_B Base: 0x%08X\n\n", (uint32_t)SAI2_Block_B);

    rt_kprintf("SAI2_Block_B Registers:\n");
    rt_kprintf("  CR1:  0x%08X\n", SAI2_Block_B->CR1);
    rt_kprintf("  CR2:  0x%08X\n", SAI2_Block_B->CR2);
    rt_kprintf("  FRCR: 0x%08X\n", SAI2_Block_B->FRCR);
    rt_kprintf("  SLOTR:0x%08X\n", SAI2_Block_B->SLOTR);
    rt_kprintf("  IMR:  0x%08X\n", SAI2_Block_B->IMR);
    rt_kprintf("  SR:   0x%08X\n", SAI2_Block_B->SR);
    rt_kprintf("  CLRFR:0x%08X\n", SAI2_Block_B->CLRFR);

    rt_kprintf("\nSAI2_Block_B->CR1 bits:\n");
    rt_kprintf("  SAIEN (SAI enabled):  %d\n", (SAI2_Block_B->CR1 & SAI_xCR1_SAIEN) ? 1 : 0);
    rt_kprintf("  DMAEN (DMA enabled):  %d\n", (SAI2_Block_B->CR1 & SAI_xCR1_DMAEN) ? 1 : 0);
    rt_kprintf("  MODE:  %d (0=MasterTX, 1=MasterRX, 2=SlaveTX, 3=SlaveRX)\n",
               (SAI2_Block_B->CR1 >> 0) & 0x03);

    rt_kprintf("\nSAI2_Block_B->SR bits:\n");
    rt_kprintf("  OVRUDR (Overrun):     %d\n", (SAI2_Block_B->SR & SAI_xSR_OVRUDR) ? 1 : 0);
    rt_kprintf("  FREQ (FIFO request):  %d\n", (SAI2_Block_B->SR & SAI_xSR_FREQ) ? 1 : 0);
    rt_kprintf("  FLVL (FIFO level):    %d\n", (SAI2_Block_B->SR >> 16) & 0x07);

    rt_kprintf("============================\n\n");
    return 0;
}
MSH_CMD_EXPORT(audio_sai_reg, Show SAI2 register status);

/**
 * @brief MSH command: Direct SAI debug - bypass DMA and read registers
 */
static int audio_sai_debug(int argc, char **argv)
{
    if (!g_audio_system_initialized)
    {
        rt_kprintf("[AudioCapture] System not initialized, initializing...\n");
        if (audio_capture_system_init() != RT_EOK)
        {
            rt_kprintf("ERROR: Failed to initialize\n");
            return -1;
        }
    }

    if (!inmp441_is_running())
    {
        rt_kprintf("[AudioCapture] Starting audio capture...\n");
        if (audio_capture_start() != RT_EOK)
        {
            rt_kprintf("ERROR: Failed to start capture\n");
            return -1;
        }
    }

    /* Call the direct read debug function */
    inmp441_debug_direct_read();

    return 0;
}
MSH_CMD_EXPORT(audio_sai_debug, Direct SAI debug - bypass DMA to diagnose hardware);

/**
 * @brief MSH command: Quick test - init, start, show level for 5 seconds
 */
static int audio_test(int argc, char **argv)
{
    rt_kprintf("\n=== Quick Audio Test ===\n");
    rt_kprintf("This will initialize, start, and display audio level for 5 seconds\n\n");

    /* Initialize if needed */
    if (!g_audio_system_initialized)
    {
        rt_kprintf("Step 1: Initializing...\n");
        if (audio_capture_system_init() != RT_EOK)
        {
            rt_kprintf("ERROR: Initialization failed\n");
            return -1;
        }
    }
    else
    {
        rt_kprintf("Step 1: Already initialized\n");
    }

    /* Start if needed */
    if (!inmp441_is_running())
    {
        rt_kprintf("Step 2: Starting capture...\n");
        if (audio_capture_start() != RT_EOK)
        {
            rt_kprintf("ERROR: Start failed\n");
            return -1;
        }
    }
    else
    {
        rt_kprintf("Step 2: Already running\n");
    }

    /* Wait a bit for stabilization */
    rt_thread_mdelay(100);

    /* Show level for 5 seconds */
    rt_kprintf("Step 3: Displaying audio level (5 seconds)...\n\n");

    /* Dynamic auto-scaling - start small and scale up */
    float max_rms = 100.0f;
    uint32_t start = rt_tick_get();
    uint32_t end = start + 5 * RT_TICK_PER_SECOND;
    int detail_counter = 0;

    while (rt_tick_get() < end)
    {
        audio_frame_t frame;
        if (inmp441_read_frame(&frame, 100) == RT_EOK)
        {
            float rms = calculate_rms(frame.buffer, frame.size);
            int32_t peak = calculate_peak(frame.buffer, frame.size);

            /* Dynamic auto-scale based on current RMS */
            if (rms > max_rms)
            {
                max_rms = rms * 1.2f;
            }
            else
            {
                max_rms *= 0.998f;
            }
            if (max_rms < 100.0f)
                max_rms = 100.0f;

            print_level_bar(rms, max_rms);

            /* Periodically print detailed info */
            if (++detail_counter >= 20)
            {
                detail_counter = 0;
                rt_kprintf("\n");
                /* RT-Thread rt_kprintf doesn't support %f, cast to int */
                rt_kprintf("[SAI] RMS: %d, Peak: %d, MaxRMS: %d\n",
                           (int32_t)rms, peak, (int32_t)max_rms);
            }

            rt_free(frame.buffer);
        }
        rt_thread_mdelay(30);
    }

    rt_kprintf("\n\n=== Test Complete ===\n");
    rt_kprintf("If you saw the level bar moving, audio is working!\n");
    rt_kprintf("If bar stays empty/full, check hardware connections.\n\n");

    return 0;
}
MSH_CMD_EXPORT(audio_test, Quick audio test - init start and display level);

/* ==================== Auto-start Audio Monitor Thread ==================== */

static rt_thread_t audio_monitor_thread = RT_NULL;
static rt_bool_t audio_monitor_running = RT_FALSE;

/**
 * @brief Audio monitor thread entry - continuously displays audio level
 * @note Uses dynamic auto-scaling based on actual peak values
 */
static void audio_monitor_entry(void *parameter)
{
    int detail_counter = 0;
    uint32_t total_frames = 0;
    /* 24-bit audio max value: 8388608 (2^23) */
    const int32_t AUDIO_MAX_VALUE = 8388608;

    rt_kprintf("\n");
    rt_kprintf("============================================\n");
    rt_kprintf("  INMP441 Audio Level Monitor (Auto-start)\n");
    rt_kprintf("  Hardware: PA2(SCK), PC0(WS), PE7(SD)\n");
    rt_kprintf("  Use 'audio_monitor_stop' to stop\n");
    rt_kprintf("============================================\n\n");

    while (audio_monitor_running)
    {
        audio_frame_t frame;
        if (inmp441_read_frame(&frame, 200) == RT_EOK)
        {
            float rms = calculate_rms(frame.buffer, frame.size);
            int32_t peak = calculate_peak(frame.buffer, frame.size);

            /* Calculate percentage based on 24-bit max value */
            int32_t percent = (int32_t)((int64_t)peak * 100 / AUDIO_MAX_VALUE);
            if (percent > 100) percent = 100;

            /* Print level bar with percentage */
            int bar_length = percent / 2;  /* 50 chars = 100% */
            if (bar_length > BAR_WIDTH) bar_length = BAR_WIDTH;
            if (bar_length < 0) bar_length = 0;

            char bar[BAR_WIDTH + 1];
            rt_memset(bar, '#', bar_length);
            rt_memset(bar + bar_length, ' ', BAR_WIDTH - bar_length);
            bar[BAR_WIDTH] = '\0';

            rt_kprintf("\r[%s] %3d%%    ", bar, percent);

            /* Periodically print detailed info (every ~1 second) */
            if (++detail_counter >= 25)
            {
                detail_counter = 0;
                rt_kprintf("\n[SAI] RMS: %d, Peak: %d (%d%%), Frames: %d\n",
                           (int32_t)rms, peak, percent, total_frames);
            }

            total_frames++;
            rt_free(frame.buffer);
        }

        rt_thread_mdelay(40);  /* ~25 updates per second */
    }

    rt_kprintf("\n\n[AudioMonitor] Stopped. Total frames: %d\n", total_frames);
}

/**
 * @brief Start the audio monitor (called automatically on boot)
 */
static int audio_monitor_start_func(void)
{
    if (audio_monitor_running)
    {
        rt_kprintf("[AudioMonitor] Already running\n");
        return 0;
    }

    /* Initialize if needed */
    if (!g_audio_system_initialized)
    {
        if (audio_capture_system_init() != RT_EOK)
        {
            rt_kprintf("[AudioMonitor] Init failed\n");
            return -1;
        }
    }

    /* Start capture if needed */
    if (!inmp441_is_running())
    {
        if (audio_capture_start() != RT_EOK)
        {
            rt_kprintf("[AudioMonitor] Start capture failed\n");
            return -1;
        }
    }

    /* Create and start monitor thread */
    audio_monitor_running = RT_TRUE;
    audio_monitor_thread = rt_thread_create("aud_mon",
                                            audio_monitor_entry,
                                            RT_NULL,
                                            2048,
                                            20,
                                            10);
    if (audio_monitor_thread != RT_NULL)
    {
        rt_thread_startup(audio_monitor_thread);
        return 0;
    }
    else
    {
        audio_monitor_running = RT_FALSE;
        rt_kprintf("[AudioMonitor] Failed to create thread\n");
        return -1;
    }
}

/**
 * @brief Stop the audio monitor
 */
static int audio_monitor_stop_func(void)
{
    if (!audio_monitor_running)
    {
        rt_kprintf("[AudioMonitor] Not running\n");
        return 0;
    }

    audio_monitor_running = RT_FALSE;
    rt_thread_mdelay(200);  /* Wait for thread to exit */

    rt_kprintf("[AudioMonitor] Stopped\n");
    return 0;
}

/* MSH commands for manual control */
static int audio_monitor_start_cmd(int argc, char **argv)
{
    return audio_monitor_start_func();
}
MSH_CMD_EXPORT_ALIAS(audio_monitor_start_cmd, audio_monitor, Start continuous audio level monitor);

static int audio_monitor_stop_cmd(int argc, char **argv)
{
    return audio_monitor_stop_func();
}
MSH_CMD_EXPORT_ALIAS(audio_monitor_stop_cmd, audio_monitor_stop, Stop audio level monitor);

/**
 * @brief Auto-initialization function - starts audio monitor after WiFi
 */
static int audio_auto_init(void)
{
    /* Wait for WiFi connection to complete (WiFi takes ~10s) */
    rt_thread_mdelay(12000);

    rt_kprintf("\n[AutoInit] Starting audio capture system...\n");

    /* Auto-start the audio monitor */
    audio_monitor_start_func();

    return 0;
}
/* Enable auto-initialization on boot */
INIT_APP_EXPORT(audio_auto_init);
