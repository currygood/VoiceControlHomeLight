# 0.96寸 OLED 显示屏使用教程

## 模块概述

0.96寸 OLED 显示屏采用 SSD1306 驱动芯片，通过 I²C 接口通信。本项目使用 OLED 显示 Wi-Fi 状态、当前灯光亮度、最后识别文本等信息。

## 技术规格

- **屏幕尺寸**：0.96 英寸
- **分辨率**：128 × 64 像素
- **驱动芯片**：SSD1306
- **接口类型**：I²C
- **通信频率**：100kHz / 400kHz
- **工作电压**：3.3V ~ 5V
- **亮度调节**：软件调节（对比度控制）
- **显示颜色**：单色（蓝绿色）

## 引脚定义

| 引脚名称 | 功能说明 | ESP32-S3 引脚 | 说明 |
|---------|---------|--------------|------|
| VCC | 电源正极 | 3.3V | 建议加 0.1μF 去耦电容 |
| GND | 接地 | GND | 必须与系统共地 |
| SDA | I²C 数据线 | GPIO11 | I²C 数据 |
| SCL | I²C 时钟线 | GPIO12 | I²C 时钟 |

## 硬件连接

### 连接示例

```
OLED显示屏               ESP32-S3
┌─────────────┐          ┌─────────────┐
│ VCC (3.3V)  │──────────▶│ 3.3V        │
│ GND         │──────────▶│ GND         │
│ SDA         │──────────▶│ GPIO11      │
│ SCL         │──────────▶│ GPIO12      │
└─────────────┘          └─────────────┘
```

## I²C 配置

### 驱动头文件

```c
#ifndef I2C_DRIVER_H
#define I2C_DRIVER_H

#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

// --- I²C硬件配置宏 ---
#define I2C_PORT I2C_NUM_0
#define I2C_SDA_GPIO 11
#define I2C_SCL_GPIO 12
#define I2C_FREQ 400000  // 400kHz 快速模式

// I²C 总线句柄
typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
} i2c_handle_t;

// 初始化 I²C 总线
esp_err_t I2c_Init_Bus(i2c_port_t port, gpio_num_t sda_pin, gpio_num_t scl_pin, uint32_t freq_hz, i2c_master_bus_handle_t *bus_handle);

// 添加 I²C 设备
esp_err_t I2c_Add_Device(i2c_master_bus_handle_t bus_handle, uint16_t dev_addr, uint32_t freq_hz, i2c_master_dev_handle_t *dev_handle);

// 写寄存器
esp_err_t I2c_Write_Reg(i2c_master_dev_handle_t dev_handle, uint8_t reg, uint8_t data);

// 读寄存器
esp_err_t I2c_Read_Reg(i2c_master_dev_handle_t dev_handle, uint8_t reg, uint8_t *data);

// 读 FIFO 或多字节
esp_err_t I2c_Read_Bytes(i2c_master_dev_handle_t dev_handle, uint8_t reg, uint8_t *buffer, size_t count);

// 删除设备
esp_err_t I2c_Remove_Device(i2c_master_dev_handle_t dev_handle);

// 删除总线
esp_err_t I2c_Delete_Bus(i2c_master_bus_handle_t bus_handle);

// 获取全局 I²C 总线句柄
i2c_master_bus_handle_t I2c_Get_Global_Bus_Handle(void);

// 写多个字节
esp_err_t I2c_Write_Bytes(i2c_master_dev_handle_t dev_handle, uint8_t reg, uint8_t *buffer, size_t count);

#endif // I2C_DRIVER_H
```

### I²C 驱动实现

