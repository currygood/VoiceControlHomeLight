/**
 * @file    OLED.c
 * @brief   OLED 显示屏驱动实现（SSD1306 128x64，I2C 接口）
 *
 * 功能概述：
 *   - SSD1306 初始化（命令序列发送、I2C 设备注册）
 *   - 显存管理（OLED_DisplayBuf 读写、清屏、反色）
 *   - 屏幕刷新（整屏更新 / 区域更新）
 *   - 字符显示（ASCII 字符 / UTF-8 中文 / GB2312 中文）
 *   - 数值显示（十进制、十六进制、二进制、浮点数）
 *   - 图像显示（位图数据写入显存）
 *   - 几何绘图（点、线、矩形、三角形、圆、椭圆、圆弧）
 *   - 格式化输出（OLED_Printf 类 printf 风格）
 *
 * 核心设计：
 *   所有显示操作均写入显存数组 OLED_DisplayBuf[8][128]，
 *   调用 OLED_Update() / OLED_UpdateArea() 后才将显存发送到 OLED 硬件。
 *   显存组织：8 页（Page）× 128 列（Column），每页 8 行像素。
 *
 * 依赖：
 *   - i2c_driver 模块（I2C 读写接口）
 *   - OLED_Data.h（字模数据）
 *   - math.h（atan2 等数学函数）
 */

#include "OLED.h"
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "OLED";


/* ======================== 全局变量 ======================== */

/**
 * @brief OLED 显示状态标志位
 *
 * true 表示 OLED 已初始化并可显示，false 表示未初始化。
 */
static bool isOLEDShow = false;

/**
 * @brief OLED 显示界面状态
 *
 * 1 = 显示主界面，其他值保留扩展。
 */
static uint8_t OLED_ShowState = 1;

/**
 * @brief I2C 设备句柄
 *
 * 由 OLED_Init() 通过 I2c_Add_Device() 获取并保存。
 */
static i2c_master_dev_handle_t oled_dev = NULL;

/**
 * @brief OLED 显存数组
 *
 * 所有显示函数都只对此显存数组进行读写操作。
 * 随后调用 OLED_Update() 或 OLED_UpdateArea() 才会将显存数据发送到 OLED 硬件。
 *
 * 显存组织：8 页（Page 0~7）× 128 列（Column 0~127）。
 * 每页 8 行像素，每字节的 bit 0 对应最上方像素。
 */
uint8_t OLED_DisplayBuf[8][128];

/* ======================== 全局变量结束 ======================== */


/* ======================== 通信协议 ======================== */

/**
 * @brief OLED 写命令
 *
 * 通过 I2C 总线向 SSD1306 发送命令字节。
 * 控制字节 0x00：Co=0（后续字节均为命令），D/C#=0（数据/命令选择为命令）。
 *
 * @param Command  要写入的命令值（SSD1306 命令码）
 */
void OLED_WriteCommand(uint8_t Command)
{
    esp_err_t ret = I2c_Write_Reg(oled_dev, 0x00, Command);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "WriteCommand 0x%02X failed: %s", Command, esp_err_to_name(ret));
    }
}

/**
 * @brief OLED 写数据
 *
 * 通过 I2C 总线向 SSD1306 GDDRAM 写入多个数据字节。
 * 控制字节 0x40：Co=0（后续字节均为数据），D/C#=1（数据/命令选择为数据）。
 *
 * @param Data   数据缓冲区起始地址
 * @param Count  要写入的字节数
 */
void OLED_WriteData(uint8_t *Data, uint8_t Count)
{
    /*
       方案 A: 使用 I2c_Write_Bytes 封装函数
    */
    esp_err_t ret = I2c_Write_Bytes(oled_dev, 0x40, Data, Count);

    /*
       方案 B: 直接使用 i2c_master_transmit
       uint8_t buf[129];
       buf[0] = 0x40;
       memcpy(&buf[1], Data, Count);
       esp_err_t ret = i2c_master_transmit(oled_dev, buf, Count + 1, -1);
    */

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "WriteData failed: %s", esp_err_to_name(ret));
    }
}

/* ======================== 通信协议结束 ======================== */

/* ======================== 硬件配置 ======================== */

