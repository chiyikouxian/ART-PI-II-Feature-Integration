#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "str_code_cvt.h"

#ifdef CVT_GB2312_TO_UTF8
#include "gb2312_to_unicode.c"
#define GB2312_TO_UNICODE_TABLE_SIZE (sizeof(gb2312_to_unicode) / sizeof(gb2312_to_unicode[0]))
#else
const uint16_t (*gb2312_to_unicode)[2] = NULL;
#define GB2312_TO_UNICODE_TABLE_SIZE 0
#endif

#ifdef CVT_GBK_TO_UTF8
#include "gbk_to_unicode.c"
#define GBK_TO_UNICODE_TABLE_SIZE (sizeof(gbk_to_unicode) / sizeof(gbk_to_unicode[0]))
#else
const uint16_t (*gbk_to_unicode)[2] = NULL;
#define GBK_TO_UNICODE_TABLE_SIZE 0
#endif

#ifdef CVT_UTF8_TO_GB2312
#include "unicode_to_gb2312.c"
#define UNICODE_TO_GB2312_TABLE_SIZE (sizeof(unicode_to_gb2312) / sizeof(unicode_to_gb2312[0]))
#else
const uint16_t (*unicode_to_gb2312)[2] = NULL;
#define UNICODE_TO_GB2312_TABLE_SIZE 0
#endif

#ifdef CVT_UTF8_TO_GBK
#include "unicode_to_gbk.c"
#define UNICODE_TO_GBK_TABLE_SIZE (sizeof(unicode_to_gbk) / sizeof(unicode_to_gbk[0]))
#else
const uint16_t (*unicode_to_gbk)[2] = NULL;
#define UNICODE_TO_GBK_TABLE_SIZE 0
#endif


// 统计一个utf8字符串中第一个字符所占用的字节数
/*
U-00000000 - U-0000007F: 0xxxxxxx
U-00000080 - U-000007FF: 110xxxxx 10xxxxxx
U-00000800 - U-0000FFFF: 1110xxxx 10xxxxxx 10xxxxxx
U-00010000 - U-001FFFFF: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
U-00200000 - U-03FFFFFF: 111110xx 10xxxxxx 10xxxxxx 10xxxxxx 10xxxxxx
U-04000000 - U-7FFFFFFF: 1111110x 10xxxxxx 10xxxxxx 10xxxxxx 10xxxxxx 10xxxxxx
*/
// 这里使用最简单的方法来判断长度，前提是utf8字符串是合法的
static int32_t _utf8_char_len(char *utf8)
{
    if (NULL == utf8) return 0;

    if (((utf8[0] & 0x80)) == 0) return 1;
    else if ((utf8[0] & 0xE0) == 0xC0) return 2;
    else if ((utf8[0] & 0xF0) == 0xE0) return 3;
    else if ((utf8[0] & 0xF8) == 0xF0) return 4;
    else if ((utf8[0] & 0xFC) == 0xF8) return 5;
    else if ((utf8[0] & 0xFE) == 0xFC) return 6;
    return 0;
}

#if 0
// 更严格的判断方法如下
static int32_t _utf8_char_len(char *utf8)
{
    if (NULL == utf8) return 0;

    uint8_t first = (uint8_t)utf8[0];
    
    if ((first & 0x80) == 0) {
        return 1;  // ASCII: 0xxxxxxx
    }
    else if ((first & 0xE0) == 0xC0) {
        // 验证第二个字节必须是10xxxxxx
        if ((utf8[1] & 0xC0) != 0x80) return 0;
        return 2;  // 2字节: 110xxxxx 10xxxxxx
    }
    else if ((first & 0xF0) == 0xE0) {
        // 验证第二、三个字节必须是10xxxxxx
        if ((utf8[1] & 0xC0) != 0x80 || (utf8[2] & 0xC0) != 0x80) return 0;
        return 3;  // 3字节: 1110xxxx 10xxxxxx 10xxxxxx
    }
    else if ((first & 0xF8) == 0xF0) {
        // 验证后续3个字节必须是10xxxxxx
        if ((utf8[1] & 0xC0) != 0x80 || (utf8[2] & 0xC0) != 0x80 || 
            (utf8[3] & 0xC0) != 0x80) return 0;
        return 4;  // 4字节: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
    }
    else if ((first & 0xFC) == 0xF8) {
        // 验证后续4个字节
        if ((utf8[1] & 0xC0) != 0x80 || (utf8[2] & 0xC0) != 0x80 || 
            (utf8[3] & 0xC0) != 0x80 || (utf8[4] & 0xC0) != 0x80) return 0;
        return 5;
    }
    else if ((first & 0xFE) == 0xFC) {
        // 验证后续5个字节
        if ((utf8[1] & 0xC0) != 0x80 || (utf8[2] & 0xC0) != 0x80 || 
            (utf8[3] & 0xC0) != 0x80 || (utf8[4] & 0xC0) != 0x80 || 
            (utf8[5] & 0xC0) != 0x80) return 0;
        return 6;
    }
    return 0; // 非法UTF8序列
}
#endif

