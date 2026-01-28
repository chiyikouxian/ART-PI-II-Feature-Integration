/*
 * stt_manager.c - STT管理器
 *
 * 工作流程:
 * 1. audio_process的VAD检测到语音并录完一段后，通过回调调用stt_manager_feed_recording()
 * 2. stt_manager线程从邮箱收到通知，编码WAV并调用百度API
 * 3. 识别结果存储在共享变量中，串口打印，并通知OLED线程刷新
 *
 * 内存优化: 不再复制PCM数据，直接使用audio_process传入的指针进行编码
 */
#include "stt_manager.h"
#include "audio_encoder.h"
#include "stt_baidu.h"
#include "stt_config.h"
#include "../SAI/drv_sai_inmp441.h"  /* 用于暂停/恢复音频采集 */
#include <string.h>

#define STT_THREAD_STACK_SIZE   4096
#define STT_THREAD_PRIORITY     18

/* STT管理器上下文 */
typedef struct {
    rt_thread_t         thread;
    rt_bool_t           running;
    stt_state_t         state;
    stt_result_callback_t callback;

    /* 邮箱: audio_process -> stt_manager 通知 */
    rt_mailbox_t        mbox;

    /* 录音数据指针(直接指向audio_process的缓冲，不复制) */
    const int32_t      *pcm_ptr;
    uint32_t            pcm_samples;
    rt_mutex_t          lock;
    volatile rt_bool_t  data_ready;

    /* 最近识别结果 */
    char                last_text[STT_RESULT_MAX_LEN];
    volatile rt_bool_t  result_updated;
} stt_manager_ctx_t;

static stt_manager_ctx_t g_stt_ctx = {0};

/* 邮箱消息 */
#define STT_MSG_NEW_RECORDING   ((rt_ubase_t)1)

/* ==================== STT处理线程 ==================== */

static void stt_thread_entry(void *parameter)
{
    stt_manager_ctx_t *ctx = &g_stt_ctx;
    rt_ubase_t msg;

    rt_kprintf("[STT] Thread started\n");

    /* 等待WiFi连接后再获取token */
    rt_thread_mdelay(5000);

    /* 初始化百度STT (获取token) */
    ctx->state = STT_STATE_IDLE;
    if (stt_baidu_init() != RT_EOK)
    {
        rt_kprintf("[STT] Warning: Token init failed, will retry on first use\n");
    }

    while (ctx->running)
    {
        /* 等待录音完成通知 */
        if (rt_mb_recv(ctx->mbox, &msg, RT_WAITING_FOREVER) != RT_EOK)
            continue;

        if (msg != STT_MSG_NEW_RECORDING)
            continue;

        /* ---- 步骤1: 编码WAV ---- */
        ctx->state = STT_STATE_ENCODING;
        rt_kprintf("[STT] Encoding audio...\n");

        rt_mutex_take(ctx->lock, RT_WAITING_FOREVER);

        if (!ctx->data_ready || ctx->pcm_ptr == RT_NULL)
        {
            rt_mutex_release(ctx->lock);
            ctx->state = STT_STATE_IDLE;
            continue;
        }

        uint32_t samples = ctx->pcm_samples;
        uint32_t duration_ms = (samples * 1000) / STT_SAMPLE_RATE;

        /* 检查最小时长 */
        if (duration_ms < STT_MIN_RECORD_MS)
        {
            rt_kprintf("[STT] Too short (%d ms), skipping\n", duration_ms);
            ctx->data_ready = RT_FALSE;
            rt_mutex_release(ctx->lock);
            ctx->state = STT_STATE_IDLE;
            continue;
        }

        /* 直接使用audio_process的缓冲进行编码 */
        uint8_t *wav_buf = RT_NULL;
        uint32_t wav_size = 0;
        rt_err_t ret = audio_encode_wav(ctx->pcm_ptr, samples, &wav_buf, &wav_size);

        ctx->data_ready = RT_FALSE;
        rt_mutex_release(ctx->lock);

        if (ret != RT_EOK || wav_buf == RT_NULL)
        {
            rt_kprintf("[STT] Encode failed\n");
            ctx->state = STT_STATE_ERROR;
            rt_thread_mdelay(1000);
            ctx->state = STT_STATE_IDLE;
            continue;
        }

        rt_kprintf("[STT] WAV ready: %d bytes (%d ms)\n", wav_size, duration_ms);

        /* ---- 步骤2: 上传识别 ---- */
        ctx->state = STT_STATE_UPLOADING;
        rt_kprintf("[STT] Uploading to Baidu...\n");

        /* 暂停DMA音频采集以释放内存给WiFi */
        rt_kprintf("[STT] Pausing audio capture for upload...\n");
        inmp441_stop();
        rt_thread_mdelay(50);  /* 等待DMA完全停止 */

        stt_result_t result;
        ret = stt_baidu_recognize(wav_buf, wav_size, &result);

        /* 恢复DMA音频采集 */
        rt_kprintf("[STT] Resuming audio capture...\n");
        inmp441_start();

        /* WAV数据已发送，释放 */
        rt_free(wav_buf);

        if (ret == RT_EOK)
        {
            /* ---- 步骤3: 显示结果 ---- */
            ctx->state = STT_STATE_DISPLAYING;

            rt_strncpy(ctx->last_text, result.text, sizeof(ctx->last_text) - 1);
            ctx->result_updated = RT_TRUE;

            rt_kprintf("\n==============================\n");
            rt_kprintf("  STT Result: %s\n", result.text);
            rt_kprintf("==============================\n\n");

            /* 调用用户回调 */
            if (ctx->callback)
                ctx->callback(result.text);

            /* 缩短显示时间，尽快处理下一段 */
            rt_thread_mdelay(500);
        }
        else
        {
            ctx->state = STT_STATE_ERROR;
            rt_kprintf("[STT] Recognition error %d: %s\n", result.err_no, result.err_msg);

            rt_snprintf(ctx->last_text, sizeof(ctx->last_text),
                       "ERR:%d", result.err_no);
            ctx->result_updated = RT_TRUE;

            rt_thread_mdelay(500);
        }

        ctx->state = STT_STATE_IDLE;

        /* 检查是否有新的录音等待处理 */
        if (ctx->data_ready)
        {
            rt_kprintf("[STT] New recording available, processing...\n");
            rt_mb_send(ctx->mbox, STT_MSG_NEW_RECORDING);
        }
    }

    rt_kprintf("[STT] Thread exited\n");
}

