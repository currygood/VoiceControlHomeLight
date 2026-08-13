# LED PWM 调光控制教程

## 模块概述

本项目使用 3 个 LED 灯珠模拟三路灯光，通过 ESP32-S3 的 PWM（脉宽调制）功能实现调光控制。PWM 通过改变占空比来调节 LED 的亮度。

## 技术规格

- **LED 类型**：标准 LED（建议 220Ω 限流电阻）
- **PWM 通道**：3 个（通道 0、1、2）
- **PWM 频率**：5000 Hz（5kHz）
- **分辨率**：16 位（0 ~ 65535）
- **调光范围**：0% ~ 100%

## 引脚定义

| LED 名称 | 功能说明 | ESP32-S3 引脚 | PWM 通道 |
|---------|---------|--------------|---------|
| LED1 | 灯光 1（客厅灯） | GPIO35 | PWM 通道 0 |
| LED2 | 灯光 2（卧室灯） | GPIO36 | PWM 通道 1 |
| LED3 | 灯光 3（书房灯） | GPIO37 | PWM 通道 2 |

## 硬件连接

### 连接示例

```
LED1 (阳极) ──[220Ω]──▶ GPIO35 (PWM0)
LED1 (阴极) ────────────── GND

LED2 (阳极) ──[220Ω]──▶ GPIO36 (PWM1)
LED2 (阴极) ────────────── GND

LED3 (阳极) ──[220Ω]──▶ GPIO37 (PWM2)
LED3 (阴极) ────────────── GND
```

### 注意事项

1. **限流电阻**：每个 LED 串联 220Ω 限流电阻，保护 LED。
2. **极性**：LED 有正负极，长脚为正极（阳极），短脚为负极（阴极）。
3. **电流**：LED 额定电流 20mA，220Ω 电阻限制电流约 15mA（3.3V / 220Ω）。

## PWM 配置

### PWM 驱动头文件

```c
#ifndef __PWM_DRIVER_H
#define __PWM_DRIVER_H

#include <stdint.h>
#include "driver/pwm.h"
#include "esp_err.h"

// --- PWM硬件配置宏 ---
#define LED_NUM 3            // LED 数量
#define PWM_FREQ 5000        // PWM 频率 5kHz
#define PWM_RES 16           // 16位分辨率 (0-65535)
#define PWM_GPIO_0 GPIO35    // LED1 PWM 通道
#define PWM_GPIO_1 GPIO36    // LED2 PWM 通道
#define PWM_GPIO_2 GPIO37    // LED3 PWM 通道

// PWM 通道数组
#define PWM_CHANS {PWM_GPIO_0, PWM_GPIO_1, PWM_GPIO_2}

// PWM 增益（亮度缩放）
#define PWM_GAIN 100         // 100% 增益

// 初始化 PWM
esp_err_t PWM_Init(void);

// 设置 LED 亮度
esp_err_t PWM_Set_Brightness(uint8_t led_id, uint8_t brightness);

// 获取 LED 亮度
uint8_t PWM_Get_Brightness(uint8_t led_id);

// 调整 LED 亮度
esp_err_t PWM_Adjust_Brightness(uint8_t led_id, int8_t step);

// 切换 LED 状态（开/关）
esp_err_t PWM_Toggle(uint8_t led_id, bool state);

// 关闭所有 LED
esp_err_t PWM_All_Off(void);

// 打开所有 LED
esp_err_t PWM_All_On(void);

// 获取 PWM 频率
uint32_t PWM_Get_Frequency(void);

#endif // __PWM_DRIVER_H
```

## PWM 驱动实现