// 统计一个utf8字符串中的字符个数
static int32_t _utf8_char_num(char *utf8)
{
    int byte_num = 0;
    int len = 0;
    int char_num = 0;
    char *ptr = utf8;
 
    if (NULL == utf8)
        return 0;
 
    len = strlen(utf8);
    while (*ptr != '\0' && char_num < len)
    {
        byte_num = _utf8_char_len(ptr);
        if (byte_num == 0) return 0;
        ptr += byte_num;
        char_num++;
    }
 
    return char_num;
}

// 输入一个utf8的字符串，返回第一个汉字的小端unicode
static uint32_t _utf8_unicode(char *utf8, uint32_t *unicode)
{
    unsigned char *ptr = NULL;
    uint32_t unicode_tmp = 0;

    if (NULL == utf8) {*unicode = 0; return 0;}

    ptr = (unsigned char *)utf8;

    if (((ptr[0] & 0x80)) == 0)
    {
        unicode_tmp = ptr[0];
    }
    else if ((ptr[0] & 0xE0) == 0xC0)
    {
        unicode_tmp= (ptr[0] & 0x1f) << 6;
        unicode_tmp|= (ptr[1] & 0x3f);
    }
    else if ((ptr[0] & 0xF0) == 0xE0)
    {
        unicode_tmp= (ptr[0] & 0x0f) << 12;
        unicode_tmp|= (ptr[1] & 0x3f) << 6;
        unicode_tmp|= (ptr[2] & 0x3f);
    }
    else if ((ptr[0] & 0xF8) == 0xF0)
    {
        unicode_tmp= (ptr[0] & 0x07) << 18;
        unicode_tmp|= (ptr[1] & 0x3f) << 12;
        unicode_tmp|= (ptr[2] & 0x3f) << 6;
        unicode_tmp|= (ptr[3] & 0x3f);
    }
    else if ((ptr[0] & 0xFC) == 0xF8)
    {
        unicode_tmp= (ptr[0] & 0x03) << 24;
        unicode_tmp|= (ptr[1] & 0x3f) << 18;
        unicode_tmp|= (ptr[2] & 0x3f) << 12;
        unicode_tmp|= (ptr[3] & 0x3f) << 6;
        unicode_tmp|= (ptr[4] & 0x3f);
    }
    else if ((ptr[0] & 0xFE) == 0xFC)
    {
        unicode_tmp = (ptr[0] & 0x01) << 30;
        unicode_tmp |= (ptr[1] & 0x3f) << 24;
        unicode_tmp |= (ptr[2] & 0x3f) << 18;
        unicode_tmp |= (ptr[3] & 0x3f) << 12;
        unicode_tmp |= (ptr[4] & 0x3f) << 6;
        unicode_tmp |= (ptr[5] & 0x3f);
    }
    *unicode = unicode_tmp;
    return 0;
}

// 输入一个unicode，返回utf8的字符串
static uint32_t _unicode_utf8(uint32_t unicode, char *utf8)
{
    int32_t utf8_len = 0;
    if (unicode < 0x80) {
        utf8_len = 1;
    }
    else if (unicode < 0x800){
        utf8_len = 2;
    }
    else if (unicode < 0x10000) {
        if (unicode < 0xd800 || unicode >= 0xe000){
            utf8_len = 3;
        }
        else {
            utf8_len = 0;
        }
    }
    else if (unicode < 0x110000) {
        utf8_len = 4;
    }
    else {
        utf8_len = 0;
    }

    switch (utf8_len) {
        case 4:
            utf8[3] = 0x80 | (unicode & 0x3f);
            unicode = unicode >> 6;
            unicode |= 0x10000;
        case 3:
            utf8[2] = 0x80 | (unicode & 0x3f);
            unicode = unicode >> 6;
            unicode |= 0x800;
        case 2:
            utf8[1] = 0x80 | (unicode & 0x3f);
            unicode = unicode >> 6;
            unicode |= 0xc0;
        case 1:
        case 0:
            utf8[0] = unicode;
    }

    return utf8_len;
}

