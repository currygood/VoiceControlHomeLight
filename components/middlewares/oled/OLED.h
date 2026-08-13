/**
 * @file    OLED.h
 * @brief   OLED 显示屏驱动模块（SSD1306 128x64，I2C 接口）
 *
 * 功能概述：
 *   - I2C 通信初始化（基于 i2c_driver 模块）
 *   - 显存管理（整屏/区域清除、反色、写点、读点）
 *   - 字符/字符串显示（支持 6x8、8x16、12x24 三种字号，UTF-8/GB2312 中文）
 *   - 数值显示（十进制、十六进制、二进制、浮点数）
 *   - 图像显示（位图数据直接写入显存）
 *   - 几何绘图（点、线、矩形、三角形、圆、椭圆、圆弧）
 *   - 格式化输出（类 printf 风格）
 *
 * 硬件依赖：
 *   - SSD1306 驱动的 128x64 OLED 显示屏
 *   - I2C 总线（地址 0x3C）
 *   - 依赖 i2c_driver 模块提供 I2C 读写接口
 *
 * 使用流程：
 *   1. 初始化 I2C 总线
 *   2. 调用 OLED_Init() 初始化 OLED
 *   3. 调用显示/绘图函数写入显存
 *   4. 调用 OLED_Update() 或 OLED_UpdateArea() 刷新到屏幕
 */

#ifndef __OLED_H
#define __OLED_H

#include <stdint.h>
#include "OLED_Data.h"
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "i2c_driver.h"
#include "esp_log.h"


/* ======================== 参数宏定义 ======================== */

/**
 * @brief OLED I2C 设备地址（7 位地址，左对齐后为 0x78）
 */
#define OLED_ADDR 0x3C

/**
 * @brief 字体大小参数（用于 FontSize 参数）
 *
 * 取值说明：
 *   - OLED_6X8   6x8 点阵字体（ASCII 全半角）
 *   - OLED_8X16  8x16 点阵字体（ASCII 半角，中文 16x16）
 *   - OLED_12X24 12x24 点阵字体
 */
#define OLED_8X16       8
#define OLED_6X8        6
#define OLED_12X24      12

/**
 * @brief 填充模式参数（用于 IsFilled 参数）
 *
 * 取值说明：
 *   - OLED_UNFILLED  仅绘制轮廓
 *   - OLED_FILLED    填充内部
 */
#define OLED_UNFILLED   0
#define OLED_FILLED     1

/* ======================== 参数宏定义结束 ======================== */


/* ======================== 初始化函数 ======================== */

/**
 * @brief 初始化 OLED 显示屏
 *
 * 完成以下操作：
 *   1. 通过 I2c_Add_Device() 将 OLED 注册到 I2C 总线
 *   2. 发送 SSD1306 初始化命令序列（时钟、复用、对比度等）
 *   3. 清屏并刷新
 *
 * 调用前需已完成 I2C 总线初始化。
 *
 * @param bus_handle  I2C 总线句柄（由 I2c_Get_Global_Bus_Handle() 获取）
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t OLED_Init(i2c_master_bus_handle_t bus_handle);

/**
 * @brief 向 OLED 发送命令字节
 *
 * 通过 I2C 总线写入寄存器，控制字节为 0x00（Co=0, D/C#=0）。
 *
 * @param Command  要写入的命令值
 */
void OLED_WriteCommand(uint8_t Command);

/* ======================== 初始化函数结束 ======================== */


/* ======================== 更新函数 ======================== */

/**
 * @brief 将显存数据全部刷新到 OLED 屏幕
 *
 * 遍历 8 个页（Page 0~7），每页写入 128 字节数据。
 */
void OLED_Update(void);

/**
 * @brief 将显存指定区域刷新到 OLED 屏幕
 *
 * 仅刷新指定矩形区域内的页，减少 I2C 通信量，提升刷新速度。
 *
 * @param X       区域左上角 X 坐标（0~127）
 * @param Y       区域左上角 Y 坐标（0~63）
 * @param Width   区域宽度（像素）
 * @param Height  区域高度（像素）
 */
void OLED_UpdateArea(int16_t X, int16_t Y, uint8_t Width, uint8_t Height);

/* ======================== 更新函数结束 ======================== */


/* ======================== 显存控制函数 ======================== */

/**
 * @brief 清除显存（全屏填充 0x00）
 */
void OLED_Clear(void);

/**
 * @brief 清除显存指定区域
 *
 * @param X       区域左上角 X 坐标（0~127）
 * @param Y       区域左上角 Y 坐标（0~63）
 * @param Width   区域宽度（像素）
 * @param Height  区域高度（像素）
 */
