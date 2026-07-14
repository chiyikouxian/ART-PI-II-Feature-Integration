/**
 * @file    oled_font_hzk.c
 * @brief   HZK16 点阵字库读取模块
 *
 * @details 从编译进固件的 HZK16 字库数组中读取字模数据 (存储在外部 Flash),
 *          根据 GB2312 区码和位码定位汉字字模, 并将横向取模格式
 *          转换为 OLED 所需的纵向取模格式。
 *
 *          HZK16 格式: 横向取模, 字节高位在左, 每行2字节, 共16行=32字节
 *          OLED 格式:  纵向8点, 高位在下(B7在底), 先左后右, 先上后下
 */

#include <rtthread.h>
#include "oled_font_hzk.h"

#define DBG_TAG "oled.hzk"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* HZK16 每个字符占 32 字节 */
#define HZK16_CHAR_SIZE     32
/* GB2312 每区 94 个字符 */
#define HZK16_CHARS_PER_QU  94

/* 引入 HZK16 字库数据数组 (267616 字节, 存储在外部 Flash) */
#include "hzk16_data.inc"

int oled_hzk16_init(void)
{
    /* 字库已编译进固件，无需初始化 */
    LOG_I("HZK16 font ready (embedded, %d bytes)", (int)sizeof(hzk16_font_data));
    return 0;
}

void oled_hzk16_deinit(void)
{
    /* 无需清理 */
}

/**
 * @brief  将 HZK16 横向取模数据转换为 OLED 纵向取模数据
 * @param  hzk_raw  输入: 32 字节横向取模 (16行×2字节)
 * @param  oled_buf 输出: 32 字节纵向取模 (上半16列 + 下半16列)
 */
static void hzk_to_oled_format(const uint8_t *hzk_raw, uint8_t *oled_buf)
{
    int col, row;

    for (col = 0; col < 16; col++)
    {
        uint8_t upper = 0, lower = 0;

        /* 上半部分: 第 0~7 行 */
        for (row = 0; row < 8; row++)
        {
            int byte_idx = row * 2 + (col >= 8 ? 1 : 0);
            int bit_idx = 7 - (col % 8);

            if (hzk_raw[byte_idx] & (1 << bit_idx))
                upper |= (1 << row);   /* OLED: 高位在下 */
        }

        /* 下半部分: 第 8~15 行 */
        for (row = 8; row < 16; row++)
        {
            int byte_idx = row * 2 + (col >= 8 ? 1 : 0);
            int bit_idx = 7 - (col % 8);

            if (hzk_raw[byte_idx] & (1 << bit_idx))
                lower |= (1 << (row - 8));
        }

        oled_buf[col] = upper;
        oled_buf[16 + col] = lower;
    }
}

int oled_hzk16_get_char(uint8_t qh, uint8_t wh, uint8_t *buf)
{
    uint32_t offset;

    if (qh < 1 || qh > 94 || wh < 1 || wh > 94)
        return -1;

    /* HZK16 偏移公式 */
    offset = ((uint32_t)(qh - 1) * HZK16_CHARS_PER_QU + (wh - 1)) * HZK16_CHAR_SIZE;

    /* 边界检查 */
    if (offset + HZK16_CHAR_SIZE > sizeof(hzk16_font_data))
        return -1;

    /* 直接从数组读取并转换格式 */
    hzk_to_oled_format(&hzk16_font_data[offset], buf);

    return 0;
}