/**
 * @brief OLED 初始化
 *
 * 完成以下操作：
 *   1. 调用 I2c_Add_Device() 将 OLED 注册到 I2C 总线
 *   2. 等待 100ms 确保 OLED 上电稳定
 *   3. 发送 SSD1306 初始化命令序列
 *   4. 清屏并刷新
 *
 * SSD1306 初始化命令序列说明：
 *   - 0xAE：关闭显示
 *   - 0xD5+0x80：设置时钟分频因子和振荡器频率
 *   - 0xA8+0x3F：设置多路复用率（64 路）
 *   - 0xD3+0x00：设置显示偏移
 *   - 0x40：设置显示起始行
 *   - 0xA1：段重映射（左右翻转）
 *   - 0xC8：COM 扫描方向（上下翻转）
 *   - 0xDA+0x12：COM 引脚硬件配置
 *   - 0x81+0xCF：设置对比度
 *   - 0xD9+0xF1：设置预充电周期
 *   - 0xDB+0x30：设置 VCOMH 电压
 *   - 0xA4：正常显示（非全屏点亮）
 *   - 0xA6：正常显示（非反色）
 *   - 0x8D+0x14：启用电荷泵
 *   - 0xAF：打开显示
 *
 * @param bus_handle  I2C 总线句柄
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t OLED_Init(i2c_master_bus_handle_t bus_handle)
{
    if (bus_handle == NULL) return ESP_ERR_INVALID_ARG;

    /* 步骤 1：注册设备到 I2C 总线 */
    esp_err_t ret = I2c_Add_Device(bus_handle, OLED_ADDR, I2C_FREQ, &oled_dev);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to add OLED device");
        return ret;
    }

    /* 步骤 2：上电稳定延时 */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* 步骤 3：发送初始化命令序列 */
    OLED_WriteCommand(0xAE);    /* 关闭显示 */
    OLED_WriteCommand(0xD5);    /* 设置时钟分频因子 */
    OLED_WriteCommand(0x80);
    OLED_WriteCommand(0xA8);    /* 设置多路复用率 */
    OLED_WriteCommand(0x3F);
    OLED_WriteCommand(0xD3);    /* 设置显示偏移 */
    OLED_WriteCommand(0x00);
    OLED_WriteCommand(0x40);    /* 设置显示起始行 */
    OLED_WriteCommand(0xA1);    /* 段重映射（左右翻转） */
    OLED_WriteCommand(0xC8);    /* COM 扫描方向（上下翻转） */
    OLED_WriteCommand(0xDA);    /* COM 引脚硬件配置 */
    OLED_WriteCommand(0x12);
    OLED_WriteCommand(0x81);    /* 设置对比度 */
    OLED_WriteCommand(0xCF);
    OLED_WriteCommand(0xD9);    /* 设置预充电周期 */
    OLED_WriteCommand(0xF1);
    OLED_WriteCommand(0xDB);    /* 设置 VCOMH 电压 */
    OLED_WriteCommand(0x30);
    OLED_WriteCommand(0xA4);    /* 正常显示（非全屏点亮） */
    OLED_WriteCommand(0xA6);    /* 正常显示（非反色） */
    OLED_WriteCommand(0x8D);    /* 启用电荷泵 */
    OLED_WriteCommand(0x14);
    OLED_WriteCommand(0xAF);    /* 打开显示 */

    /* 步骤 4：清屏并刷新 */
    OLED_Clear();
    OLED_Update();

    return ESP_OK;
}

/**
 * @brief 设置 OLED 显示光标位置
 *
 * 通过设置页地址和列地址来定位 GDDRAM 中的写入位置。
 *
 * 页地址：0xB0 | Page（Page 0~7）
 * 列地址：分两次发送，高 4 位（0x10 | (X>>4)）和低 4 位（0x00 | (X&0x0F)）
 *
 * 注意：若使用 1.3 寸 OLED（SH1106 驱动，132 列），需将 X += 2 的注释解除。
 *
 * @param Page  页地址（0~7），每页对应 8 行像素
 * @param X     列地址（0~127），128 列对应 128 像素宽
 */
void OLED_SetCursor(uint8_t Page, uint8_t X)
{
//  X += 2;   /* 1.3 寸 SH1106 偏移修正 */
    OLED_WriteCommand(0xB0 | Page);                 /* 设置页地址 */
    OLED_WriteCommand(0x10 | ((X & 0xF0) >> 4));    /* 设置列地址高 4 位 */
    OLED_WriteCommand(0x00 | (X & 0x0F));            /* 设置列地址低 4 位 */
}

/* ======================== 硬件配置结束 ======================== */


/* ======================== 工具函数 ======================== */

/**
 * @brief 幂运算（整数）
 *
 * 计算 X^Y，用于数值显示时提取各位数字。
 *
 * @param X  底数
 * @param Y  指数
 * @return X 的 Y 次幂
 */
uint32_t OLED_Pow(uint32_t X, uint32_t Y)
{
    uint32_t Result = 1;
    while (Y --)
    {
        Result *= X;
    }
    return Result;
}

/**
 * @brief 射线法判断点是否在多边形内部（PNPoly 算法）
 *
 * 用于填充模式下判断像素是否在三角形内部。
 *
 * @param nvert  多边形顶点数
 * @param vertx  顶点 X 坐标数组
 * @param verty  顶点 Y 坐标数组
 * @param testx  测试点 X 坐标
 * @param testy  测试点 Y 坐标
 * @return 1 表示点在多边形内部，0 表示在外部
 */
uint8_t OLED_pnpoly(uint8_t nvert, int16_t *vertx, int16_t *verty, int16_t testx, int16_t testy)
{
    int16_t i, j, c = 0;
    for (i = 0, j = nvert - 1; i < nvert; j = i++)
    {
        if (((verty[i] > testy) != (verty[j] > testy)) &&
            (testx < (vertx[j] - vertx[i]) * (testy - verty[i]) / (verty[j] - verty[i]) + vertx[i]))
        {
            c = !c;
        }
    }
    return c;
}

/**
 * @brief 判断点是否在指定角度范围内
 *
 * 使用 atan2 计算点相对于圆心的角度，判断是否在 [StartAngle, EndAngle] 范围内。
 * 支持跨越 0 度的角度范围（如 StartAngle=350, EndAngle=10）。
 *
 * @param X          点相对于圆心的 X 坐标
 * @param Y          点相对于圆心的 Y 坐标
 * @param StartAngle 起始角度（度）
 * @param EndAngle   结束角度（度）
 * @return 1 表示在角度范围内，0 表示不在
 */
uint8_t OLED_IsInAngle(int16_t X, int16_t Y, int16_t StartAngle, int16_t EndAngle)
{
    int16_t PointAngle;
    PointAngle = atan2(Y, X) / 3.14 * 180;
    if (StartAngle < EndAngle)
    {
        if (PointAngle >= StartAngle && PointAngle <= EndAngle)
        {
            return 1;
        }
    }
    else
    {
        if (PointAngle >= StartAngle || PointAngle <= EndAngle)
        {
            return 1;
        }
    }
    return 0;
}

