/**
 * @file    utf8_to_gb2312.h
 * @brief   UTF-8 到 GB2312 编码转换模块
 *
 * @details 使用编译进固件的 7445 条 Unicode→GB2312 排序映射表，
 *          通过二分查找将 UTF-8 中文字符转换为 GB2312 双字节编码，
 *          供 HZK16 字库查表使用。无需 TF 卡上的额外映射文件。
 */

#ifndef __UTF8_TO_GB2312_H__
#define __UTF8_TO_GB2312_H__

#include <stdint.h>

/**
 * @brief  初始化 UTF-8→GB2312 转换模块 (映射表已编译进固件，调用可选)
 * @return 0 成功
 */
int utf8_to_gb2312_init(void);

/**
 * @brief  将一个 UTF-8 编码的字符转换为 GB2312 编码
 * @param  utf8_char  UTF-8 编码的字符串 (指向字符起始字节)
 * @param  gb_out     输出: 2 字节 GB2312 编码 [高字节, 低字节]
 * @return 0 成功, -1 失败 (非中文字符或无映射)
 */
int utf8_char_to_gb2312(const char *utf8_char, uint8_t *gb_out);

/**
 * @brief  清理转换模块 (当前为空操作)
 */
void utf8_to_gb2312_deinit(void);

#endif /* __UTF8_TO_GB2312_H__ */