```c
#include "i2c_driver.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "I2C_DRIVER";

// 全局 I²C 总线句柄
static i2c_master_bus_handle_t global_i2c_bus = NULL;

// 初始化 I²C 总线
esp_err_t I2c_Init_Bus(i2c_port_t port, gpio_num_t sda_pin, gpio_num_t scl_pin, uint32_t freq_hz, i2c_master_bus_handle_t *bus_handle)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = port,
        .sda_io_num = sda_pin,
        .scl_io_num = scl_pin,
        .flags = {
            .enable_internal_pullup = true,
        }
    };

    esp_err_t ret = i2c_new_master_bus(&bus_cfg, bus_handle);
    if (ret == ESP_OK && bus_handle != NULL)
    {
        global_i2c_bus = *bus_handle; // 保存全局句柄
        ESP_LOGI(TAG, "I²C 总线初始化成功: SDA=GPIO%d, SCL=GPIO%d, 频率=%d kHz",
                 sda_pin, scl_pin, freq_hz / 1000);
    }
    return ret;
}

// 添加 I²C 设备
esp_err_t I2c_Add_Device(i2c_master_bus_handle_t bus_handle, uint16_t dev_addr, uint32_t freq_hz, i2c_master_dev_handle_t *dev_handle)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = dev_addr,
        .scl_speed_hz = freq_hz,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus_handle, &dev_cfg, dev_handle);
    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "I²C 设备添加成功: 地址=0x%02X", dev_addr);
    }
    return ret;
}

// 写寄存器
esp_err_t I2c_Write_Reg(i2c_master_dev_handle_t dev_handle, uint8_t reg, uint8_t data)
{
    uint8_t buf[2] = {reg, data};
    return i2c_master_transmit(dev_handle, buf, sizeof(buf), -1);
}

// 读寄存器
esp_err_t I2c_Read_Reg(i2c_master_dev_handle_t dev_handle, uint8_t reg, uint8_t *data)
{
    return i2c_master_transmit_receive(dev_handle, &reg, 1, data, 1, -1);
}

// 读多字节
esp_err_t I2c_Read_Bytes(i2c_master_dev_handle_t dev_handle, uint8_t reg, uint8_t *buffer, size_t count)
{
    return i2c_master_transmit_receive(dev_handle, &reg, 1, buffer, count, -1);
}

// 删除设备
esp_err_t I2c_Remove_Device(i2c_master_dev_handle_t dev_handle)
{
    return i2c_master_bus_rm_device(dev_handle);
}

// 删除总线
esp_err_t I2c_Delete_Bus(i2c_master_bus_handle_t bus_handle)
{
    return i2c_del_master_bus(bus_handle);
}

// 获取全局 I²C 总线句柄
i2c_master_bus_handle_t I2c_Get_Global_Bus_Handle(void)
{
    return global_i2c_bus;
}

// 写多个字节
esp_err_t I2c_Write_Bytes(i2c_master_dev_handle_t dev_handle, uint8_t reg, uint8_t *buffer, size_t count)
{
    uint8_t *temp_buf = (uint8_t *)malloc(count + 1);
    if (temp_buf == NULL) return ESP_ERR_NO_MEM;

    temp_buf[0] = reg;
    memcpy(&temp_buf[1], buffer, count);

    esp_err_t ret = i2c_master_transmit(dev_handle, temp_buf, count + 1, -1);
    free(temp_buf);
    return ret;
}
```

## OLED 接口

### OLED 驱动头文件

```c
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

/* 参数宏定义 */

// OLED设备地址定义
#define OLED_ADDR 0x3C

/*FontSize参数取值*/
#define OLED_8X16       8
#define OLED_6X8        6
#define OLED_12X24      12

/*IsFilled参数数值*/
#define OLED_UNFILLED   0
#define OLED_FILLED     1

/*初始化函数*/

/**
 * OLED_Init - 初始化 OLED 屏幕
 *
 * 调用前需已完成 I²C 总线初始化，并通过 I2c_Add_Device() 添加 OLED 设备，
 * 将设备句柄传入本函数。
 *
 * @param dev_handle  由 I2c_Add_Device() 返回的 I²C 设备句柄
 */
esp_err_t OLED_Init(i2c_master_bus_handle_t bus_handle);

void OLED_WriteCommand(uint8_t Command);

/*更新函数*/
void OLED_Update(void);
void OLED_UpdateArea(int16_t X, int16_t Y, uint8_t Width, uint8_t Height);

/*显存控制函数*/
void OLED_Clear(void);
void OLED_ClearArea(int16_t X, int16_t Y, uint8_t Width, uint8_t Height);
void OLED_Reverse(void);
void OLED_ReverseArea(int16_t X, int16_t Y, uint8_t Width, uint8_t Height);

/*显示函数*/
void OLED_ShowChar(int16_t X, int16_t Y, char Char, uint8_t FontSize);
void OLED_ShowString(int16_t X, int16_t Y, char *String, uint8_t FontSize);
void OLED_ShowNum(int16_t X, int16_t Y, uint32_t Number, uint8_t Length, uint8_t FontSize);
void OLED_ShowSignedNum(int16_t X, int16_t Y, int32_t Number, uint8_t Length, uint8_t FontSize);
void OLED_ShowHexNum(int16_t X, int16_t Y, uint32_t Number, uint8_t Length, uint8_t FontSize);
void OLED_ShowBinNum(int16_t X, int16_t Y, uint32_t Number, uint8_t Length, uint8_t FontSize);
void OLED_ShowFloatNum(int16_t X, int16_t Y, double Number, uint8_t IntLength, uint8_t FraLength, uint8_t FontSize);
void OLED_ShowImage(int16_t X, int16_t Y, uint8_t Width, uint8_t Height, const uint8_t *Image);
void OLED_Printf(int16_t X, int16_t Y, uint8_t FontSize, char *format, ...);

/*绘图函数*/
void OLED_DrawPoint(int16_t X, int16_t Y);
uint8_t OLED_GetPoint(int16_t X, int16_t Y);
void OLED_DrawLine(int16_t X0, int16_t Y0, int16_t X1, int16_t Y1);
void OLED_DrawRectangle(int16_t X, int16_t Y, uint8_t Width, uint8_t Height, uint8_t IsFilled);
void OLED_DrawTriangle(int16_t X0, int16_t Y0, int16_t X1, int16_t Y1, int16_t X2, int16_t Y2, uint8_t IsFilled);
void OLED_DrawCircle(int16_t X, int16_t Y, uint8_t Radius, uint8_t IsFilled);
void OLED_DrawEllipse(int16_t X, int16_t Y, uint8_t A, uint8_t B, uint8_t IsFilled);
void OLED_DrawArc(int16_t X, int16_t Y, uint8_t Radius, int16_t StartAngle, int16_t EndAngle, uint8_t IsFilled);


// FreeRTOS 任务函数
void OLED_Set_ShowState(uint8_t state);
void OLED_Notify_Show(bool isShow);
void Task_OLED_Show(void *pvParameters);

#endif // __OLED_H
```