/* ======================== 工具函数结束 ======================== */


/* ======================== 功能函数 ======================== */

/* ======================== 更新函数 ======================== */

/**
 * @brief 将显存全部刷新到 OLED 屏幕
 *
 * 遍历 8 个页，每页写入 128 字节显存数据。
 */
void OLED_Update(void)
{
    uint8_t j;
    for (j = 0; j < 8; j ++)
    {
        OLED_SetCursor(j, 0);
        OLED_WriteData(OLED_DisplayBuf[j], 128);
    }
}

/**
 * @brief 将显存指定区域刷新到 OLED 屏幕
 *
 * 仅刷新覆盖的页，减少 I2C 通信量。
 *
 * @param X       区域左上角 X 坐标
 * @param Y       区域左上角 Y 坐标
 * @param Width   区域宽度
 * @param Height  区域高度
 */
void OLED_UpdateArea(int16_t X, int16_t Y, uint8_t Width, uint8_t Height)
{
    int16_t j;
    int16_t Page, Page1;

    Page = Y / 8;
    Page1 = (Y + Height - 1) / 8 + 1;
    if (Y < 0)
    {
        Page -= 1;
        Page1 -= 1;
    }

    for (j = Page; j < Page1; j ++)
    {
        if (X >= 0 && X <= 127 && j >= 0 && j <= 7)
        {
            OLED_SetCursor(j, X);
            OLED_WriteData(&OLED_DisplayBuf[j][X], Width);
        }
    }
}

/* ======================== 更新函数结束 ======================== */


/* ======================== 显存控制函数 ======================== */

/**
 * @brief 清除显存（全屏填充 0x00）
 */
void OLED_Clear(void)
{
    uint8_t i, j;
    for (j = 0; j < 8; j ++)
    {
        for (i = 0; i < 128; i ++)
        {
            OLED_DisplayBuf[j][i] = 0x00;
        }
    }
}

/**
 * @brief 清除显存指定区域
 *
 * @param X       区域左上角 X 坐标
 * @param Y       区域左上角 Y 坐标
 * @param Width   区域宽度
 * @param Height  区域高度
 */
void OLED_ClearArea(int16_t X, int16_t Y, uint8_t Width, uint8_t Height)
{
    int16_t i, j;
    for (j = Y; j < Y + Height; j ++)
    {
        for (i = X; i < X + Width; i ++)
        {
            if (i >= 0 && i <= 127 && j >= 0 && j <= 63)
            {
                OLED_DisplayBuf[j / 8][i] &= ~(0x01 << (j % 8));
            }
        }
    }
}

/**
 * @brief 显存全屏反色（按位取反）
 */
void OLED_Reverse(void)
{
    uint8_t i, j;
    for (j = 0; j < 8; j ++)
    {
        for (i = 0; i < 128; i ++)
        {
            OLED_DisplayBuf[j][i] ^= 0xFF;
        }
    }
}

/**
 * @brief 显存指定区域反色
 *
 * @param X       区域左上角 X 坐标
 * @param Y       区域左上角 Y 坐标
 * @param Width   区域宽度
 * @param Height  区域高度
 */
void OLED_ReverseArea(int16_t X, int16_t Y, uint8_t Width, uint8_t Height)
{
    int16_t i, j;
    for (j = Y; j < Y + Height; j ++)
    {
        for (i = X; i < X + Width; i ++)
        {
            if (i >= 0 && i <= 127 && j >= 0 && j <= 63)
            {
                OLED_DisplayBuf[j / 8][i] ^= 0x01 << (j % 8);
            }
        }
    }
}

/* ======================== 显存控制函数结束 ======================== */


/* ======================== 显示函数 ======================== */

/**
 * @brief 在指定位置显示一个字符
 *
 * 根据 FontSize 选择对应的字模数据并通过 OLED_ShowImage() 显示。
 * 字符索引 = Char - ' '（ASCII 空格偏移）。
 *
 * @param X        字符左上角 X 坐标
 * @param Y        字符左上角 Y 坐标
 * @param Char     要显示的字符（ASCII 码）
 * @param FontSize 字体大小
 */
void OLED_ShowChar(int16_t X, int16_t Y, char Char, uint8_t FontSize)
{
    if (FontSize == OLED_8X16)
    {
        OLED_ShowImage(X, Y, 8, 16, OLED_F8x16[Char - ' ']);
    }
    else if (FontSize == OLED_6X8)
    {
        OLED_ShowImage(X, Y, 6, 8, OLED_F6x8[Char - ' ']);
    }
	else if(FontSize == OLED_12X24)
	{
		OLED_ShowImage(X, Y, 12, 24, OLED_F12x24[Char - ' ']);
	}
}

/**
 * @brief 在指定位置显示字符串
 *
 * 支持 UTF-8 和 GB2312 编码。
 *
 * 处理流程：
 *   1. 遍历字符串，根据编码格式提取单个字符
 *   2. 对于单字节字符（ASCII），直接调用 OLED_ShowChar() 显示
 *   3. 对于多字节字符（中文），在字模库 OLED_CF16x16 中查找匹配的字模
 *   4. 找到后以 16x16 图像格式显示，未找到则显示 '?'
 *
 * UTF-8 编码识别规则：
 *   - 0xxxxxxx（1 字节 ASCII）
 *   - 110xxxxx（2 字节字符）
 *   - 1110xxxx（3 字节字符，常见中文）
 *   - 11110xxx（4 字节字符）
 *
 * @param X        字符串左上角 X 坐标
 * @param Y        字符串左上角 Y 坐标
 * @param String   要显示的字符串（以 '\0' 结尾）
 * @param FontSize 字体大小
 */
