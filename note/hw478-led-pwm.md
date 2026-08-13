# HW 478 RGB LED 模块 — PWM 亮度和颜色控制

## 硬件原理

HW 478 是一个共阳 RGB LED 模块，3 个输入端分别接 R、G、B 三个颜色的 PWM 信号。

```
         ┌──────────────────┐
GPIO 1 ──┤ R (PWM)          │
GPIO 2 ──┤ G (PWM)   HW478 │
GPIO 3 ──┤ B (PWM)          │
GND    ──┤ GND              │
         └──────────────────┘
```

ESP32-S3 通过 LEDC（LED PWM Controller）外设输出 PWM 信号。改变占空比就能改变每个颜色通道的亮度，三通道叠加出任意颜色。

## 颜色 = 三通道比例

PWM 占空比决定了每个颜色发光二极管的导通时间占比。RGB 三个值组合出颜色：

| 颜色 | R | G | B | 视觉效果 |
|------|---|---|---|---------|
| red | 255 | 0 | 0 | 红色 |
| green | 0 | 255 | 0 | 绿色 |
| blue | 0 | 0 | 255 | 蓝色 |
| white | 255 | 255 | 255 | 冷白 |
| warm_yellow | 255 | 180 | 80 | 暖黄 |
| cool_white | 200 | 220 | 255 | 正白 |

## 亮度 = 等比缩放

在 RGB 值上统一乘以亮度系数，颜色不变但整体更暗或更亮：

```
duty = (color_value × brightness × max_duty) / (255 × 100)
```

示例（暖黄色，10-bit 分辨率 max_duty=1023）：

| brightness | R duty | G duty | B duty | 效果 |
|-----------|--------|--------|--------|------|
| 100% | 1023 | 722 | 321 | 最亮 |
| 50% | 511 | 361 | 160 | 亮度减半，颜色不变 |
| 10% | 102 | 72 | 32 | 很暗但仍是暖黄色 |
| 0% | 0 | 0 | 0 | 关闭 |

三个通道等比缩放，R:G:B 的比例始终是 255:180:80，所以颜色不会变，只有亮度变化。

## 代码实现

### 初始化 LEDC

```c
// 1 个 Timer，6 个 Channel（卧室和客厅各 3 路）

ledc_timer_config_t timer_cfg = {
    .speed_mode = LEDC_LOW_SPEED_MODE,
    .timer_num = LEDC_TIMER_0,
    .duty_resolution = LEDC_TIMER_10_BIT,  // 0-1023
    .freq_hz = 4000,                        // 4kHz，远超可见闪烁
    .clk_cfg = LEDC_AUTO_CLK,
};
ledc_timer_config(&timer_cfg);

ledc_channel_config_t ch_cfg = {
    .speed_mode = LEDC_LOW_SPEED_MODE,
    .timer_sel = LEDC_TIMER_0,
    .intr_type = LEDC_INTR_DISABLE,
    .duty = 0,  // 初始关闭
};
// 对每个 GPIO 设置 channel_num 0-5
```

### 设置颜色和亮度

```c
static const struct {
    const char *name;
    uint8_t r, g, b;
} color_map[] = {
    {"red",         255, 0,   0},
    {"green",       0,   255, 0},
    {"blue",        0,   0,   255},
    {"white",       255, 255, 255},
    {"warm_yellow", 255, 180, 80},
    {"cool_white",  200, 220, 255},
};

esp_err_t LightController_SetLight(const char *location, bool on,
                                   uint8_t brightness, const char *color)
{
    // 1. 查找颜色表
    // 2. 计算每个通道的 duty 值
    // 3. ledc_set_duty + ledc_update_duty

    int ch_offset = (strcmp(location, "bedroom") == 0) ? 0 : 3;

    for (int i = 0; i < 3; i++)
    {
        uint8_t rgb[3] = {found_color.r, found_color.g, found_color.b};
        uint32_t duty = on ? ((uint32_t)rgb[i] * brightness * 1023) / (255 * 100) : 0;

        ledc_set_duty(LEDC_LOW_SPEED_MODE, ch_offset + i, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, ch_offset + i);
    }
    return ESP_OK;
}
```

## GPIO 引脚分配

| 灯 | 通道 | GPIO | LEDC Channel |
|----|------|------|-------------|
| 卧室 R | RED | GPIO 1 | CH0 |
| 卧室 G | GREEN | GPIO 2 | CH1 |
| 卧室 B | BLUE | GPIO 3 | CH2 |
| 客厅 R | RED | GPIO 4 | CH3 |
| 客厅 G | GREEN | GPIO 11 | CH4 |
| 客厅 B | BLUE | GPIO 12 | CH5 |

6 个通道共用一个 LEDC Timer（同频率 4kHz），省 Timer 资源，LED 不需要不同频率。
