/**
 * @file main.c
 * @brief 这是一个简易的例程兼测试用例
 * @details
 * 本文件必须使用utf8编码，否则测试用例字符串会出现编码转换错误
 * 编译命令：
 * gcc main.c str_code_cvt.c -O2 -g -o main.exe
 * windows的cmd或powershell一般默认为gbk编码，运行本例程打印转换后的utf8字符串时会出现乱码属于正常现象
 * linux的terminal一般默认为utf8编码，运行本例程打印转换后的GB2312或GBK字符串时会出现乱码属于正常现象
 * 
 * @copyright Copyright (c) 2025
 * 
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "str_code_cvt.h"

int main(int argc, char *argv[])
{
    int32_t result = 0;
    int32_t dts_limit_len = 0;
    char *str_gb2312 = "你好，世界123测  试";
    char *str_gbk = "你好，世界,123  ABC鼲 鼳 鼴 鼵 鼶 鼸 鼺 鼼 鼿 齀 齁 齂";

    char src[128] = {0};
    char dst[128] = {0};

    printf("测试UTF8与GB2312之间的转码\n");
    memset(dst, 0, sizeof(dst));
    result = string_code_convert(CODE_UTF8, CODE_GB2312, str_gb2312, dst, sizeof(dst));
    printf("result: %d, string: %s\n", result, dst);

    memset(src, 0, sizeof(src));
    result = string_code_convert(CODE_GB2312, CODE_UTF8, dst, src, sizeof(src));
    printf("result: %d, string: %s\n", result, src);

    if (strcmp(str_gb2312, src)) {
        printf("GB2312 convert error\n");
    }


    printf("测试UTF8与GBK之间的转码\n");
    memset(dst, 0, sizeof(dst));
    result = string_code_convert(CODE_UTF8, CODE_GBK, str_gbk, dst, sizeof(dst));
    printf("result: %d, string: %s\n", result, dst);

    memset(src, 0, sizeof(src));
    result = string_code_convert(CODE_GBK, CODE_UTF8, dst, src, sizeof(src));
    printf("result: %d, string: %s\n", result, src);

    if (strcmp(str_gbk, src)) {
        printf("GBK convert error\n");
    }


    strcpy(src, "ABC123 测试输入长度超过输出长度，字符串结尾不应出现乱码等情况\n");
    printf("%s", src);

    printf("测试UTF8转GB2312，输出长度不足的情况\n");
    for (dts_limit_len = 6; dts_limit_len < 13; dts_limit_len++) {
        // 填充字符"0"，方便观察和检测字符串是否被正常截断
        memset(dst, '0', sizeof(dst));
        result = string_code_convert(CODE_UTF8, CODE_GB2312, src, dst, dts_limit_len);
        printf("limit_len: %d, result: %d, string: %s\n", dts_limit_len, result, dst);
        if (strchr(dst, '0') || strlen(dst) >= dts_limit_len) {
            printf("over length test error\n");
        }
    }

    // 准备一个GB2312编码的字符串
    result = string_code_convert(CODE_UTF8, CODE_GB2312, src, dst, sizeof(dst));
    strcpy(src, dst);
    printf("测试GB2312转UTF8，输出长度不足的情况\n");
    for (dts_limit_len = 6; dts_limit_len < 15; dts_limit_len++) {
        // 填充字符"0"，方便观察和检测字符串是否被正常截断
        memset(dst, '0', sizeof(dst));
        result = string_code_convert(CODE_GB2312, CODE_UTF8, src, dst, dts_limit_len);
        printf("limit_len: %d, result: %d, string: %s\n", dts_limit_len, result, dst);
        if (strchr(dst, '0') || strlen(dst) >= dts_limit_len) {
            printf("over length test error\n");
        }
    }
}