void OLED_ShowString(int16_t X, int16_t Y, char *String, uint8_t FontSize)
{
	uint16_t i = 0;
	char SingleChar[5];
	uint8_t CharLength = 0;
	uint16_t XOffset = 0;
	uint16_t pIndex;
	
	while (String[i] != '\0')	//遍历字符串
	{
		
#ifdef OLED_CHARSET_UTF8						//定义字符集为UTF8
		/*此段代码的目的是，提取UTF8字符串中的一个字符，转存到SingleChar子字符串中*/
		/*判断UTF8编码第一个字节的标志位*/
		if ((String[i] & 0x80) == 0x00)			//第一个字节为0xxxxxxx
		{
			CharLength = 1;						//字符为1字节
			SingleChar[0] = String[i ++];		//将第一个字节写入SingleChar第0个位置，随后i指向下一个字节
			SingleChar[1] = '\0';				//为SingleChar添加字符串结束标志位
		}
		else if ((String[i] & 0xE0) == 0xC0)	//第一个字节为110xxxxx
		{
			CharLength = 2;						//字符为2字节
			SingleChar[0] = String[i ++];		//将第一个字节写入SingleChar第0个位置，随后i指向下一个字节
			if (String[i] == '\0') {break;}		//意外情况，跳出循环，结束显示
			SingleChar[1] = String[i ++];		//将第二个字节写入SingleChar第1个位置，随后i指向下一个字节
			SingleChar[2] = '\0';				//为SingleChar添加字符串结束标志位
		}
		else if ((String[i] & 0xF0) == 0xE0)	//第一个字节为1110xxxx
		{
			CharLength = 3;						//字符为3字节
			SingleChar[0] = String[i ++];
			if (String[i] == '\0') {break;}
			SingleChar[1] = String[i ++];
			if (String[i] == '\0') {break;}
			SingleChar[2] = String[i ++];
			SingleChar[3] = '\0';
		}
		else if ((String[i] & 0xF8) == 0xF0)	//第一个字节为11110xxx
		{
			CharLength = 4;						//字符为4字节
			SingleChar[0] = String[i ++];
			if (String[i] == '\0') {break;}
			SingleChar[1] = String[i ++];
			if (String[i] == '\0') {break;}
			SingleChar[2] = String[i ++];
			if (String[i] == '\0') {break;}
			SingleChar[3] = String[i ++];
			SingleChar[4] = '\0';
		}
		else
		{
			i ++;			//意外情况，i指向下一个字节，忽略此字节，继续判断下一个字节
			continue;
		}
#endif
		
#ifdef OLED_CHARSET_GB2312						//定义字符集为GB2312
		/*此段代码的目的是，提取GB2312字符串中的一个字符，转存到SingleChar子字符串中*/
		/*判断GB2312字节的最高位标志位*/
		if ((String[i] & 0x80) == 0x00)			//最高位为0
		{
			CharLength = 1;						//字符为1字节
			SingleChar[0] = String[i ++];		//将第一个字节写入SingleChar第0个位置，随后i指向下一个字节
			SingleChar[1] = '\0';				//为SingleChar添加字符串结束标志位
		}
		else									//最高位为1
		{
			CharLength = 2;						//字符为2字节
			SingleChar[0] = String[i ++];		//将第一个字节写入SingleChar第0个位置，随后i指向下一个字节
			if (String[i] == '\0') {break;}		//意外情况，跳出循环，结束显示
			SingleChar[1] = String[i ++];		//将第二个字节写入SingleChar第1个位置，随后i指向下一个字节
			SingleChar[2] = '\0';				//为SingleChar添加字符串结束标志位
		}
#endif
		
		/*显示上述代码提取到的SingleChar*/
		if (CharLength == 1)	//如果是单字节字符
		{
			/*使用OLED_ShowChar显示此字符*/
			OLED_ShowChar(X + XOffset, Y, SingleChar[0], FontSize);
			XOffset += FontSize;
		}
		else					//否则，即多字节字符（常见的就是中文）
		{
			/*遍历整个字模库，从字模库中寻找此字符的数据*/
			/*如果找到最后一个字符（定义为空字符串），则表示字符未在字模库定义，停止寻找*/
			for (pIndex = 0; strcmp(OLED_CF16x16[pIndex].Index, "") != 0; pIndex ++)
			{
				/*找到匹配的字符*/
				if (strcmp(OLED_CF16x16[pIndex].Index, SingleChar) == 0)
				{
					break;		//跳出循环，此时pIndex的值为指定字符的索引
				}
			}
			if (FontSize == OLED_8X16)		//给定字体为8*16点阵
			{
				/*将字模库OLED_CF16x16的指定数据以16*16的图像格式显示*/
				OLED_ShowImage(X + XOffset, Y, 16, 16, OLED_CF16x16[pIndex].Data);
				XOffset += 16;
			}
			else if (FontSize == OLED_6X8)	//给定字体为6*8点阵
			{
				/*空间不足，此位置显示'?'*/
				OLED_ShowChar(X + XOffset, Y, '?', OLED_6X8);
				XOffset += OLED_6X8;
			}
		}
	}
}

/**
 * @brief 显示无符号十进制数字
 *
 * 固定位数显示，从高位到低位逐位显示。
 *
 * @param X        左上角 X 坐标
 * @param Y        左上角 Y 坐标
 * @param Number   要显示的数字
 * @param Length   显示位数
 * @param FontSize 字体大小
 */
