/**
 * @file LED_Control.c
 * @brief HW478 RGB LED 模块实现（基于 PWM 亮度和颜色控制）
 *
 * 本模块封装 HW478 共阳 RGB LED 模块的 PWM 控制功能。
 * 通过 ESP32-S3 的 LEDC 外设输出 3 路 PWM 信号，分别控制
 * R、G、B 三个颜色通道的亮度，三通道叠加出任意颜色。
 *
 * 核心功能：
 *   - 初始化：配置 LEDC 定时器和 6 个 PWM 通道（卧室 3 路 + 客厅 3 路）
 *   - 颜色控制：通过 RGB 三个分量组合出任意颜色
 *   - 亮度控制：等比例缩放 RGB 值，保持颜色比例不变
 *
 * 硬件连接：
 *   - 卧室 R: GPIO1  (LEDC CH0)
 *   - 卧室 G: GPIO2  (LEDC CH1)
 *   - 卧室 B: GPIO3  (LEDC CH2)
 *   - 客厅 R: GPIO4  (LEDC CH3)
 *   - 客厅 G: GPIO11 (LEDC CH4)
 *   - 客厅 B: GPIO12 (LEDC CH5)
 *
 * 依赖模块：
 *   - pwm_driver：提供 Pwm_Init/Set_Duty/Get_Max_Duty 等底层 PWM 操作
 */

#include "LED_Control.h"
#include "pwm_driver.h"
#include "esp_log.h"

/* ======================== 模块静态变量 =========================================== */

static const char *TAG = "LED_CONTROL";  /**< 日志标签 */

/** 每个灯的当前状态数组，索引对应 LED_ID */
static LED_Control_State Led_State_Arr[LED_ID_MAX];

/** 6 个 PWM 通道的 GPIO 和 LEDC 通道配置 */
static const Pwm_Channel_Cfg_t Led_Channel_Cfg[LED_PWM_CHANNEL_COUNT] = {
    {LED_BEDROOM_R_GPIO,    LEDC_CHANNEL_0},  /* 卧室 R */
    {LED_BEDROOM_G_GPIO,    LEDC_CHANNEL_1},  /* 卧室 G */
    {LED_BEDROOM_B_GPIO,    LEDC_CHANNEL_2},  /* 卧室 B */
    {LED_LIVINGROOM_R_GPIO, LEDC_CHANNEL_3},  /* 客厅 R */
    {LED_LIVINGROOM_G_GPIO, LEDC_CHANNEL_4},  /* 客厅 G */
    {LED_LIVINGROOM_B_GPIO, LEDC_CHANNEL_5},  /* 客厅 B */
};

/** PWM 驱动初始化配置 */
static const Pwm_Driver_Cfg_t Led_Pwm_Cfg = {
    .timer_num        = LED_PWM_TIMER,
    .speed_mode       = LED_PWM_SPEED_MODE,
    .freq_hz          = LED_PWM_FREQ_HZ,
    .duty_resolution  = LED_PWM_DUTY_RESOLUTION,
    .channel_count    = LED_PWM_CHANNEL_COUNT,
    .channels         = Led_Channel_Cfg,
};

/* ======================== LED 控制公共 API ======================================== */

/**
 * @brief 初始化 LED 控制模块
 *
 * 初始化 LEDC PWM 定时器（4kHz, 10-bit 分辨率）和 6 个通道。
 * 所有通道初始占空比为 0（灯关闭）。
 * 每个灯的默认状态：关闭、亮度 100%、颜色白色(255,255,255)。
 *
 * @return ESP_OK  初始化成功
 *         ESP_FAIL PWM 驱动初始化失败
 */