static uint16_t search_char_code(uint16_t code, const uint16_t (*table)[2], uint16_t table_size)
{
    uint16_t aim_code = 0, now_code = 0;
    uint16_t low, high, mid, aim, now;
    low = 0;
    high = table_size - 1;

    // 0x80以下为ASCII字符，不需要转换
    if (code < 0x80) {
        return code;
    }

    if (code < table[0][0] || code > table[table_size - 1][0]) {
        return 0;
    }

    while (low <= high) {
        mid = low + (high - low) / 2;
        now_code = table[mid][0];
        if (now_code < code) {
            low = mid + 1;
        }
        else if (now_code > code) {
            // 极端情况mid=0时，high=-1，溢出为65535，此时会出现数组越界，本身也属于查表失败，未找到指定编码
            if (mid == 0) { break; }
            high = mid - 1;
        }
        else {
            aim_code = table[mid][1];
            break;
        }
    }

    return aim_code;
}

static int32_t _cvt_check(code_type_e from, code_type_e to)
{
    #ifdef CVT_UTF8_TO_GB2312
    if (from == CODE_UTF8 && to == CODE_GB2312) { return 1; }
    #endif

    #ifdef CVT_UTF8_TO_GBK
    if (from == CODE_UTF8 && to == CODE_GBK) { return 1; }
    #endif

    #ifdef CVT_GB2312_TO_UTF8
    if (from == CODE_GB2312 && to == CODE_UTF8) { return 1; }
    #endif

    #ifdef CVT_GBK_TO_UTF8
    if (from == CODE_GBK && to == CODE_UTF8) { return 1; }
    #endif

    return 0;
}

int32_t string_code_convert(code_type_e from, code_type_e to, char *src, char *dst, uint32_t dst_size)
{
    if (from == to || !_cvt_check(from, to)) {
        return 0;
    }
    if (src == NULL || dst == NULL || dst_size == 0) {
        return 0;
    }

    uint32_t src_pos = 0;
    uint32_t src_pos_plus = 0;
    uint32_t dst_pos = 0;
    uint32_t unicode = 0;
    uint16_t gb_code = 0;
    char utf8_buf[6] = {0};
    int char_len = 0;


    while (src[src_pos] != '\0' && dst_pos < dst_size) {
        // ASCII字符直接复制
        if ((uint8_t)src[src_pos] < (uint8_t)0x80) {
            if (dst_pos + 1 >= dst_size) { goto end; }
            dst[dst_pos++] = src[src_pos++];
            continue;
        }

        // 根据源编码获取Unicode
        switch (from) {
            case CODE_UTF8:
                char_len = _utf8_char_len(&src[src_pos]);
                if (char_len == 0) { goto end; }
                _utf8_unicode(&src[src_pos], &unicode);
                src_pos_plus = char_len;
                break;
                
            case CODE_GB2312:
                if (src[src_pos+1] == '\0') { goto end; }
                gb_code = (src[src_pos] << 8) | (src[src_pos+1] & 0xFF);
                unicode = search_char_code(gb_code, gb2312_to_unicode, GB2312_TO_UNICODE_TABLE_SIZE);
                src_pos_plus = 2;
                break;
                
            case CODE_GBK:
                if (src[src_pos+1] == '\0') { goto end; }
                gb_code = (src[src_pos] << 8) | (src[src_pos+1] & 0xFF);
                unicode = search_char_code(gb_code, gbk_to_unicode, GBK_TO_UNICODE_TABLE_SIZE);
                src_pos_plus = 2;
                break;
                
            default:
                goto end;
        }

        // 根据目标编码转换Unicode
        switch (to) {
            case CODE_UTF8:
                char_len = _unicode_utf8(unicode, utf8_buf);
                if (dst_pos + char_len >= dst_size) { goto end; }
                memcpy(&dst[dst_pos], utf8_buf, char_len);
                dst_pos += char_len;
                break;
                
            case CODE_GB2312:
                gb_code = search_char_code(unicode, unicode_to_gb2312, UNICODE_TO_GB2312_TABLE_SIZE);
                if (dst_pos + 2 >= dst_size) { goto end; }
                dst[dst_pos++] = (gb_code >> 8) & 0xFF;
                dst[dst_pos++] = gb_code & 0xFF;
                break;
                
            case CODE_GBK:
                gb_code = search_char_code(unicode, unicode_to_gbk, UNICODE_TO_GBK_TABLE_SIZE);
                if (dst_pos + 2 >= dst_size) { goto end; }
                dst[dst_pos++] = (gb_code >> 8) & 0xFF;
                dst[dst_pos++] = gb_code & 0xFF;
                break;
                
            default:
                goto end;
        }

        src_pos += src_pos_plus;
    }

end:
    // 确保目标字符串以NULL结尾
    if (dst_pos < dst_size) {
        dst[dst_pos] = '\0';
    } else if (dst_size > 0) {
        dst[dst_size-1] = '\0';
    }

    return src_pos;
}
