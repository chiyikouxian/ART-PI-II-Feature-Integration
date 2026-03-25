/**
 * @file    oled_font_hzk.h
 * @brief   HZK16 点阵字库模块 — 16×16 中文字模 (编译进固件, 存储在外部 Flash)
 */

#ifndef __OLED_FONT_HZK_H__
#define __OLED_FONT_HZK_H__

#include <stdint.h>

/**
 * @brief  初始化 HZK16 字库 (字库已编译进固件, 调用可选)
 * @return 0 成功
 */
int oled_hzk16_init(void);

/**
 * @brief  从 HZK16 字库读取一个汉字的字模数据
 * @param  qh   区码 (1~94, 即 GB2312 高字节 - 0xA0)
 * @param  wh   位码 (1~94, 即 GB2312 低字节 - 0xA0)
 * @param  buf  输出缓冲区, 32 字节, 格式已转为 OLED 纵向取模
 * @return 0 成功, -1 失败
 */
int oled_hzk16_get_char(uint8_t qh, uint8_t wh, uint8_t *buf);

/**
 * @brief  清理字库模块 (当前为空操作)
 */
void oled_hzk16_deinit(void);

#endif /* __OLED_FONT_HZK_H__ */