/* ==================== 公开API ==================== */

rt_err_t stt_manager_init(stt_result_callback_t callback)
{
    stt_manager_ctx_t *ctx = &g_stt_ctx;

    rt_kprintf("[STT] Initializing STT manager...\n");

    rt_memset(ctx, 0, sizeof(stt_manager_ctx_t));
    ctx->callback = callback;

    /* 不再分配PCM缓冲，直接使用audio_process的数据 */

    ctx->lock = rt_mutex_create("stt_lock", RT_IPC_FLAG_FIFO);
    if (ctx->lock == RT_NULL)
    {
        return -RT_ENOMEM;
    }

    ctx->mbox = rt_mb_create("stt_mb", 4, RT_IPC_FLAG_FIFO);
    if (ctx->mbox == RT_NULL)
    {
        rt_mutex_delete(ctx->lock);
        return -RT_ENOMEM;
    }

    ctx->thread = rt_thread_create("stt_mgr",
                                   stt_thread_entry, RT_NULL,
                                   STT_THREAD_STACK_SIZE,
                                   STT_THREAD_PRIORITY, 10);
    if (ctx->thread == RT_NULL)
    {
        rt_mb_delete(ctx->mbox);
        rt_mutex_delete(ctx->lock);
        return -RT_ENOMEM;
    }

    rt_kprintf("[STT] STT manager initialized (zero-copy mode)\n");
    return RT_EOK;
}

rt_err_t stt_manager_start(void)
{
    stt_manager_ctx_t *ctx = &g_stt_ctx;

    if (ctx->running)
        return RT_EOK;

    ctx->running = RT_TRUE;
    rt_thread_startup(ctx->thread);
    rt_kprintf("[STT] STT manager started\n");
    return RT_EOK;
}

rt_err_t stt_manager_stop(void)
{
    stt_manager_ctx_t *ctx = &g_stt_ctx;

    if (!ctx->running)
        return RT_EOK;

    ctx->running = RT_FALSE;
    rt_mb_send(ctx->mbox, 0);
    rt_thread_mdelay(200);
    rt_kprintf("[STT] STT manager stopped\n");
    return RT_EOK;
}

stt_state_t stt_manager_get_state(void)
{
    return g_stt_ctx.state;
}

const char *stt_manager_get_last_text(void)
{
    return g_stt_ctx.last_text;
}

/**
 * @brief 供audio_process回调调用: 录音完成后送入STT管理器
 *
 * 注意: 此函数直接保存指针，不复制数据。
 * audio_process在下次录音前不会修改这块内存，所以是安全的。
 * STT线程会在编码完成后释放对这块内存的引用。
 *
 * 改进: 即使STT忙也更新指针，这样至少处理最新的录音而不是丢弃
 */
void stt_manager_feed_recording(const int32_t *pcm32, uint32_t samples,
                                uint32_t sample_rate)
{
    stt_manager_ctx_t *ctx = &g_stt_ctx;

    if (!ctx->running)
        return;

    rt_mutex_take(ctx->lock, RT_WAITING_FOREVER);

    /* 总是更新指针到最新录音 */
    ctx->pcm_ptr = pcm32;
    ctx->pcm_samples = samples;
    ctx->data_ready = RT_TRUE;

    rt_mutex_release(ctx->lock);

    /* 如果STT线程空闲，发送通知；否则等它处理完后会检查data_ready */
    if (ctx->state == STT_STATE_IDLE)
    {
        rt_mb_send(ctx->mbox, STT_MSG_NEW_RECORDING);
    }
    else
    {
        rt_kprintf("[STT] Busy, will process latest when ready\n");
    }
}
