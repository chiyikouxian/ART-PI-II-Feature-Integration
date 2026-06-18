#ifndef _STR_CODE_CVT_H
#define _STR_CODE_CVT_H

#include <stdint.h>

// 修改这里的设置，选择需要转换的字符编码
#define CVT_GB2312_TO_UTF8
#define CVT_GBK_TO_UTF8
#define CVT_UTF8_TO_GB2312
#define CVT_UTF8_TO_GBK

typedef enum {
    CODE_UNKNOWN = 0,
    CODE_UTF8    = 1 << 0,
    CODE_GB2312  = 1 << 1,
    CODE_GBK     = 1 << 2,
} code_type_e;

/**
 * @brief 转换字符串的编码格式
 * 若dst的长度不足以容纳src中转换后的字符，则字符串将会被截断，且会将字符串结尾替换为\0
 * 不支持原地转换字符编码，即src和dst不得指向同一个内存区域
 * 
 * @param from src的编码
 * @param to dst的编码
 * @param src 待转换的字符串
 * @param dst 转换后的字符串
 * @param dst_size dst的长度
 * @return int32_t 返回src中已转换的字节数
 */
int32_t string_code_convert(code_type_e from, code_type_e to, char *src, char *dst, uint32_t dst_size);

#endif