```c
#include "pwm_driver.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "PWM_DRIVER";

// 全局变量
static uint8_t brightness[LED_NUM] = {0};  // LED 亮度（0-100%）

/**
 * @brief 初始化 PWM
 */
esp_err_t PWM_Init(void)
{
    // 配置 PWM 参数
    pwm_config_t pwm_conf = PWM_DEFAULT_CONFIG(PWM_FREQ, PWM_RES);
    pwm_conf.clk_src = PWM_CLK_APB;  // 使用 APB 时钟

    // 初始化所有 PWM 通道
    for (int i = 0; i < LED_NUM; i++)
    {
        esp_err_t ret = pwm_init(PWM_CHANS[i], &pwm_conf, 1);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "PWM 通道 %d 初始化失败", i);
            return ret;
        }

        // 初始亮度为 0
        pwm_set_duty(PWM_CHANS[i], 0);
        pwm_set_alias_duty(PWM_CHANS[i], 0);  // 设置占空比
        pwm_start(PWM_CHANS[i]);              // 启动 PWM
    }

    ESP_LOGI(TAG, "PWM 初始化成功: 频率=%d Hz, 分辨率=%d 位",
             PWM_FREQ, PWM_RES);
    return ESP_OK;
}

/**
 * @brief 设置 LED 亮度
 * @param led_id LED 编号（0-2）
 * @param brightness 亮度（0-100%）
 */
esp_err_t PWM_Set_Brightness(uint8_t led_id, uint8_t brightness)
{
    if (led_id >= LED_NUM)
    {
        ESP_LOGE(TAG, "LED 编号超出范围: %d", led_id);
        return ESP_ERR_INVALID_ARG;
    }

    if (brightness > 100)
    {
        brightness = 100;
    }

    brightness[led_id] = brightness;

    // 将亮度（0-100）转换为 PWM 占空比（0-65535）
    uint16_t duty = (uint16_t)((brightness / 100.0) * PWM_GAIN);
    pwm_set_duty(PWM_CHANS[led_id], duty);
    pwm_set_alias_duty(PWM_CHANS[led_id], duty);

    ESP_LOGD(TAG, "LED%d 亮度设置: %d%%", led_id, brightness);
    return ESP_OK;
}

/**
 * @brief 获取 LED 亮度
 * @param led_id LED 编号（0-2）
 * @return 亮度（0-100%）
 */
uint8_t PWM_Get_Brightness(uint8_t led_id)
{
    if (led_id >= LED_NUM)
    {
        return 0;
    }
    return brightness[led_id];
}

/**
 * @brief 调整 LED 亮度
 * @param led_id LED 编号（0-2）
 * @param step 调整量（正数为增，负数为减）
 */
esp_err_t PWM_Adjust_Brightness(uint8_t led_id, int8_t step)
{
    if (led_id >= LED_NUM)
    {
        return ESP_ERR_INVALID_ARG;
    }

    brightness[led_id] += step;
    if (brightness[led_id] > 100)
    {
        brightness[led_id] = 100;
    }
    if (brightness[led_id] < 0)
    {
        brightness[led_id] = 0;
    }

    uint16_t duty = (uint16_t)((brightness[led_id] / 100.0) * PWM_GAIN);
    pwm_set_duty(PWM_CHANS[led_id], duty);
    pwm_set_alias_duty(PWM_CHANS[led_id], duty);

    ESP_LOGI(TAG, "LED%d 亮度调整: %d%%", led_id, brightness[led_id]);
    return ESP_OK;
}

/**
 * @brief 切换 LED 状态
 * @param led_id LED 编号（0-2）
 * @param state true=开，false=关
 */
esp_err_t PWM_Toggle(uint8_t led_id, bool state)
{
    if (state)
    {
        return PWM_Set_Brightness(led_id, brightness[led_id]);
    }
    else
    {
        return PWM_Set_Brightness(led_id, 0);
    }
}

/**
 * @brief 关闭所有 LED
 */
esp_err_t PWM_All_Off(void)
{
    for (int i = 0; i < LED_NUM; i++)
    {
        PWM_Set_Brightness(i, 0);
    }
    return ESP_OK;
}

/**
 * @brief 打开所有 LED
 */
esp_err_t PWM_All_On(void)
{
    for (int i = 0; i < LED_NUM; i++)
    {
        PWM_Set_Brightness(i, 100);
    }
    return ESP_OK;
}

/**
 * @brief 获取 PWM 频率
 */
uint32_t PWM_Get_Frequency(void)
{
    return PWM_FREQ;
}
```

## 使用示例

### 完整使用示例