void OLED_ShowNum(int16_t X, int16_t Y, uint32_t Number, uint8_t Length, uint8_t FontSize)
{
    uint8_t i;
    for (i = 0; i < Length; i++)
    {
        OLED_ShowChar(X + i * FontSize, Y, Number / OLED_Pow(10, Length - i - 1) % 10 + '0', FontSize);
    }
}

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
void OLED_ShowSignedNum(int16_t X, int16_t Y, int32_t Number, uint8_t Length, uint8_t FontSize)
{
    uint8_t i;
    uint32_t Number1;

    if (Number >= 0)
    {
        OLED_ShowChar(X, Y, '+', FontSize);
        Number1 = Number;
    }
    else
    {
        OLED_ShowChar(X, Y, '-', FontSize);
        Number1 = -Number;
    }

    for (i = 0; i < Length; i++)
    {
        OLED_ShowChar(X + (i + 1) * FontSize, Y, Number1 / OLED_Pow(10, Length - i - 1) % 10 + '0', FontSize);
    }
}

/**
 * @brief 显示十六进制数字
 *
 * @param X        左上角 X 坐标
 * @param Y        左上角 Y 坐标
 * @param Number   要显示的数字
 * @param Length   显示位数
 * @param FontSize 字体大小
 */
void OLED_ShowHexNum(int16_t X, int16_t Y, uint32_t Number, uint8_t Length, uint8_t FontSize)
{
    uint8_t i, SingleNumber;
    for (i = 0; i < Length; i++)
    {
        SingleNumber = Number / OLED_Pow(16, Length - i - 1) % 16;
        if (SingleNumber < 10)
        {
            OLED_ShowChar(X + i * FontSize, Y, SingleNumber + '0', FontSize);
        }
        else
        {
            OLED_ShowChar(X + i * FontSize, Y, SingleNumber - 10 + 'A', FontSize);
        }
    }
}

/**
 * @brief 显示二进制数字
 *
 * @param X        左上角 X 坐标
 * @param Y        左上角 Y 坐标
 * @param Number   要显示的数字
 * @param Length   显示位数
 * @param FontSize 字体大小
 */
void OLED_ShowBinNum(int16_t X, int16_t Y, uint32_t Number, uint8_t Length, uint8_t FontSize)
{
    uint8_t i;
    for (i = 0; i < Length; i++)
    {
        OLED_ShowChar(X + i * FontSize, Y, Number / OLED_Pow(2, Length - i - 1) % 2 + '0', FontSize);
    }
}

/**
 * @brief 显示浮点数
 *
 * 格式：+/- 整数部分 . 小数部分
 *
 * 处理流程：
 *   1. 显示符号位
 *   2. 分离整数部分和小数部分
 *   3. 四舍五入处理小数进位
 *   4. 分别显示整数部分和小数部分
 *
 * @param X         左上角 X 坐标
 * @param Y         左上角 Y 坐标
 * @param Number    要显示的浮点数
 * @param IntLength 整数部分位数
 * @param FraLength 小数部分位数
 * @param FontSize  字体大小
 */
void OLED_ShowFloatNum(int16_t X, int16_t Y, double Number, uint8_t IntLength, uint8_t FraLength, uint8_t FontSize)
{
    uint32_t PowNum, IntNum, FraNum;

    if (Number >= 0)
    {
        OLED_ShowChar(X, Y, '+', FontSize);
    }
    else
    {
        OLED_ShowChar(X, Y, '-', FontSize);
        Number = -Number;
    }

    IntNum = Number;
    Number -= IntNum;
    PowNum = OLED_Pow(10, FraLength);
    FraNum = round(Number * PowNum);
    IntNum += FraNum / PowNum;

    OLED_ShowNum(X + FontSize, Y, IntNum, IntLength, FontSize);
    OLED_ShowChar(X + (IntLength + 1) * FontSize, Y, '.', FontSize);
    OLED_ShowNum(X + (IntLength + 2) * FontSize, Y, FraNum, FraLength, FontSize);
}

/**
 * @brief 显示图像（位图数据）
 *
 * 将位图数据写入显存指定区域。
 *
 * 处理流程：
 *   1. 先清除目标区域，防止叠加乱码
 *   2. 遍历图像数据的每一个 8 像素垂直切片
 *   3. 将每个字节的 8 个 bit 拆分映射到显存的对应 Page 中
 *   4. 进行横向和纵向越界裁剪
 *
 * 数据格式：纵向 8 点，高位在下，先从左到右再从上到下。
 *
 * @param X       图像左上角 X 坐标
 * @param Y       图像左上角 Y 坐标
 * @param Width   图像宽度（像素）
 * @param Height  图像高度（像素）
 * @param Image   图像字模数据指针
 */
void OLED_ShowImage(int16_t X, int16_t Y, uint8_t Width, uint8_t Height, const uint8_t *Image)
{
    int16_t i, j;
    uint8_t ImageRowHeight = (Height - 1) / 8 + 1;  /* 图像占用的页数（源数据） */

    /* 步骤 1：先清除指定区域，防止叠加乱码 */
    OLED_ClearArea(X, Y, Width, Height);

    /* 步骤 2：遍历图像数据的每一个 8 像素垂直切片 */
    for (j = 0; j < ImageRowHeight; j++)
    {
        for (i = 0; i < Width; i++)
        {
            /* 当前处理的显存横坐标 */
            int16_t CurrentX = X + i;
            if (CurrentX < 0 || CurrentX > 127) continue;  /* 横向越界裁剪 */

            /* 取出源数据中的一字节（8 个垂直像素） */
            uint8_t Data = Image[j * Width + i];

            /* 计算这一字节数据在屏幕上的实际像素起始 Y 坐标 */
            int16_t RealY = Y + (j * 8);

            /* 将 8 位数据拆分映射到显存的 Page 中 */
            for (uint8_t bit = 0; bit < 8; bit++)
            {
                /* 检查当前位是否超出图像实际高度 */
                if ((j * 8 + bit) >= Height) break;

                /* 计算当前像素点在屏幕上的绝对 Y 坐标 */
                int16_t TargetY = RealY + bit;

                if (TargetY < 0 || TargetY > 63) continue;  /* 纵向越界裁剪 */

                /* 写入显存（bit=1 则置位） */
                if (Data & (0x01 << bit))
                {
                    OLED_DisplayBuf[TargetY / 8][CurrentX] |= (0x01 << (TargetY % 8));
                }
            }
        }
    }
}

