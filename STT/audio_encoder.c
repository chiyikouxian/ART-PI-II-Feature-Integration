/*
 * audio_encoder.c - PCM音频编码为WAV格式
 */
#include "audio_encoder.h"
#include "stt_config.h"
#include <string.h>

rt_err_t audio_encode_wav(const int32_t *pcm32, uint32_t samples,
                          uint8_t **out_buf, uint32_t *out_size)
{
    if (pcm32 == RT_NULL || samples == 0 || out_buf == RT_NULL || out_size == RT_NULL)
        return -RT_EINVAL;

    uint32_t pcm16_size = samples * sizeof(int16_t);
    uint32_t total_size = sizeof(wav_header_t) + pcm16_size;

    /* 分配输出缓冲区: WAV头 + 16-bit PCM数据 */
    uint8_t *buf = (uint8_t *)rt_malloc(total_size);
    if (buf == RT_NULL)
    {
        rt_kprintf("[Encoder] Failed to alloc %d bytes\n", total_size);
        return -RT_ENOMEM;
    }

    /* 填充WAV头 */
    wav_header_t *hdr = (wav_header_t *)buf;
    rt_memcpy(hdr->riff, "RIFF", 4);
    hdr->file_size      = total_size - 8;
    rt_memcpy(hdr->wave, "WAVE", 4);
    rt_memcpy(hdr->fmt,  "fmt ", 4);
    hdr->fmt_size        = 16;
    hdr->audio_format    = 1;   /* PCM */
    hdr->num_channels    = STT_CHANNEL;
    hdr->sample_rate     = STT_SAMPLE_RATE;
    hdr->byte_rate       = STT_SAMPLE_RATE * STT_CHANNEL * (STT_BIT_DEPTH / 8);
    hdr->block_align     = STT_CHANNEL * (STT_BIT_DEPTH / 8);
    hdr->bits_per_sample = STT_BIT_DEPTH;
    rt_memcpy(hdr->data, "data", 4);
    hdr->data_size       = pcm16_size;

    /* 32-bit PCM → 16-bit PCM
     * 数据流:
     * 1. INMP441输出24-bit有符号数据，左对齐在32-bit (如 0x005BDC00)
     * 2. DMA处理: src >> 8 得到24-bit值 (如 0x00005BDC = 23516)
     * 3. 这里: 直接截断为16-bit (高位可能溢出，需要处理)
     *
     * 实际上 DMA处理后的值范围是 -8388608 ~ +8388607 (24-bit)
     * 需要右移8位得到 -32768 ~ +32767 (16-bit)
     */
    int16_t *pcm16 = (int16_t *)(buf + sizeof(wav_header_t));

    /* 调试: 打印前几个样本的原始值和转换后的值 */
    rt_kprintf("[Encoder] Sample preview: pcm32[0]=%d, pcm32[1]=%d, pcm32[2]=%d\n",
               (int)pcm32[0], (int)pcm32[1], (int)pcm32[2]);

    for (uint32_t i = 0; i < samples; i++)
    {
        int32_t sample = pcm32[i];
        /*
         * DMA处理时右移8位，得到24-bit有符号值 (-8388608 ~ +8388607)
         * 需要右移8位转换为16-bit范围 (-32768 ~ +32767)
         */
        sample = sample >> 8;
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;
        pcm16[i] = (int16_t)sample;
    }

    rt_kprintf("[Encoder] After convert: pcm16[0]=%d, pcm16[1]=%d, pcm16[2]=%d\n",
               (int)pcm16[0], (int)pcm16[1], (int)pcm16[2]);

    *out_buf  = buf;
    *out_size = total_size;

    rt_kprintf("[Encoder] WAV encoded: %d samples -> %d bytes\n", samples, total_size);
    return RT_EOK;
}