```c
#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "pwm_driver.h"

static const char *TAG = "LED_DEMO";

void app_main(void)
{
    // 1. 初始化 PWM
    esp_err_t ret = PWM_Init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "PWM 初始化失败");
        return;
    }

    ESP_LOGI(TAG, "PWM 初始化成功");

    // 2. 设置 LED 亮度
    PWM_Set_Brightness(0, 50);  // LED1 50%
    PWM_Set_Brightness(1, 80);  // LED2 80%
    PWM_Set_Brightness(2, 30);  // LED3 30%

    // 3. 演示调光效果
    ESP_LOGI(TAG, "演示调光效果");

    for (int i = 0; i <= 100; i += 10)
    {
        PWM_Set_Brightness(0, i);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    for (int i = 100; i >= 0; i -= 10)
    {
        PWM_Set_Brightness(1, i);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    for (int i = 0; i <= 100; i += 5)
    {
        PWM_Set_Brightness(2, i);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // 4. 关闭所有 LED
    PWM_All_Off();
    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG, "演示完成");
}
```

### 任务模式使用

```c
void LED_Task(void *pvParameters)
{
    // 初始化 PWM
    PWM_Init();

    while (1)
    {
        // 读取亮度值并更新
        uint8_t brightness0 = PWM_Get_Brightness(0);
        uint8_t brightness1 = PWM_Get_Brightness(1);
        uint8_t brightness2 = PWM_Get_Brightness(2);

        ESP_LOGI(TAG, "LED亮度: LED0=%d%%, LED1=%d%%, LED2=%d%%",
                 brightness0, brightness1, brightness2);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```

### 与 OLED 联动显示

```c
void Task_OLED_With_LED(void *pvParameters)
{
    // 初始化 I²C 和 OLED
    i2c_master_bus_handle_t i2c_bus;
    I2c_Init_Bus(I2C_NUM_0, I2C_SDA_GPIO, I2C_SCL_GPIO, I2C_FREQ, &i2c_bus);
    I2c_Add_Device(i2c_bus, OLED_ADDR, I2C_FREQ, &oled_dev);
    OLED_Init(i2c_bus);

    // 初始化 PWM
    PWM_Init();

    while (1)
    {
        // 获取 LED 亮度
        uint8_t b0 = PWM_Get_Brightness(0);
        uint8_t b1 = PWM_Get_Brightness(1);
        uint8_t b2 = PWM_Get_Brightness(2);

        // 在 OLED 上显示
        OLED_Clear();
        OLED_ShowString(0, 0, "Light Status", OLED_8X16);

        char buf[20];
        snprintf(buf, sizeof(buf), "LED1: %d%%", b0);
        OLED_ShowString(0, 2, buf, OLED_6X8);

        snprintf(buf, sizeof(buf), "LED2: %d%%", b1);
        OLED_ShowString(0, 3, buf, OLED_6X8);

        snprintf(buf, sizeof(buf), "LED3: %d%%", b2);
        OLED_ShowString(0, 4, buf, OLED_6X8);

        OLED_Update();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
```

## MCP 指令处理

本项目使用 MCP 协议接收云端指令来控制 LED。

### MCP 指令格式

```json
{
  "type": "light.set",
  "params": {
    "target": "led1",
    "brightness": 80
  }
}
```

### MCP 处理函数

```c
#include "mcp_handler.h"

void MCP_Handle_Light_Set(JSON_Object *params)
{
    const char *target = json_object_get_string(params, "target");
    int brightness = json_object_get_number(params, "brightness");

    if (strcmp(target, "led1") == 0)
    {
        PWM_Set_Brightness(0, brightness);
        ESP_LOGI("MCP", "LED1 亮度设置为: %d%%", brightness);
    }
    else if (strcmp(target, "led2") == 0)
    {
        PWM_Set_Brightness(1, brightness);
        ESP_LOGI("MCP", "LED2 亮度设置为: %d%%", brightness);
    }
    else if (strcmp(target, "led3") == 0)
    {
        PWM_Set_Brightness(2, brightness);
        ESP_LOGI("MCP", "LED3 亮度设置为: %d%%", brightness);
    }
    else
    {
        ESP_LOGE("MCP", "未知的 LED: %s", target);
    }
}
```

### 状态查询指令

```json
{
  "type": "light.status.get",
  "params": {}
}
```

### 返回状态

```json
{
  "type": "light.status",
  "data": {
    "led1": 80,
    "led2": 50,
    "led3": 30
  }
}
```

