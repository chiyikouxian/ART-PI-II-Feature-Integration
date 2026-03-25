/**
 * @file    utf8_to_gb2312.c
 * @brief   UTF-8 到 GB2312 编码转换
 *
 * @details 使用编译进固件的 Unicode→GB2312 排序映射表进行二分查找转换。
 *          映射表包含 7445 个 {unicode, gb2312} 条目，覆盖 GB2312 全部字符。
 *          占用约 30KB Flash，无需依赖 TF 卡上的转换表文件。
 *
 *          转换流程: UTF-8 字节 → Unicode 码点 → 二分查找 → GB2312 编码
 */

#include <rtthread.h>
#include "utf8_to_gb2312.h"

#define DBG_TAG "oled.utf8"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* 引入 unicode_to_gb2312 映射表 (按 Unicode 码点升序排列) */
#include "unicode_to_gb2312_table.inc"

#define UNI2GB_TABLE_SIZE  7445

int utf8_to_gb2312_init(void)
{
    /* 映射表已编译进固件，无需初始化 */
    LOG_I("UTF8→GB2312 converter ready (embedded table, %d entries)", UNI2GB_TABLE_SIZE);
    return 0;
}

void utf8_to_gb2312_deinit(void)
{
    /* 无需清理 */
}

/**
 * @brief  从 UTF-8 编码提取 Unicode 码点
 * @param  utf8  UTF-8 字符串
 * @param  unicode  输出 Unicode 码点
 * @return 消耗的字节数, 0 表示错误
 */
static int utf8_decode(const char *utf8, uint32_t *unicode)
{
    uint8_t c = (uint8_t)utf8[0];

    if ((c & 0x80) == 0x00)
    {
        *unicode = c;
        return 1;
    }
    else if ((c & 0xE0) == 0xC0)
    {
        if ((utf8[1] & 0xC0) != 0x80) return 0;
        *unicode = ((uint32_t)(c & 0x1F) << 6) | (utf8[1] & 0x3F);
        return 2;
    }
    else if ((c & 0xF0) == 0xE0)
    {
        if ((utf8[1] & 0xC0) != 0x80 || (utf8[2] & 0xC0) != 0x80) return 0;
        *unicode = ((uint32_t)(c & 0x0F) << 12) |
                   ((uint32_t)(utf8[1] & 0x3F) << 6) |
                   (utf8[2] & 0x3F);
        return 3;
    }
    else if ((c & 0xF8) == 0xF0)
    {
        if ((utf8[1] & 0xC0) != 0x80 || (utf8[2] & 0xC0) != 0x80 ||
            (utf8[3] & 0xC0) != 0x80) return 0;
        *unicode = ((uint32_t)(c & 0x07) << 18) |
                   ((uint32_t)(utf8[1] & 0x3F) << 12) |
                   ((uint32_t)(utf8[2] & 0x3F) << 6) |
                   (utf8[3] & 0x3F);
        return 4;
    }

    return 0;
}

/**
 * @brief  二分查找 Unicode→GB2312 映射
 * @param  unicode  Unicode 码点
 * @param  gb_out   输出 GB2312 编码 [高字节, 低字节]
 * @return 0 成功, -1 未找到
 */
static int unicode_lookup_gb2312(uint32_t unicode, uint8_t *gb_out)
{
    int low = 0;
    int high = UNI2GB_TABLE_SIZE - 1;

    while (low <= high)
    {
        int mid = (low + high) / 2;
        uint16_t key = unicode_to_gb2312[mid][0];

        if (key == (uint16_t)unicode)
        {
            uint16_t gb = unicode_to_gb2312[mid][1];
            gb_out[0] = (uint8_t)(gb >> 8);    /* 高字节 */
            gb_out[1] = (uint8_t)(gb & 0xFF);  /* 低字节 */
            return 0;
        }
        else if (key < (uint16_t)unicode)
        {
            low = mid + 1;
        }
        else
        {
            high = mid - 1;
        }
    }

    return -1;
}

int utf8_char_to_gb2312(const char *utf8_char, uint8_t *gb_out)
{
    uint32_t unicode;

    if (utf8_char == RT_NULL || gb_out == RT_NULL)
        return -1;

    if (utf8_decode(utf8_char, &unicode) == 0)
        return -1;

    /* ASCII 不需要转换 */
    if (unicode < 0x80)
        return -1;

    /* 超出 BMP 范围 */
    if (unicode > 0xFFFF)
        return -1;

    return unicode_lookup_gb2312(unicode, gb_out);
}