## 使用示例

### 完整使用示例

```c
#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "i2c_driver.h"
#include "OLED.h"

static const char *TAG = "OLED_DEMO";

void app_main(void)
{
    // 1. 初始化 I²C 总线
    i2c_master_bus_handle_t i2c_bus;
    esp_err_t ret = I2c_Init_Bus(I2C_NUM_0, I2C_SDA_GPIO, I2C_SCL_GPIO, I2C_FREQ, &i2c_bus);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "I²C 总线初始化失败");
        return;
    }

    // 2. 添加 OLED 设备
    i2c_master_dev_handle_t oled_dev;
    ret = I2c_Add_Device(i2c_bus, OLED_ADDR, I2C_FREQ, &oled_dev);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "OLED 设备添加失败");
        return;
    }

    // 3. 初始化 OLED
    ret = OLED_Init(i2c_bus);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "OLED 初始化失败");
        return;
    }

    ESP_LOGI(TAG, "OLED 初始化成功，开始显示");

    // 4. 清屏
    OLED_Clear();

    // 5. 显示欢迎信息
    OLED_ShowString(0, 0, "Welcome!", OLED_8X16);
    OLED_ShowString(0, 2, "ESP32-S3", OLED_8X16);
    OLED_ShowString(0, 4, "OLED Test", OLED_8X16);
    OLED_Update();

    // 6. 显示文本
    OLED_ShowString(0, 0, "Hello, World!", OLED_6X8);
    OLED_ShowString(0, 2, "ESP32-S3", OLED_6X8);

    // 7. 显示数字
    OLED_ShowNum(0, 4, 12345, 5, OLED_6X8);
    OLED_ShowNum(0, 5, 67890, 5, OLED_6X8);

    OLED_Update();

    // 8. 绘制图形
    OLED_DrawCircle(64, 32, 20, OLED_FILLED);
    OLED_Update();

    vTaskDelay(pdMS_TO_TICKS(3000));

    // 9. 清屏
    OLED_Clear();
    OLED_ShowString(0, 2, "Bye!", OLED_8X16);
    OLED_Update();
}
```

### 任务模式使用

```c
#include "OLED.h"

// 全局 I²C 总线句柄
i2c_master_bus_handle_t global_i2c_bus = NULL;

// 显示状态标志
bool isOLEDShow = false;
uint8_t OLED_ShowState = 1;

void Task_OLED_Show(void *pvParameters)
{
    // 1. 获取全局 I²C 总线句柄
    i2c_master_bus_handle_t i2c_bus = I2c_Get_Global_Bus_Handle();
    if (i2c_bus == NULL)
    {
        ESP_LOGE("OLED", "I²C 总线句柄为空");
        vTaskDelete(NULL);
        return;
    }

    // 2. 添加 OLED 设备
    i2c_master_dev_handle_t oled_dev;
    esp_err_t ret = I2c_Add_Device(i2c_bus, OLED_ADDR, I2C_FREQ, &oled_dev);
    if (ret != ESP_OK)
    {
        ESP_LOGE("OLED", "OLED 设备添加失败");
        vTaskDelete(NULL);
        return;
    }

    // 3. 初始化 OLED
    ret = OLED_Init(i2c_bus);
    if (ret != ESP_OK)
    {
        ESP_LOGE("OLED", "OLED 初始化失败");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI("OLED", "OLED 初始化成功");

    while (1)
    {
        // 根据状态刷新显示
        if (isOLEDShow)
        {
            OLED_WriteCommand(0xAF); // 开启显示
        }
        else
        {
            OLED_Clear();
            OLED_WriteCommand(0xAE); // 关闭显示
        }

        OLED_Update();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
```

## 显示功能详解

### 字符显示