## PWM 参数详解

### 频率配置

```c
#define PWM_FREQ 5000  // 5kHz
```

- **推荐频率**：1kHz ~ 10kHz
- **原因**：人眼无法识别 PWM 频率闪烁，避免闪烁
- **太低**：可能出现闪烁
- **太高**：CPU 开销增加

### 分辨率配置

```c
#define PWM_RES 16  // 16 位
```

- **16 位**：0 ~ 65535 级调光，非常平滑
- **12 位**：0 ~ 4095 级调光，推荐
- **8 位**：0 ~ 255 级调光，精度较低

### 时钟源

```c
pwm_conf.clk_src = PWM_CLK_APB;
```

- **APB 时钟**：直接连接，响应快
- **PLL 时钟**：频率高，但延迟大

## 性能优化

### 1. 批量设置

```c
// 批量设置所有 LED 亮度
void PWM_Set_All_Brightness(uint8_t *brightness_array)
{
    for (int i = 0; i < LED_NUM; i++)
    {
        PWM_Set_Brightness(i, brightness_array[i]);
    }
}
```

### 2. 低功耗模式

```c
// LED 亮度为 0 时，关闭 PWM
if (brightness == 0)
{
    pwm_stop(channel);
}
else
{
    pwm_start(channel);
}
```

### 3. 动态调整频率

```c
// 根据亮度动态调整 PWM 频率
uint32_t freq = (brightness == 0) ? 0 : PWM_FREQ;
pwm_set_freq(channel, freq);
```

## 常见问题

### 1. LED 不亮

**原因**：
- 引脚配置错误
- PWM 未启动
- 亮度为 0

**解决方法**：
- 检查硬件连接
- 确认 PWM 已启动（pwm_start）
- 设置亮度不为 0

### 2. LED 闪烁

**原因**：
- PWM 频率过低
- 任务调度延迟

**解决方法**：
- 增加 PWM 频率到 5kHz 或更高
- 提高 PWM 任务优先级

### 3. 调光不平滑

**原因**：
- 分辨率太低
- 亮度调整步长过大

**解决方法**：
- 使用 16 位分辨率
- 减小步长（如 1%）

## 调试技巧

### 1. 监听 PWM 信号

使用示波器观察 PWM 引脚波形。

### 2. 打印调试信息

```c
ESP_LOGI(TAG, "LED%d 亮度: %d%%", led_id, brightness);
ESP_LOGD(TAG, "PWM 占空比: %d", duty);
```

### 3. 测试单个 LED

```c
// 只测试 LED1
PWM_Init();
PWM_Set_Brightness(0, 50);
vTaskDelay(pdMS_TO_TICKS(5000));
PWM_All_Off();
```

## 扩展功能

### 1. 定时开关

```c
void PWM_Set_Timeout(uint8_t led_id, uint8_t brightness, uint32_t timeout_ms)
{
    PWM_Set_Brightness(led_id, brightness);
    xTimerStart(xTimerCreate(...), timeout_ms);
}
```

### 2. 渐变效果

```c
void PWM_Fade(uint8_t led_id, uint8_t target_brightness, uint32_t duration_ms)
{
    uint8_t current = PWM_Get_Brightness(led_id);
    int step = (target_brightness > current) ? 1 : -1;

    for (int i = current; i != target_brightness; i += step)
    {
        PWM_Set_Brightness(led_id, i);
        vTaskDelay(pdMS_TO_TICKS(duration_ms / 100));
    }
}
```

### 3. 场景模式

```c
void PWM_Set_Scene(uint8_t scene)
{
    switch (scene)
    {
        case 1:  // 夜间模式
            PWM_Set_Brightness(0, 20);
            PWM_Set_Brightness(1, 10);
            PWM_Set_Brightness(2, 30);
            break;
        case 2:  // 白天模式
            PWM_Set_Brightness(0, 100);
            PWM_Set_Brightness(1, 80);
            PWM_Set_Brightness(2, 90);
            break;
        case 3:  // 节能模式
            PWM_All_Off();
            break;
    }
}
```

## 参考资料

- ESP32-S3 PWM 驱动文档
- LED 驱动原理
- PWM 调光技术
