/*
 * Copyright (c) 2006-2025, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Description: SAI1 Hardware I2S Driver for INMP441 Microphone
 *
 * CRITICAL FIX: PE3 is SAI1_SD_B (AF6), NOT SAI2_SD_B!
 *
 * According to ART-PI II schematic:
 * - PE3 (Pin 38, #PCM-IN) = SAI1_SD_B (AF6) - Data Input
 * - PE2 (Pin 21) = SAI1_CK1/MCLK (AF2) - Can be used as clock reference
 *
 * Since SAI1_Block_B doesn't have dedicated SCK/FS pins exposed on header,
 * we need to use SAI1 in SYNCHRONOUS mode with Block_A providing clocks,
 * OR use software-generated clocks.
 *
 * Alternative: Use SAI2 with PE7 for SD if hardware allows rewiring.
 */

#include "drv_sai_inmp441.h"
#include "drv_common.h"
#include <string.h>

/* STM32 HAL Headers */
#include "stm32h7rsxx_hal.h"

/* Debug tag */
#define DBG_TAG "SAI.INMP441"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* ==================== CONFIGURATION OPTIONS ==================== */

/*
 * Option 1: Use SAI1 with PE3 as SD (requires clock from Block_A or external)
 * Option 2: Use SAI2 with PE7 as SD (requires rewiring INMP441 SD to PE7)
 *
 * Currently implementing Option 2 since SAI2 has proper SCK (PA2) and FS (PC0)
 * User needs to rewire: INMP441 SD -> PE7 (Pin 40) instead of PE3 (Pin 38)
 */
#define USE_SAI2_WITH_PE7   1   /* Set to 1 to use SAI2 + PE7 */

/* ==================== Private Variables ==================== */

static inmp441_device_t g_inmp441_dev = {0};

#if USE_SAI2_WITH_PE7
/* SAI2 handles */
static SAI_HandleTypeDef hsai2b = {0};
static DMA_HandleTypeDef hdma_sai2b = {0};

/* Corrected pin for SAI2 SD */
#define SAI2_SD_PIN_CORRECTED   GPIO_PIN_7  /* PE7 - SAI2_SD_B (AF10) */
#define SAI2_SD_PORT_CORRECTED  GPIOE
#define SAI2_SD_AF_CORRECTED    GPIO_AF10_SAI2

#else
/* SAI1 handles - for future implementation */
static SAI_HandleTypeDef hsai1a = {0};  /* Block A for clock generation */
static SAI_HandleTypeDef hsai1b = {0};  /* Block B for data reception */
static DMA_HandleTypeDef hdma_sai1b = {0};
#endif

/* DMA buffer */
static int32_t dma_buffer[SAI_DMA_BUFFER_SIZE * 2] __attribute__((aligned(32)));

/* Debug counter */
static volatile uint32_t debug_print_counter = 0;

/* ==================== GPIO Initialization ==================== */

static void sai_gpio_init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();

#if USE_SAI2_WITH_PE7
    /* PA2 - SAI2_SCK_B (AF8) */
    GPIO_InitStruct.Pin = GPIO_PIN_2;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF8_SAI2;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /* PC0 - SAI2_FS_B (AF8) */
    GPIO_InitStruct.Pin = GPIO_PIN_0;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF8_SAI2;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    /* PE7 - SAI2_SD_B (AF10) - CORRECT pin for SAI2 data input */
    GPIO_InitStruct.Pin = SAI2_SD_PIN_CORRECTED;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStruct.Alternate = SAI2_SD_AF_CORRECTED;
    HAL_GPIO_Init(SAI2_SD_PORT_CORRECTED, &GPIO_InitStruct);

    LOG_I("GPIO: PA2(SCK/AF8), PC0(FS/AF8), PE7(SD/AF10)");
    rt_kprintf("\n");
    rt_kprintf("!!! IMPORTANT: Connect INMP441 SD to PE7 (Pin 40), NOT PE3 !!!\n");
    rt_kprintf("\n");