void OLED_ClearArea(int16_t X, int16_t Y, uint8_t Width, uint8_t Height);

/**
 * @brief 显存全屏反色（按位取反）
 */
void OLED_Reverse(void);

/**
 * @brief 显存指定区域反色
 *
 * @param X       区域左上角 X 坐标（0~127）
 * @param Y       区域左上角 Y 坐标（0~63）
 * @param Width   区域宽度（像素）
 * @param Height  区域高度（像素）
 */
void OLED_ReverseArea(int16_t X, int16_t Y, uint8_t Width, uint8_t Height);

/* ======================== 显存控制函数结束 ======================== */


/* ======================== 显示函数 ======================== */

/**
 * @brief 在指定位置显示一个字符
 *
 * 根据 FontSize 自动选择对应的字模数据（OLED_F8x16 / OLED_F6x8 / OLED_F12x24）。
 * 字符索引基于 ASCII 空格（' '）偏移。
 *
 * @param X        字符左上角 X 坐标
 * @param Y        字符左上角 Y 坐标
 * @param Char     要显示的字符（ASCII 码）
 * @param FontSize 字体大小：OLED_6X8 / OLED_8X16 / OLED_12X24
 */
void OLED_ShowChar(int16_t X, int16_t Y, char Char, uint8_t FontSize);

/**
 * @brief 在指定位置显示字符串
 *
 * 支持 UTF-8 和 GB2312 编码的中文字符。
 * 对于 ASCII 字符，直接调用 OLED_ShowChar() 显示。
 * 对于多字节字符（中文），在字模库 OLED_CF16x16 中查找并显示。
 *
 * @param X        字符串左上角 X 坐标
 * @param Y        字符串左上角 Y 坐标
 * @param String   要显示的字符串（以 '\0' 结尾，支持 UTF-8/GB2312 编码）
 * @param FontSize 字体大小
 */
void OLED_ShowString(int16_t X, int16_t Y, char *String, uint8_t FontSize);

/**
 * @brief 显示无符号十进制数字
 *
 * 固定位数显示，不足位时高位不补零（直接显示数字字符）。
 *
 * @param X        左上角 X 坐标
 * @param Y        左上角 Y 坐标
 * @param Number   要显示的数字
 * @param Length   显示位数
 * @param FontSize 字体大小
 */
void OLED_ShowNum(int16_t X, int16_t Y, uint32_t Number, uint8_t Length, uint8_t FontSize);

/**
 * @brief 显示有符号十进制数字
 *
 * 在数字前显示 '+' 或 '-' 符号。
 *
 * @param X        左上角 X 坐标
 * @param Y        左上角 Y 坐标
 * @param Number   要显示的数字
 * @param Length   显示位数（不含符号位）
 * @param FontSize 字体大小
 */
void OLED_ShowSignedNum(int16_t X, int16_t Y, int32_t Number, uint8_t Length, uint8_t FontSize);

/**
 * @brief 显示十六进制数字
 *
 * @param X        左上角 X 坐标
 * @param Y        左上角 Y 坐标
 * @param Number   要显示的数字
 * @param Length   显示位数
 * @param FontSize 字体大小
 */
void OLED_ShowHexNum(int16_t X, int16_t Y, uint32_t Number, uint8_t Length, uint8_t FontSize);

/**
 * @brief 显示二进制数字
 *
 * @param X        左上角 X 坐标
 * @param Y        左上角 Y 坐标
 * @param Number   要显示的数字
 * @param Length   显示位数
 * @param FontSize 字体大小
 */
void OLED_ShowBinNum(int16_t X, int16_t Y, uint32_t Number, uint8_t Length, uint8_t FontSize);

/**
 * @brief 显示浮点数
 *
 * 格式：+/- 整数部分 . 小数部分
 *
 * @param X         左上角 X 坐标
 * @param Y         左上角 Y 坐标
 * @param Number    要显示的浮点数
 * @param IntLength 整数部分位数
 * @param FraLength 小数部分位数
 * @param FontSize  字体大小
 */
void OLED_ShowFloatNum(int16_t X, int16_t Y, double Number, uint8_t IntLength, uint8_t FraLength, uint8_t FontSize);

/**
 * @brief 显示图像（位图数据）
 *
 * 将位图数据写入显存指定区域。先清除目标区域，再逐字节写入。
 * 数据格式：纵向 8 点，高位在下，先从左到右再从上到下。
 *
 * @param X       图像左上角 X 坐标
 * @param Y       图像左上角 Y 坐标
 * @param Width   图像宽度（像素）
 * @param Height  图像高度（像素）
 * @param Image   图像字模数据指针
 */