/**
 * @brief 格式化输出字符串（类 printf 风格）
 *
 * 内部使用 vsprintf() 格式化字符串，再调用 OLED_ShowString() 显示。
 * 注意：缓冲区大小限制为 256 字节，超长字符串会被截断。
 *
 * @param X        左上角 X 坐标
 * @param Y        左上角 Y 坐标
 * @param FontSize 字体大小
 * @param format   格式化字符串
 * @param ...      可变参数
 */
void OLED_Printf(int16_t X, int16_t Y, uint8_t FontSize, char *format, ...)
{
    char String[256];
    va_list arg;
    va_start(arg, format);
    vsprintf(String, format, arg);
    va_end(arg);
    OLED_ShowString(X, Y, String, FontSize);
}

/* ======================== 显示函数结束 ======================== */


/* ======================== 绘图函数 ======================== */

/**
 * @brief 在指定坐标绘制一个点
 *
 * 将显存中对应位置的位置 1。坐标越界时自动忽略。
 *
 * @param X  X 坐标（0~127）
 * @param Y  Y 坐标（0~63）
 */
void OLED_DrawPoint(int16_t X, int16_t Y)
{
    if (X >= 0 && X <= 127 && Y >= 0 && Y <= 63)
    {
        OLED_DisplayBuf[Y / 8][X] |= 0x01 << (Y % 8);
    }
}

/**
 * @brief 获取指定坐标的像素状态
 *
 * @param X  X 坐标（0~127）
 * @param Y  Y 坐标（0~63）
 * @return 1 表示该像素点亮，0 表示熄灭
 */
uint8_t OLED_GetPoint(int16_t X, int16_t Y)
{
    if (X >= 0 && X <= 127 && Y >= 0 && Y <= 63)
    {
        if (OLED_DisplayBuf[Y / 8][X] & 0x01 << (Y % 8))
        {
            return 1;
        }
    }
    return 0;
}

/**
 * @brief 绘制直线（Bresenham 算法）
 *
 * 支持水平线、垂直线和任意斜率的直线。
 *
 * 处理流程：
 *   - 水平线/垂直线：直接逐点绘制
 *   - 斜线：使用 Bresenham 中点画线算法，通过坐标变换处理八分圆对称
 *
 * @param X0  起点 X 坐标
 * @param Y0  起点 Y 坐标
 * @param X1  终点 X 坐标
 * @param Y1  终点 Y 坐标
 */
void OLED_DrawLine(int16_t X0, int16_t Y0, int16_t X1, int16_t Y1)
{
    int16_t x, y, dx, dy, d, incrE, incrNE, temp;
    int16_t x0 = X0, y0 = Y0, x1 = X1, y1 = Y1;
    uint8_t yflag = 0, xyflag = 0;

    if (y0 == y1)
    {
        if (x0 > x1) {temp = x0; x0 = x1; x1 = temp;}
        for (x = x0; x <= x1; x ++)
        {
            OLED_DrawPoint(x, y0);
        }
    }
    else if (x0 == x1)
    {
        if (y0 > y1) {temp = y0; y0 = y1; y1 = temp;}
        for (y = y0; y <= y1; y ++)
        {
            OLED_DrawPoint(x0, y);
        }
    }
    else
    {
        if (x0 > x1)
        {
            temp = x0; x0 = x1; x1 = temp;
            temp = y0; y0 = y1; y1 = temp;
        }

        if (y0 > y1)
        {
            y0 = -y0;
            y1 = -y1;
            yflag = 1;
        }

        if (y1 - y0 > x1 - x0)
        {
            temp = x0; x0 = y0; y0 = temp;
            temp = x1; x1 = y1; y1 = temp;
            xyflag = 1;
        }

        dx = x1 - x0;
        dy = y1 - y0;
        incrE = 2 * dy;
        incrNE = 2 * (dy - dx);
        d = 2 * dy - dx;
        x = x0;
        y = y0;

        if (yflag && xyflag)        {OLED_DrawPoint(y, -x);}
        else if (yflag)             {OLED_DrawPoint(x, -y);}
        else if (xyflag)            {OLED_DrawPoint(y, x);}
        else                        {OLED_DrawPoint(x, y);}

        while (x < x1)
        {
            x ++;
            if (d < 0)
            {
                d += incrE;
            }
            else
            {
                y ++;
                d += incrNE;
            }

            if (yflag && xyflag)    {OLED_DrawPoint(y, -x);}
            else if (yflag)         {OLED_DrawPoint(x, -y);}
            else if (xyflag)        {OLED_DrawPoint(y, x);}
            else                    {OLED_DrawPoint(x, y);}
        }
    }
}

/**
 * @brief 绘制矩形
 *
 * @param X        左上角 X 坐标
 * @param Y        左上角 Y 坐标
 * @param Width    宽度（像素）
 * @param Height   高度（像素）
 * @param IsFilled 填充模式：OLED_UNFILLED（仅边框）/ OLED_FILLED（填充）
 */