#else
    /* SAI1 configuration - PE3 is SAI1_SD_B */
    /* PE3 - SAI1_SD_B (AF6) */
    GPIO_InitStruct.Pin = GPIO_PIN_3;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF6_SAI1;
    HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

    LOG_I("GPIO: PE3(SD/AF6) - SAI1_SD_B");
#endif
}

static void sai_gpio_deinit(void)
{
#if USE_SAI2_WITH_PE7
    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_2);
    HAL_GPIO_DeInit(GPIOC, GPIO_PIN_0);
    HAL_GPIO_DeInit(GPIOE, SAI2_SD_PIN_CORRECTED);
#else
    HAL_GPIO_DeInit(GPIOE, GPIO_PIN_3);
#endif
}

/* ==================== SAI Peripheral Configuration ==================== */

#if USE_SAI2_WITH_PE7

static rt_err_t sai_peripheral_init(void)
{
    RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};
    HAL_StatusTypeDef status;

    /* Configure SAI2 clock */
    PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_SAI2;
    PeriphClkInit.Sai2ClockSelection = RCC_SAI2CLKSOURCE_PLL1Q;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
    {
        LOG_E("SAI2 clock config failed");
        return -RT_ERROR;
    }

    __HAL_RCC_SAI2_CLK_ENABLE();

    hsai2b.Instance = SAI2_Block_B;
    __HAL_SAI_DISABLE(&hsai2b);

    /* Master receiver configuration */
    hsai2b.Init.AudioMode = SAI_MODEMASTER_RX;
    hsai2b.Init.Synchro = SAI_ASYNCHRONOUS;
    hsai2b.Init.SynchroExt = SAI_SYNCEXT_DISABLE;
    hsai2b.Init.OutputDrive = SAI_OUTPUTDRIVE_ENABLE;  /* Drive SCK/FS outputs */
    hsai2b.Init.NoDivider = SAI_MASTERDIVIDER_ENABLE;
    hsai2b.Init.FIFOThreshold = SAI_FIFOTHRESHOLD_1QF;
    hsai2b.Init.AudioFrequency = SAI_AUDIO_FREQUENCY_16K;
    hsai2b.Init.MckOutput = SAI_MCK_OUTPUT_DISABLE;
    hsai2b.Init.MonoStereoMode = SAI_STEREOMODE;  /* Use stereo to receive I2S format */
    hsai2b.Init.CompandingMode = SAI_NOCOMPANDING;
    hsai2b.Init.TriState = SAI_OUTPUT_NOTRELEASED;

    /* I2S Protocol */
    hsai2b.Init.Protocol = SAI_FREE_PROTOCOL;
    hsai2b.Init.DataSize = SAI_DATASIZE_32;
    hsai2b.Init.FirstBit = SAI_FIRSTBIT_MSB;
    hsai2b.Init.ClockStrobing = SAI_CLOCKSTROBING_RISINGEDGE;  /* INMP441: sample on rising edge when data is stable */

    /* Frame: 64 bits total (32 per channel) */
    hsai2b.FrameInit.FrameLength = 64;
    hsai2b.FrameInit.ActiveFrameLength = 32;
    hsai2b.FrameInit.FSDefinition = SAI_FS_CHANNEL_IDENTIFICATION;
    hsai2b.FrameInit.FSPolarity = SAI_FS_ACTIVE_LOW;  /* WS low = left channel */
    hsai2b.FrameInit.FSOffset = SAI_FS_BEFOREFIRSTBIT;

    /* Slots */
    hsai2b.SlotInit.FirstBitOffset = 0;
    hsai2b.SlotInit.SlotSize = SAI_SLOTSIZE_32B;
    hsai2b.SlotInit.SlotNumber = 2;
    hsai2b.SlotInit.SlotActive = SAI_SLOTACTIVE_0 | SAI_SLOTACTIVE_1;

    status = HAL_SAI_Init(&hsai2b);
    if (status != HAL_OK)
    {
        LOG_E("SAI2 init failed: %d, err=0x%08X", status, hsai2b.ErrorCode);
        return -RT_ERROR;
    }

    LOG_I("SAI2_Block_B initialized (Master RX, I2S, 16kHz)");
    return RT_EOK;
}