void OLED_ShowImage(int16_t X, int16_t Y, uint8_t Width, uint8_t Height, const uint8_t *Image);

/**
 * @brief 格式化输出字符串（类 printf 风格）
 *
 * 内部使用 vsprintf() 格式化字符串，再调用 OLED_ShowString() 显示。
 * 注意：缓冲区大小限制为 256 字节。
 *
 * @param X        左上角 X 坐标
 * @param Y        左上角 Y 坐标
 * @param FontSize 字体大小
 * @param format   格式化字符串
 * @param ...      可变参数
 */
void OLED_Printf(int16_t X, int16_t Y, uint8_t FontSize, char *format, ...);

/* ======================== 显示函数结束 ======================== */


/* ======================== 绘图函数 ======================== */

/**
 * @brief 在指定坐标绘制一个点
 *
 * 将显存中对应位置的位置 1。
 *
 * @param X  X 坐标（0~127）
 * @param Y  Y 坐标（0~63）
 */
void OLED_DrawPoint(int16_t X, int16_t Y);

/**
 * @brief 获取指定坐标的像素状态
 *
 * @param X  X 坐标（0~127）
 * @param Y  Y 坐标（0~63）
 * @return 1 表示该像素点亮，0 表示熄灭
 */
uint8_t OLED_GetPoint(int16_t X, int16_t Y);

/**
 * @brief 绘制直线（Bresenham 算法）
 *
 * 支持任意方向的直线绘制。
 *
 * @param X0  起点 X 坐标
 * @param Y0  起点 Y 坐标
 * @param X1  终点 X 坐标
 * @param Y1  终点 Y 坐标
 */
void OLED_DrawLine(int16_t X0, int16_t Y0, int16_t X1, int16_t Y1);

/**
 * @brief 绘制矩形
 *
 * @param X        左上角 X 坐标
 * @param Y        左上角 Y 坐标
 * @param Width    宽度（像素）
 * @param Height   高度（像素）
 * @param IsFilled 填充模式：OLED_UNFILLED（仅边框）/ OLED_FILLED（填充）
 */
void OLED_DrawRectangle(int16_t X, int16_t Y, uint8_t Width, uint8_t Height, uint8_t IsFilled);

/**
 * @brief 绘制三角形
 *
 * 填充模式下使用射线法（PNPoly 算法）判断像素是否在三角形内部。
 *
 * @param X0  顶点 0 X 坐标
 * @param Y0  顶点 0 Y 坐标
 * @param X1  顶点 1 X 坐标
 * @param Y1  顶点 1 Y 坐标
 * @param X2  顶点 2 X 坐标
 * @param Y2  顶点 2 Y 坐标
 * @param IsFilled  填充模式
 */
void OLED_DrawTriangle(int16_t X0, int16_t Y0, int16_t X1, int16_t Y1, int16_t X2, int16_t Y2, uint8_t IsFilled);

/**
 * @brief 绘制圆（中点画圆算法）
 *
 * @param X        圆心 X 坐标
 * @param Y        圆心 Y 坐标
 * @param Radius   半径（像素）
 * @param IsFilled 填充模式
 */
void OLED_DrawCircle(int16_t X, int16_t Y, uint8_t Radius, uint8_t IsFilled);

/**
 * @brief 绘制椭圆（中点椭圆算法）
 *
 * @param X        椭圆中心 X 坐标
 * @param Y        椭圆中心 Y 坐标
 * @param A        半长轴（X 方向半径，像素）
 * @param B        半短轴（Y 方向半径，像素）
 * @param IsFilled 填充模式
 */
void OLED_DrawEllipse(int16_t X, int16_t Y, uint8_t A, uint8_t B, uint8_t IsFilled);

/**
 * @brief 绘制圆弧
 *
 * 基于中点画圆算法，通过角度判断是否绘制当前像素。
 *
 * @param X          圆心 X 坐标
 * @param Y          圆心 Y 坐标
 * @param Radius     半径（像素）
 * @param StartAngle 起始角度（度，0~360）
 * @param EndAngle   结束角度（度，0~360）
 * @param IsFilled   填充模式
 */
void OLED_DrawArc(int16_t X, int16_t Y, uint8_t Radius, int16_t StartAngle, int16_t EndAngle, uint8_t IsFilled);

/* ======================== 绘图函数结束 ======================== */

#endif