```c
// 显示单个字符
OLED_ShowChar(0, 0, 'A', OLED_8X16);

// 显示字符串
OLED_ShowString(0, 0, "Hello", OLED_6X8);

// 显示数字
OLED_ShowNum(0, 2, 123, 3, OLED_6X8);
```

### 图形绘制

```c
// 画点
OLED_DrawPoint(64, 32);

// 画线
OLED_DrawLine(0, 0, 127, 63);

// 画矩形
OLED_DrawRectangle(10, 10, 50, 30, OLED_FILLED);

// 画圆
OLED_DrawCircle(64, 32, 20, OLED_FILLED);

// 画椭圆
OLED_DrawEllipse(64, 32, 30, 20, OLED_FILLED);
```

### 区域操作

```c
// 清屏
OLED_Clear();

// 清除指定区域
OLED_ClearArea(0, 0, 128, 64);

// 反色显示
OLED_Reverse();

// 反色显示指定区域
OLED_ReverseArea(0, 0, 128, 64);
```

### 图像显示

```c
// 显示图片
OLED_ShowImage(0, 0, 16, 16, image_data);
```

## 控制命令详解

### 初始化命令序列

```c
void OLED_Init(i2c_master_bus_handle_t bus_handle)
{
    // 关闭显示
    OLED_WriteCommand(0xAE);

    // 设置时钟分频
    OLED_WriteCommand(0xD5);
    OLED_WriteCommand(0x80);

    // 设置多路复用
    OLED_WriteCommand(0xA8);
    OLED_WriteCommand(0x3F);  // 64 复用率

    // 设置位移
    OLED_WriteCommand(0xD3);
    OLED_WriteCommand(0x00);

    // 设置开始行
    OLED_WriteCommand(0x40);

    // 左右方向
    OLED_WriteCommand(0xA1);

    // 上下方向
    OLED_WriteCommand(0xC8);

    // COM 引脚配置
    OLED_WriteCommand(0xDA);
    OLED_WriteCommand(0x12);

    // 对比度
    OLED_WriteCommand(0x81);
    OLED_WriteCommand(0xCF);

    // 预充电
    OLED_WriteCommand(0xD9);
    OLED_WriteCommand(0xF1);

    // VCOMH 电压
    OLED_WriteCommand(0xDB);
    OLED_WriteCommand(0x30);

    // 全屏点亮/不点亮
    OLED_WriteCommand(0xA4);

    // 正常显示
    OLED_WriteCommand(0xA6);

    // 电荷泵
    OLED_WriteCommand(0x8D);
    OLED_WriteCommand(0x14);

    // 打开显示
    OLED_WriteCommand(0xAF);
}
```

### 窗口操作

```c
// 设置光标位置（页和列）
OLED_SetCursor(0, 0);  // 第0页，第0列

// 写入数据到指定区域
OLED_UpdateArea(0, 0, 64, 32);
```

## 性能优化

### 1. 增量更新

```c
// 只更新变化的区域，避免全屏刷新
OLED_UpdateArea(0, 0, 128, 64);
```

### 2. 减少写入次数

```c
// 批量写入数据
OLED_WriteData(buffer, size);
```

### 3. 控制刷新频率

```c
// 降低刷新频率
vTaskDelay(pdMS_TO_TICKS(200));
```

## 常见问题

### 1. 无法初始化

**原因**：
- I²C 引脚配置错误
- 设备地址错误
- 电源不稳定

**解决方法**：
- 检查 SDA、SCL 引脚
- 确认地址为 0x3C 或 0x3D
- 检查电源电压

### 2. 显示异常

**原因**：
- 初始化命令顺序错误
- 显存未正确填充

**解决方法**：
- 检查初始化序列
- 确保调用 OLED_Update() 刷新

### 3. 字符显示乱码

**原因**：
- 字体数组未定义
- 字符索引错误

**解决方法**：
- 检查 OLED_Data.h
- 确保字符在 ASCII 范围内

## 调试技巧

### 1. 检查 I²C 通信

```c
// 使用逻辑分析仪或 i2c-tools
i2c_detect I2C_NUM_0 11 12
```

### 2. 打印调试信息

```c
ESP_LOGI(TAG, "OLED 初始化完成");
ESP_LOGI(TAG, "显示状态: %s", isOLEDShow ? "开" : "关");
```

### 3. 测试单个命令

```c
// 测试写命令
OLED_WriteCommand(0xAE);
OLED_WriteCommand(0xAF);
OLED_Update();
```

## 应用场景

本项目使用 OLED 显示：
- Wi-Fi 连接状态
- 当前灯光亮度
- 最后识别的语音文本
- 系统状态信息

### 示例显示内容

```
Wi-Fi: Connected
IP: 192.168.1.100

Brightness: 80%

Last command: Turn on light
```

## 参考资料

- SSD1306 数据手册
- I²C 协议规范
- ESP32-S3 I²C 驱动文档