esp_err_t LED_Control_Init(void)
{
    /*
     * 初始化 PWM 驱动：
     *   - 1 个 LEDC 定时器（LEDC_TIMER_0），6 个通道共用
     *   - 频率 4kHz，10-bit 分辨率（0~1023）
     *   - 6 个通道分别对应卧室和客厅的 R/G/B
     */
    esp_err_t ret = Pwm_Init(&Led_Pwm_Cfg);
    if(ret != ESP_OK)
    {
        ESP_LOGE(TAG, "LED PWM init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /*
     * 初始化每个灯的默认状态：
     *   - 默认关闭（is_on = false）
     *   - 默认亮度 100%
     *   - 默认颜色白色（255, 255, 255）
     */
    for(int i = 0; i < LED_ID_MAX; i++)
    {
        Led_State_Arr[i].is_on      = false;
        Led_State_Arr[i].brightness = LED_BRIGHTNESS_MAX;
        Led_State_Arr[i].color_r    = LED_COLOR_MAX;
        Led_State_Arr[i].color_g    = LED_COLOR_MAX;
        Led_State_Arr[i].color_b    = LED_COLOR_MAX;
    }

	// // 都关掉
	// LED_Control_State state1 = {
	// 	.is_on      = false,
	// 	.brightness = LED_BRIGHTNESS_MIN,
	// 	.color_r    = LED_BRIGHTNESS_MAX,
	// 	.color_g    = LED_BRIGHTNESS_MAX,
	// 	.color_b    = LED_BRIGHTNESS_MAX,
	// };
	// LED_ID livingroom = { .id = LED_ID_LIVINGROOM, .name = "客厅" };
	// LED_Control_Set_Light(livingroom, state1);
	// LED_ID bedroom = { .id = LED_ID_BEDROOM, .name = "卧室" };
	// LED_Control_Set_Light(bedroom, state1);

    ESP_LOGI(TAG, "LED control initialized, freq=%d Hz, resolution=%d bit, channels=%d",
             LED_PWM_FREQ_HZ, (int)LED_PWM_DUTY_RESOLUTION, LED_PWM_CHANNEL_COUNT);
    return ESP_OK;
}

/**
 * @brief 设置指定灯的颜色和亮度
 *
 * 根据 RGB 颜色值和亮度百分比计算 PWM 占空比。
 *
 * 占空比计算公式（与 note/hw478-led-pwm.md 一致）：
 *   duty = (color_value × brightness × max_duty) / (255 × 100)
 *
 * 三个通道等比缩放，R:G:B 的比例始终不变，
 * 所以颜色不会变，只有亮度变化。
 *
 * 当 is_on 为 false 时，duty = 0（灯关闭）。
 *
 * @param led_id 灯标识（卧室或客厅）
 * @param state  目标状态（开关、亮度、RGB 颜色）
 *
 * @return ESP_OK              设置成功
 *         ESP_ERR_INVALID_ARG  led_id 无效
 *         ESP_FAIL             PWM 设置失败
 */
esp_err_t LED_Control_Set_Light(LED_ID led_id, LED_Control_State state)
{
    /* 参数校验：LED ID 必须在有效范围内 */
    if(led_id.id >= LED_ID_MAX)
    {
        ESP_LOGE(TAG, "Invalid LED ID: %d (max: %d)", led_id.id, LED_ID_MAX - 1);
        return ESP_ERR_INVALID_ARG;
    }

    /* 亮度上限保护：超出最大值则截断到最大值 */
    if(state.brightness > LED_BRIGHTNESS_MAX)
    {
        state.brightness = LED_BRIGHTNESS_MAX;
    }

    /* 保存当前状态，供 LED_Control_Get_Light() 查询 */
    Led_State_Arr[led_id.id] = state;

    /*
     * 计算每个通道的 PWM 占空比并设置。
     *
     * 通道偏移：卧室 = 0（CH0~CH2），客厅 = 3（CH3~CH5）
     * 每个灯占用连续的 3 个通道：R, G, B
     */
    uint32_t maxDuty = Pwm_Get_Max_Duty();
    uint8_t chOffset = led_id.id * 3;
    uint8_t rgb[3] = {state.color_r, state.color_g, state.color_b};

    for(int i = 0; i < 3; i++)
    {
        uint32_t duty = 0;

        if(state.is_on)
        {
            /*
             * 占空比 = (颜色分量 × 亮度百分比 × 最大占空比) / (255 × 100)
             *
             * 示例（10-bit 分辨率，maxDuty=1023）：
             *   暖黄色(255,180,80)，亮度 50%：
             *     R: (255 × 50 × 1023) / (255 × 100) = 511
             *     G: (180 × 50 × 1023) / (255 × 100) = 361
             *     B: (80  × 50 × 1023) / (255 × 100) = 160
             */
            duty = ((uint32_t)rgb[i] * state.brightness * maxDuty)
                 / (LED_COLOR_MAX * LED_BRIGHTNESS_MAX);
        }
        /* is_on 为 false 时 duty 保持 0，灯关闭 */

        esp_err_t ret = Pwm_Set_Duty(chOffset + i, duty);
        if(ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Set duty failed for channel %d: %s",
                     chOffset + i, esp_err_to_name(ret));
            return ret;
        }
    }

    ESP_LOGI(TAG, "%s: %s, R=%d G=%d B=%d, brightness=%d%%",
             led_id.name ? led_id.name : "unknown",
             state.is_on ? "ON" : "OFF",
             state.color_r, state.color_g, state.color_b,
             state.brightness);
    return ESP_OK;
}

/**
 * @brief 获取指定灯的当前状态
 *
 * 返回上一次通过 LED_Control_Set_Light() 设置的状态。
 * 注意：返回的是软件记录的状态，不是从硬件读取的实际状态。
 *
 * @param led_id 灯标识（卧室或客厅）
 * @param state  输出参数，返回当前状态（开关、亮度、RGB 颜色）
 *
 * @return ESP_OK              获取成功
 *         ESP_ERR_INVALID_ARG  led_id 无效或 state 为 NULL
 */
esp_err_t LED_Control_Get_Light(LED_ID led_id, LED_Control_State *state)
{
    /* 参数校验：LED ID 必须在有效范围内 */
    if(led_id.id >= LED_ID_MAX)
    {
        ESP_LOGE(TAG, "Invalid LED ID: %d (max: %d)", led_id.id, LED_ID_MAX - 1);
        return ESP_ERR_INVALID_ARG;
    }

    /* 参数校验：state 指针不能为空 */
    if(state == NULL)
    {
        ESP_LOGE(TAG, "state pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    /* 返回保存的状态副本 */
    *state = Led_State_Arr[led_id.id];
    return ESP_OK;
}