void OLED_DrawRectangle(int16_t X, int16_t Y, uint8_t Width, uint8_t Height, uint8_t IsFilled)
{
    int16_t i, j;
    if (!IsFilled)
    {
        for (i = X; i < X + Width; i ++)
        {
            OLED_DrawPoint(i, Y);
            OLED_DrawPoint(i, Y + Height - 1);
        }
        for (i = Y; i < Y + Height; i ++)
        {
            OLED_DrawPoint(X, i);
            OLED_DrawPoint(X + Width - 1, i);
        }
    }
    else
    {
        for (i = X; i < X + Width; i ++)
        {
            for (j = Y; j < Y + Height; j ++)
            {
                OLED_DrawPoint(i, j);
            }
        }
    }
}

/**
 * @brief 绘制三角形
 *
 * 非填充模式：绘制三条边。
 * 填充模式：计算包围盒，使用射线法（PNPoly 算法）逐点判断是否在三角形内部。
 *
 * @param X0  顶点 0 X 坐标
 * @param Y0  顶点 0 Y 坐标
 * @param X1  顶点 1 X 坐标
 * @param Y1  顶点 1 Y 坐标
 * @param X2  顶点 2 X 坐标
 * @param Y2  顶点 2 Y 坐标
 * @param IsFilled  填充模式
 */
void OLED_DrawTriangle(int16_t X0, int16_t Y0, int16_t X1, int16_t Y1, int16_t X2, int16_t Y2, uint8_t IsFilled)
{
    int16_t minx = X0, miny = Y0, maxx = X0, maxy = Y0;
    int16_t i, j;
    int16_t vx[] = {X0, X1, X2};
    int16_t vy[] = {Y0, Y1, Y2};

    if (!IsFilled)
    {
        OLED_DrawLine(X0, Y0, X1, Y1);
        OLED_DrawLine(X0, Y0, X2, Y2);
        OLED_DrawLine(X1, Y1, X2, Y2);
    }
    else
    {
        if (X1 < minx) {minx = X1;}
        if (X2 < minx) {minx = X2;}
        if (Y1 < miny) {miny = Y1;}
        if (Y2 < miny) {miny = Y2;}

        if (X1 > maxx) {maxx = X1;}
        if (X2 > maxx) {maxx = X2;}
        if (Y1 > maxy) {maxy = Y1;}
        if (Y2 > maxy) {maxy = Y2;}

        for (i = minx; i <= maxx; i ++)
        {
            for (j = miny; j <= maxy; j ++)
            {
                if (OLED_pnpoly(3, vx, vy, i, j)) {OLED_DrawPoint(i, j);}
            }
        }
    }
}

/**
 * @brief 绘制圆（中点画圆算法 / Bresenham 画圆）
 *
 * 利用八分对称性，一次计算可绘制 8 个对称点。
 * 填充模式下，对每个 X 值绘制从 -Y 到 +Y 的垂直线段。
 *
 * @param X        圆心 X 坐标
 * @param Y        圆心 Y 坐标
 * @param Radius   半径（像素）
 * @param IsFilled 填充模式
 */
void OLED_DrawCircle(int16_t X, int16_t Y, uint8_t Radius, uint8_t IsFilled)
{
    int16_t x, y, d, j;

    d = 1 - Radius;
    x = 0;
    y = Radius;

    OLED_DrawPoint(X + x, Y + y);
    OLED_DrawPoint(X - x, Y - y);
    OLED_DrawPoint(X + y, Y + x);
    OLED_DrawPoint(X - y, Y - x);

    if (IsFilled)
    {
        for (j = -y; j < y; j ++)
        {
            OLED_DrawPoint(X, Y + j);
        }
    }

    while (x < y)
    {
        x ++;
        if (d < 0)
        {
            d += 2 * x + 1;
        }
        else
        {
            y --;
            d += 2 * (x - y) + 1;
        }

        OLED_DrawPoint(X + x, Y + y);
        OLED_DrawPoint(X + y, Y + x);
        OLED_DrawPoint(X - x, Y - y);
        OLED_DrawPoint(X - y, Y - x);
        OLED_DrawPoint(X + x, Y - y);
        OLED_DrawPoint(X + y, Y - x);
        OLED_DrawPoint(X - x, Y + y);
        OLED_DrawPoint(X - y, Y + x);

        if (IsFilled)
        {
            for (j = -y; j < y; j ++)
            {
                OLED_DrawPoint(X + x, Y + j);
                OLED_DrawPoint(X - x, Y + j);
            }

            for (j = -x; j < x; j ++)
            {
                OLED_DrawPoint(X - y, Y + j);
                OLED_DrawPoint(X + y, Y + j);
            }
        }
    }
}

/**
 * @brief 绘制椭圆（中点椭圆算法）
 *
 * 分两阶段绘制：
 *   1. 区域 1（切线斜率 < 1）：X 为主步进方向
 *   2. 区域 2（切线斜率 >= 1）：Y 为主步进方向
 *
 * 填充模式下，对每个 X 值绘制垂直线段。
 *
 * @param X        椭圆中心 X 坐标
 * @param Y        椭圆中心 Y 坐标
 * @param A        半长轴（X 方向半径，像素）
 * @param B        半短轴（Y 方向半径，像素）
 * @param IsFilled 填充模式
 */