static void sai_peripheral_deinit(void)
{
    HAL_SAI_DeInit(&hsai2b);
    __HAL_RCC_SAI2_CLK_DISABLE();
}

#else /* Use SAI1 */

static rt_err_t sai_peripheral_init(void)
{
    /* SAI1 implementation would go here */
    /* This is more complex as SAI1_Block_B needs clocks from Block_A */
    LOG_E("SAI1 mode not yet implemented");
    LOG_E("Please use SAI2 mode: rewire INMP441 SD to PE7");
    return -RT_ERROR;
}

static void sai_peripheral_deinit(void)
{
    /* SAI1 cleanup */
}

#endif

/* ==================== DMA Configuration ==================== */

static rt_err_t sai_dma_init(void)
{
    __HAL_RCC_GPDMA1_CLK_ENABLE();

#if USE_SAI2_WITH_PE7
    HAL_DMA_DeInit(&hdma_sai2b);

    hdma_sai2b.Instance = GPDMA1_Channel0;
    hdma_sai2b.Init.Request = GPDMA1_REQUEST_SAI2_B;
    hdma_sai2b.Init.BlkHWRequest = DMA_BREQ_SINGLE_BURST;
    hdma_sai2b.Init.Direction = DMA_PERIPH_TO_MEMORY;
    hdma_sai2b.Init.SrcInc = DMA_SINC_FIXED;
    hdma_sai2b.Init.DestInc = DMA_DINC_INCREMENTED;
    hdma_sai2b.Init.SrcDataWidth = DMA_SRC_DATAWIDTH_WORD;
    hdma_sai2b.Init.DestDataWidth = DMA_DEST_DATAWIDTH_WORD;
    hdma_sai2b.Init.Priority = DMA_HIGH_PRIORITY;
    hdma_sai2b.Init.SrcBurstLength = 1;
    hdma_sai2b.Init.DestBurstLength = 1;
    hdma_sai2b.Init.TransferAllocatedPort = DMA_SRC_ALLOCATED_PORT0 | DMA_DEST_ALLOCATED_PORT1;
    hdma_sai2b.Init.TransferEventMode = DMA_TCEM_BLOCK_TRANSFER;
    hdma_sai2b.Init.Mode = DMA_NORMAL;

    if (HAL_DMA_Init(&hdma_sai2b) != HAL_OK)
    {
        LOG_E("DMA init failed");
        return -RT_ERROR;
    }

    __HAL_LINKDMA(&hsai2b, hdmarx, hdma_sai2b);

    HAL_NVIC_SetPriority(GPDMA1_Channel0_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(GPDMA1_Channel0_IRQn);
#endif

    LOG_I("DMA initialized");
    return RT_EOK;
}

static void sai_dma_deinit(void)
{
    HAL_NVIC_DisableIRQ(GPDMA1_Channel0_IRQn);
#if USE_SAI2_WITH_PE7
    HAL_DMA_DeInit(&hdma_sai2b);
#endif
}

/* ==================== Interrupt Handlers ==================== */

void GPDMA1_Channel0_IRQHandler(void)
{
    rt_interrupt_enter();
#if USE_SAI2_WITH_PE7
    HAL_DMA_IRQHandler(&hdma_sai2b);
#endif
    rt_interrupt_leave();
}

/* ==================== Data Processing ==================== */

static void process_dma_data(int32_t *src, uint32_t sample_count)
{
    inmp441_device_t *dev = &g_inmp441_dev;

    if (!dev->is_running)
        return;

    /* Debug output every ~100 calls */
    if (debug_print_counter++ % 100 == 0)
    {
        rt_kprintf("\n[DMA] samples=%d\n", sample_count);
        rt_kprintf("  Raw: 0x%08X, 0x%08X, 0x%08X, 0x%08X\n",
                   (uint32_t)src[0], (uint32_t)src[1],
                   (uint32_t)src[2], (uint32_t)src[3]);
    }

    if (dev->frame_count >= AUDIO_BUFFER_COUNT)
    {
        dev->overrun_count++;
        return;
    }

    audio_frame_t *frame = &dev->frames[dev->write_idx];

    /*
     * SAI is configured in STEREO mode, data format: [L, R, L, R, ...]
     * INMP441 L/R pin = GND: outputs on LEFT channel (index 0, 2, 4, ...)
     *
     * Debug: print both channels to see which one has data
     */
    uint32_t mono_count = sample_count / 2;  /* Half the samples after extracting mono */
    uint32_t copy_size = (mono_count < AUDIO_FRAME_SIZE) ? mono_count : AUDIO_FRAME_SIZE;

    /* Debug: check both channels */
    if (debug_print_counter % 100 == 1)
    {
        rt_kprintf("  L[0]=0x%08X R[0]=0x%08X L[1]=0x%08X R[1]=0x%08X\n",
                   (uint32_t)src[0], (uint32_t)src[1],
                   (uint32_t)src[2], (uint32_t)src[3]);
    }

    /* INMP441 L/R=GND: Extract LEFT channel (src[0], src[2], src[4], ...)
     * INMP441输出24-bit数据左对齐在32-bit中
     * 右移8位得到24-bit有符号值，保留足够的动态范围
     */
    for (uint32_t i = 0; i < copy_size; i++)
    {
        frame->buffer[i] = src[i * 2] >> 8;  /* 右移8位: 保留24-bit动态范围 */
    }

    frame->size = copy_size;
    frame->sample_rate = INMP441_SAMPLE_RATE;
    frame->channels = INMP441_CHANNEL_NUM;
    frame->bit_width = INMP441_BIT_WIDTH;
    frame->timestamp = rt_tick_get();

    dev->write_idx = (dev->write_idx + 1) % AUDIO_BUFFER_COUNT;
    dev->frame_count++;
    dev->total_frames++;

    if (dev->buffer_sem)
        rt_sem_release(dev->buffer_sem);
}

/* ==================== HAL Callbacks ==================== */

void HAL_SAI_RxHalfCpltCallback(SAI_HandleTypeDef *hsai)
{
#if USE_SAI2_WITH_PE7
    if (hsai->Instance == SAI2_Block_B)
    {
        process_dma_data(&dma_buffer[0], SAI_DMA_BUFFER_SIZE);
    }
#endif
}

void HAL_SAI_RxCpltCallback(SAI_HandleTypeDef *hsai)
{
#if USE_SAI2_WITH_PE7
    if (hsai->Instance == SAI2_Block_B)
    {
        process_dma_data(&dma_buffer[SAI_DMA_BUFFER_SIZE], SAI_DMA_BUFFER_SIZE);

        if (g_inmp441_dev.is_running)
        {
            HAL_SAI_Receive_DMA(hsai, (uint8_t *)dma_buffer, SAI_DMA_BUFFER_SIZE * 2);
        }
    }
#endif
}

void HAL_SAI_ErrorCallback(SAI_HandleTypeDef *hsai)
{
#if USE_SAI2_WITH_PE7
    if (hsai->Instance == SAI2_Block_B)
    {
        g_inmp441_dev.dma_errors++;
        LOG_E("SAI error: 0x%08X", hsai->ErrorCode);

        HAL_SAI_DMAStop(hsai);
        if (g_inmp441_dev.is_running)
        {
            HAL_SAI_Receive_DMA(hsai, (uint8_t *)dma_buffer, SAI_DMA_BUFFER_SIZE * 2);
        }
    }
#endif
}

/* ==================== Public API ==================== */

rt_err_t inmp441_init(void)
{
    inmp441_device_t *dev = &g_inmp441_dev;
    rt_err_t result;

    rt_kprintf("\n");
    rt_kprintf("================================================\n");
    rt_kprintf("  INMP441 Driver - FIXED VERSION\n");
    rt_kprintf("================================================\n");
#if USE_SAI2_WITH_PE7
    rt_kprintf("  Mode: SAI2_Block_B\n");
    rt_kprintf("  Pins: PA2(SCK), PC0(WS), PE7(SD) <-- NOTE: PE7!\n");
#else
    rt_kprintf("  Mode: SAI1_Block_B (not implemented)\n");
#endif
    rt_kprintf("================================================\n\n");

    if (dev->is_initialized)
    {
        LOG_W("Already initialized");
        return RT_EOK;
    }

    rt_memset(dev, 0, sizeof(inmp441_device_t));

    dev->buffer_sem = rt_sem_create("sai_sem", 0, RT_IPC_FLAG_FIFO);
    if (!dev->buffer_sem)
    {
        LOG_E("Failed to create semaphore");
        return -RT_ENOMEM;
    }

    dev->lock = rt_mutex_create("sai_lock", RT_IPC_FLAG_PRIO);
    if (!dev->lock)
    {
        result = -RT_ENOMEM;
        goto _exit;
    }

    for (int i = 0; i < AUDIO_BUFFER_COUNT; i++)
    {
        dev->frames[i].buffer = rt_malloc(AUDIO_FRAME_SIZE * sizeof(int32_t));
        if (!dev->frames[i].buffer)
        {
            result = -RT_ENOMEM;
            goto _exit;
        }
        rt_memset(dev->frames[i].buffer, 0, AUDIO_FRAME_SIZE * sizeof(int32_t));
    }

    sai_gpio_init();

    result = sai_peripheral_init();
    if (result != RT_EOK)
        goto _exit;

    result = sai_dma_init();
    if (result != RT_EOK)
        goto _exit;

    dev->is_initialized = RT_TRUE;
    LOG_I("Initialization complete");
    return RT_EOK;

_exit:
    if (dev->buffer_sem) rt_sem_delete(dev->buffer_sem);
    if (dev->lock) rt_mutex_delete(dev->lock);
    for (int i = 0; i < AUDIO_BUFFER_COUNT; i++)
    {
        if (dev->frames[i].buffer) rt_free(dev->frames[i].buffer);
    }
    return result;
}

rt_err_t inmp441_deinit(void)
{
    inmp441_device_t *dev = &g_inmp441_dev;

    if (!dev->is_initialized)
        return RT_EOK;

    if (dev->is_running)
        inmp441_stop();

    sai_dma_deinit();
    sai_peripheral_deinit();
    sai_gpio_deinit();

    for (int i = 0; i < AUDIO_BUFFER_COUNT; i++)
    {
        if (dev->frames[i].buffer)
        {
            rt_free(dev->frames[i].buffer);
            dev->frames[i].buffer = RT_NULL;
        }
    }

    if (dev->buffer_sem) rt_sem_delete(dev->buffer_sem);
    if (dev->lock) rt_mutex_delete(dev->lock);

    dev->is_initialized = RT_FALSE;
    LOG_I("Deinitialized");
    return RT_EOK;
}

rt_err_t inmp441_start(void)
{
    inmp441_device_t *dev = &g_inmp441_dev;

    if (!dev->is_initialized)
    {
        LOG_E("Not initialized");
        return -RT_ERROR;
    }

    if (dev->is_running)
        return RT_EOK;

    dev->write_idx = 0;
    dev->read_idx = 0;
    dev->frame_count = 0;

    rt_memset(dma_buffer, 0, sizeof(dma_buffer));

#if USE_SAI2_WITH_PE7
    if (HAL_SAI_Receive_DMA(&hsai2b, (uint8_t *)dma_buffer, SAI_DMA_BUFFER_SIZE * 2) != HAL_OK)
    {
        LOG_E("Failed to start DMA: err=0x%08X", hsai2b.ErrorCode);
        return -RT_ERROR;
    }
#endif

    dev->is_running = RT_TRUE;
    LOG_I("Started");
    return RT_EOK;
}

rt_err_t inmp441_stop(void)
{
    inmp441_device_t *dev = &g_inmp441_dev;

    if (!dev->is_running)
        return RT_EOK;

#if USE_SAI2_WITH_PE7
    HAL_SAI_DMAStop(&hsai2b);
#endif

    dev->is_running = RT_FALSE;
    LOG_I("Stopped (frames=%d, errors=%d)", dev->total_frames, dev->dma_errors);
    return RT_EOK;
}

rt_err_t inmp441_read_frame(audio_frame_t *frame, rt_int32_t timeout)
{
    inmp441_device_t *dev = &g_inmp441_dev;

    if (!dev->is_running || !frame)
        return -RT_ERROR;

    if (rt_sem_take(dev->buffer_sem, timeout) != RT_EOK)
        return -RT_ETIMEOUT;

    rt_mutex_take(dev->lock, RT_WAITING_FOREVER);

    audio_frame_t *src = &dev->frames[dev->read_idx];
    frame->buffer = rt_malloc(src->size * sizeof(int32_t));
    if (!frame->buffer)
    {
        rt_mutex_release(dev->lock);
        return -RT_ENOMEM;
    }

    rt_memcpy(frame->buffer, src->buffer, src->size * sizeof(int32_t));
    frame->size = src->size;
    frame->sample_rate = src->sample_rate;
    frame->channels = src->channels;
    frame->bit_width = src->bit_width;
    frame->timestamp = src->timestamp;

    dev->read_idx = (dev->read_idx + 1) % AUDIO_BUFFER_COUNT;
    dev->frame_count--;

    rt_mutex_release(dev->lock);
    return RT_EOK;
}

void inmp441_get_stats(uint32_t *total_frames, uint32_t *overrun_count)
{
    if (total_frames) *total_frames = g_inmp441_dev.total_frames;
    if (overrun_count) *overrun_count = g_inmp441_dev.overrun_count;
}

void inmp441_reset_stats(void)
{
    g_inmp441_dev.total_frames = 0;
    g_inmp441_dev.overrun_count = 0;
    g_inmp441_dev.dma_errors = 0;
}

rt_bool_t inmp441_is_running(void)
{
    return g_inmp441_dev.is_running;
}

inmp441_device_t *inmp441_get_device(void)
{
    return &g_inmp441_dev;
}

/* ==================== Debug Functions ==================== */

void inmp441_debug_direct_read(void)
{
    rt_kprintf("\n========== SAI Debug ==========\n");

#if USE_SAI2_WITH_PE7
    rt_kprintf("Using SAI2_Block_B with PE7 for SD\n");
    rt_kprintf("SAI2_Block_B->CR1: 0x%08X\n", SAI2_Block_B->CR1);
    rt_kprintf("SAI2_Block_B->SR:  0x%08X\n", SAI2_Block_B->SR);

    /* Check PE7 state */
    rt_kprintf("\nGPIO states:\n");
    rt_kprintf("  PA2 (SCK): %d\n", (GPIOA->IDR & GPIO_PIN_2) ? 1 : 0);
    rt_kprintf("  PC0 (WS):  %d\n", (GPIOC->IDR & GPIO_PIN_0) ? 1 : 0);
    rt_kprintf("  PE7 (SD):  %d\n", (GPIOE->IDR & GPIO_PIN_7) ? 1 : 0);

    /* Sample PE7 */
    int high = 0, low = 0;
    for (int i = 0; i < 1000; i++)
    {
        if (GPIOE->IDR & GPIO_PIN_7) high++;
        else low++;
    }
    rt_kprintf("\nPE7 sampling: HIGH=%d, LOW=%d\n", high, low);

    if (high == 0 && low == 1000)
        rt_kprintf("WARNING: PE7 stuck LOW - check wiring!\n");
    else if (high > 100 && low > 100)
        rt_kprintf("OK: PE7 shows activity\n");
#else
    rt_kprintf("SAI1 mode - checking PE3\n");
    int high = 0, low = 0;
    for (int i = 0; i < 1000; i++)
    {
        if (GPIOE->IDR & GPIO_PIN_3) high++;
        else low++;
    }
    rt_kprintf("PE3 sampling: HIGH=%d, LOW=%d\n", high, low);
#endif

    rt_kprintf("================================\n\n");
}
