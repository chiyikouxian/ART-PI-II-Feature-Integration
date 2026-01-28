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

    /* 32-bit PCM → 16-bit PCM (右移8位: 24bit数据在32bit容器中) */
    int16_t *pcm16 = (int16_t *)(buf + sizeof(wav_header_t));
    for (uint32_t i = 0; i < samples; i++)
    {
        pcm16[i] = (int16_t)(pcm32[i] >> 8);
    }

    *out_buf  = buf;
    *out_size = total_size;

    rt_kprintf("[Encoder] WAV encoded: %d samples -> %d bytes\n", samples, total_size);
    return RT_EOK;
}
