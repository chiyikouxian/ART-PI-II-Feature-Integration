/*
 * audio_encoder.h - PCM音频编码为WAV格式
 */
#ifndef __AUDIO_ENCODER_H__
#define __AUDIO_ENCODER_H__

#include <rtthread.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* WAV文件头结构 (44字节) */
typedef struct {
    char     riff[4];           /* "RIFF" */
    uint32_t file_size;         /* 文件总大小 - 8 */
    char     wave[4];           /* "WAVE" */
    char     fmt[4];            /* "fmt " */
    uint32_t fmt_size;          /* 16 */
    uint16_t audio_format;      /* 1 = PCM */
    uint16_t num_channels;      /* 1 = mono */
    uint32_t sample_rate;       /* 16000 */
    uint32_t byte_rate;         /* sample_rate * channels * bits/8 */
    uint16_t block_align;       /* channels * bits/8 */
    uint16_t bits_per_sample;   /* 16 */
    char     data[4];           /* "data" */
    uint32_t data_size;         /* PCM数据大小 */
} wav_header_t;

/**
 * @brief 将32bit PCM数据编码为完整的WAV缓冲区(含头+16bit PCM数据)
 * @param pcm32     输入: 32-bit PCM采样数据
 * @param samples   输入: 采样数
 * @param out_buf   输出: WAV缓冲区指针(内部分配，调用者需rt_free)
 * @param out_size  输出: WAV缓冲区总字节数
 * @return RT_EOK成功
 */
rt_err_t audio_encode_wav(const int32_t *pcm32, uint32_t samples,
                          uint8_t **out_buf, uint32_t *out_size);

#ifdef __cplusplus
}
#endif

#endif /* __AUDIO_ENCODER_H__ */
