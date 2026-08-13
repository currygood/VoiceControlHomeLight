/**
 * @file    OLED_Data.h
 * @brief   OLED 字模数据与图像数据声明
 *
 * 功能概述：
 *   - 定义字符集编码类型（UTF-8 / GB2312）
 *   - 定义中文字符字模单元结构体（ChineseCell_t）
 *   - 定义图像数据结构体（Image）
 *   - 声明 ASCII 字模数据（6x8、8x16、12x24 三种字号）
 *   - 声明中文字模数据（16x16 点阵）
 *   - 声明图像数据（WiFi 图标等）
 *
 * 字模数据存储格式：
 *   纵向 8 点，高位在下，先从左到右，再从上到下。
 *   每个 Bit 对应一个像素点（1 = 亮，0 = 灭）。
 */

#ifndef __OLED_DATA_H
#define __OLED_DATA_H

#include <stdint.h>

/* ======================== 字符集定义 ======================== */

/**
 * @brief 字符集编码选择
 *
 * 以下两个宏只能启用其中一个：
 *   - OLED_CHARSET_UTF8    使用 UTF-8 编码（中文 3 字节索引）
 *   - OLED_CHARSET_GB2312  使用 GB2312 编码（中文 2 字节索引）
 */
#define OLED_CHARSET_UTF8
//#define OLED_CHARSET_GB2312

/* ======================== 字符集定义结束 ======================== */


/* ======================== 字模基本单元 ======================== */

/**
 * @brief 中文字符字模单元
 *
 * 每个中文汉字对应一个字模单元，包含字符索引和 16x16 点阵数据。
 *
 * 索引字段说明：
 *   - UTF-8 模式：Index[5]（最长为 4 字节 UTF-8 编码 + '\0'）
 *   - GB2312 模式：Index[3]（2 字节编码 + '\0'）
 */
typedef struct
{
#ifdef OLED_CHARSET_UTF8
	char Index[5];          /**< 汉字索引（UTF-8 编码，最多 4 字节 + '\0'） */
#endif

#ifdef OLED_CHARSET_GB2312
	char Index[3];          /**< 汉字索引（GB2312 编码，2 字节 + '\0'） */
#endif

	uint8_t Data[32];       /**< 16x16 点阵字模数据（32 字节） */
} ChineseCell_t;

/* ======================== 字模基本单元结束 ======================== */


/* ======================== ASCII 字模数据声明 ======================== */

/**
 * @brief 8x16 点阵 ASCII 字模（每字符 16 字节）
 */
extern const uint8_t OLED_F8x16[][16];

/**
 * @brief 6x8 点阵 ASCII 字模（每字符 6 字节）
 */
extern const uint8_t OLED_F6x8[][6];

/**
 * @brief 12x24 点阵 ASCII 字模（每字符 36 字节）
 */
extern const uint8_t OLED_F12x24[][36];

/* ======================== ASCII 字模数据声明结束 ======================== */


/* ======================== 汉字字模数据声明 ======================== */

/**
 * @brief 16x16 点阵中文字模数组
 *
 * 数组以空 Index 的元素作为结束标记。
 */
extern const ChineseCell_t OLED_CF16x16[];

/* ======================== 汉字字模数据声明结束 ======================== */


/* ======================== 图像数据声明 ======================== */

/**
 * @brief 图像数据结构体
 *
 * 用于描述位图图像的基本属性，配合 OLED_ShowImage() 使用。
 */
typedef struct
{
	uint8_t width;          /**< 图像宽度（像素） */
	uint8_t height;         /**< 图像高度（像素） */
	const uint8_t *data;    /**< 图像字模数据指针 */
} Image;

/**
 * @brief WiFi 图标位图数据（13x9 像素）
 */
extern const uint8_t wifiData[];

/**
 * @brief WiFi 图标图像描述
 */
extern Image wifiImg;

/* ======================== 图像数据声明结束 ======================== */

#endif