void OLED_DrawEllipse(int16_t X, int16_t Y, uint8_t A, uint8_t B, uint8_t IsFilled)
{
    int16_t x, y, j;
    int16_t a = A, b = B;
    float d1, d2;

    x = 0;
    y = b;
    d1 = b * b + a * a * (-b + 0.5);

    if (IsFilled)
    {
        for (j = -y; j < y; j ++)
        {
            OLED_DrawPoint(X, Y + j);
            OLED_DrawPoint(X, Y + j);
        }
    }

    OLED_DrawPoint(X + x, Y + y);
    OLED_DrawPoint(X - x, Y - y);
    OLED_DrawPoint(X - x, Y + y);
    OLED_DrawPoint(X + x, Y - y);

    while (b * b * (x + 1) < a * a * (y - 0.5))
    {
        if (d1 <= 0)
        {
            d1 += b * b * (2 * x + 3);
        }
        else
        {
            d1 += b * b * (2 * x + 3) + a * a * (-2 * y + 2);
            y --;
        }
        x ++;

        if (IsFilled)
        {
            for (j = -y; j < y; j ++)
            {
                OLED_DrawPoint(X + x, Y + j);
                OLED_DrawPoint(X - x, Y + j);
            }
        }

        OLED_DrawPoint(X + x, Y + y);
        OLED_DrawPoint(X - x, Y - y);
        OLED_DrawPoint(X - x, Y + y);
        OLED_DrawPoint(X + x, Y - y);
    }

    d2 = b * b * (x + 0.5) * (x + 0.5) + a * a * (y - 1) * (y - 1) - a * a * b * b;

    while (y > 0)
    {
        if (d2 <= 0)
        {
            d2 += b * b * (2 * x + 2) + a * a * (-2 * y + 3);
            x ++;
        }
        else
        {
            d2 += a * a * (-2 * y + 3);
        }
        y --;

        if (IsFilled)
        {
            for (j = -y; j < y; j ++)
            {
                OLED_DrawPoint(X + x, Y + j);
                OLED_DrawPoint(X - x, Y + j);
            }
        }

        OLED_DrawPoint(X + x, Y + y);
        OLED_DrawPoint(X - x, Y - y);
        OLED_DrawPoint(X - x, Y + y);
        OLED_DrawPoint(X + x, Y - y);
    }
}

/**
 * @brief 绘制圆弧
 *
 * 基于中点画圆算法，利用 OLED_IsInAngle() 判断当前像素是否在角度范围内。
 * 支持跨越 0 度的角度范围（如 350° ~ 10°）。
 *
 * 填充模式下，在每个 X 值处绘制从 -Y 到 +Y 的垂直线段（仅角度范围内的点）。
 *
 * @param X          圆心 X 坐标
 * @param Y          圆心 Y 坐标
 * @param Radius     半径（像素）
 * @param StartAngle 起始角度（度，0~360）
 * @param EndAngle   结束角度（度，0~360）
 * @param IsFilled   填充模式
 */
void OLED_DrawArc(int16_t X, int16_t Y, uint8_t Radius, int16_t StartAngle, int16_t EndAngle, uint8_t IsFilled)
{
    int16_t x, y, d, j;

    d = 1 - Radius;
    x = 0;
    y = Radius;

    if (OLED_IsInAngle(x, y, StartAngle, EndAngle))   {OLED_DrawPoint(X + x, Y + y);}
    if (OLED_IsInAngle(-x, -y, StartAngle, EndAngle)) {OLED_DrawPoint(X - x, Y - y);}
    if (OLED_IsInAngle(y, x, StartAngle, EndAngle))   {OLED_DrawPoint(X + y, Y + x);}
    if (OLED_IsInAngle(-y, -x, StartAngle, EndAngle)) {OLED_DrawPoint(X - y, Y - x);}

    if (IsFilled)
    {
        for (j = -y; j < y; j ++)
        {
            if (OLED_IsInAngle(0, j, StartAngle, EndAngle)) {OLED_DrawPoint(X, Y + j);}
        }
    }

    while (x < y)
    {
        x ++;
        if (d < 0)
        {
            d += 2 * x + 1;
        }
        else
        {
            y --;
            d += 2 * (x - y) + 1;
        }

        if (OLED_IsInAngle(x, y, StartAngle, EndAngle))   {OLED_DrawPoint(X + x, Y + y);}
        if (OLED_IsInAngle(y, x, StartAngle, EndAngle))   {OLED_DrawPoint(X + y, Y + x);}
        if (OLED_IsInAngle(-x, -y, StartAngle, EndAngle)) {OLED_DrawPoint(X - x, Y - y);}
        if (OLED_IsInAngle(-y, -x, StartAngle, EndAngle)) {OLED_DrawPoint(X - y, Y - x);}
        if (OLED_IsInAngle(x, -y, StartAngle, EndAngle))  {OLED_DrawPoint(X + x, Y - y);}
        if (OLED_IsInAngle(y, -x, StartAngle, EndAngle))  {OLED_DrawPoint(X + y, Y - x);}
        if (OLED_IsInAngle(-x, y, StartAngle, EndAngle))  {OLED_DrawPoint(X - x, Y + y);}
        if (OLED_IsInAngle(-y, x, StartAngle, EndAngle))  {OLED_DrawPoint(X - y, Y + x);}

        if (IsFilled)
        {
            for (j = -y; j < y; j ++)
            {
                if (OLED_IsInAngle(x, j, StartAngle, EndAngle))  {OLED_DrawPoint(X + x, Y + j);}
                if (OLED_IsInAngle(-x, j, StartAngle, EndAngle)) {OLED_DrawPoint(X - x, Y + j);}
            }

            for (j = -x; j < x; j ++)
            {
                if (OLED_IsInAngle(-y, j, StartAngle, EndAngle)) {OLED_DrawPoint(X - y, Y + j);}
                if (OLED_IsInAngle(y, j, StartAngle, EndAngle))  {OLED_DrawPoint(X + y, Y + j);}
            }
        }
    }
}

/* ======================== 绘图函数结束 ======================== */

/* ======================== 功能函数结束 ======================== */