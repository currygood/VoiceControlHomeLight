/**
 * @file pwm_driver.c
 * @brief PWM 驱动实现（基于 ESP32 LEDC 外设）
 *
 * 本模块封装 ESP32 的 LEDC（LED PWM Controller）外设，提供多通道 PWM 输出。
 * 主要用于驱动 RGB LED 灯光，通过调节各通道（R/G/B）占空比实现颜色和亮度控制。
 *
 * 核心流程：
 *   1. 初始化：配置 LEDC 定时器 → 分配占空比缓存 → 逐个配置 LEDC 通道
 *   2. 设置占空比：写入 LEDC 硬件寄存器 → 更新 PWM 输出
 *   3. 反初始化：停止所有通道 → 复位 GPIO → 释放缓存
 *
 * 设计说明：
 *   - 所有通道共享同一个 LEDC 定时器，保证 PWM 频率一致（RGB 同步需要）
 *   - 占空比本地缓存（Pwm_Duty_Arr[]），避免频繁读取硬件寄存器
 *   - 通过配置结构体传入参数，同一套代码适配不同 GPIO 和通道组合
 *
 * 依赖：
 *   - ESP-IDF LEDC Driver
 */

#include "pwm_driver.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include <stdlib.h>

/* ======================== 模块静态变量 =========================================== */

static const char *TAG = "PWM_DRIVER";       /**< 日志标签 */

/** 当前使用的 PWM 配置（由 Pwm_Init 保存，反初始化时置 NULL） */
static const Pwm_Driver_Cfg_t *Pwm_Cfg = NULL;

/** 占空比缓存数组，每个通道一个 uint32_t 元素，避免频繁读取硬件寄存器 */
static uint32_t *Pwm_Duty_Arr = NULL;

/** 模块初始化标志：true = 已完成初始化，false = 未初始化或已反初始化 */
static bool Pwm_Initialized = false;

/* ======================== 初始化和反初始化 ======================================= */

/**
 * @brief 初始化 PWM 驱动
 *
 * 初始化流程（共 3 步）：
 *   1. 配置 LEDC 定时器（频率、分辨率、速度模式）
 *   2. 分配占空比缓存数组（calloc 分配，初始值全为 0）
 *   3. 逐个配置 LEDC 通道（绑定 GPIO 引脚，初始占空比设为 0）
 *
 * 任意步骤失败都会回滚已分配的资源（释放内存、不保存配置），
 * 保证调用方可以安全重试。
 *
 * @param cfg 驱动配置结构体指针
 * @return ESP_OK             初始化成功
 *         ESP_ERR_INVALID_ARG 参数无效
 *         ESP_ERR_NO_MEM      内存分配失败
 *         ESP_FAIL            定时器或通道配置失败
 */
esp_err_t Pwm_Init(const Pwm_Driver_Cfg_t *cfg)
{
    /* 参数有效性检查 */
    if (cfg == NULL || cfg->channels == NULL || cfg->channel_count == 0)
    {
        ESP_LOGE(TAG, "无效的配置参数");
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * 步骤 1：配置 LEDC 定时器。
     * 定时器决定所有通道的 PWM 频率和占空比分辨率。
     * clk_cfg = LEDC_AUTO_CLK 让驱动自动选择最佳时钟源。
     */
    ledc_timer_config_t timerConfig = {
        .speed_mode = cfg->speed_mode,
        .duty_resolution = cfg->duty_resolution,
        .timer_num = cfg->timer_num,
        .freq_hz = cfg->freq_hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };

    esp_err_t ret = ledc_timer_config(&timerConfig);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "LEDC 定时器配置失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /*
     * 步骤 2：分配占空比缓存数组。
     * 使用 calloc 确保初始值为 0，与硬件初始占空比一致。
     */
    Pwm_Duty_Arr = (uint32_t *)calloc(cfg->channel_count, sizeof(uint32_t));
    if (Pwm_Duty_Arr == NULL)
    {
        ESP_LOGE(TAG, "内存分配失败");
        return ESP_ERR_NO_MEM;
    }

    /*
     * 步骤 3：逐个配置 LEDC 通道。
     * 每个通道绑定一个 GPIO 引脚，共享同一定时器。
     * 初始占空比设为 0（LED 熄灭状态）。
     */
    for (uint8_t i = 0; i < cfg->channel_count; i++)
    {
        ledc_channel_config_t chanConfig = {
            .gpio_num = cfg->channels[i].gpio_num,
            .speed_mode = cfg->speed_mode,
            .channel = cfg->channels[i].channel,
            .timer_sel = cfg->timer_num,
            .duty = 0,          /* 初始占空比：0（熄灭） */
            .hpoint = 0,        /* 无相位偏移 */
        };

        ret = ledc_channel_config(&chanConfig);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "LEDC 通道 %d 配置失败: %s", i, esp_err_to_name(ret));
            /* 配置失败，回滚：释放已分配的缓存 */
            free(Pwm_Duty_Arr);
            Pwm_Duty_Arr = NULL;
            return ret;
        }

        /* 缓存初始占空比 */
        Pwm_Duty_Arr[i] = 0;
    }

    /* 保存配置，标记初始化完成 */
    Pwm_Cfg = cfg;
    Pwm_Initialized = true;
    ESP_LOGI(TAG, "PWM 初始化成功: 频率=%lu Hz, 分辨率=%d bit, 通道数=%d",
             (unsigned long)cfg->freq_hz, (int)cfg->duty_resolution, cfg->channel_count);
    return ESP_OK;
}

/**
 * @brief 反初始化 PWM 驱动
 *
 * 操作顺序：
 *   1. 停止所有通道的 PWM 输出（ledc_stop）
 *   2. 复位 GPIO 引脚为默认状态（gpio_reset_pin）
 *   3. 释放占空比缓存数组
 *   4. 清除配置指针和初始化标志
 *
 * 反初始化后可重新调用 Pwm_Init() 切换配置。
 * 如果未初始化则直接返回 ESP_OK（幂等操作）。
 *
 * @return ESP_OK 反初始化成功
 */
esp_err_t Pwm_Deinit(void)
{
    /* 未初始化，直接返回（幂等） */
    if (!Pwm_Initialized)
    {
        return ESP_OK;
    }

    /* 逐个停止通道并复位 GPIO */
    for (uint8_t i = 0; i < Pwm_Cfg->channel_count; i++)
    {
        ledc_stop(Pwm_Cfg->speed_mode, Pwm_Cfg->channels[i].channel, 0);
        gpio_reset_pin(Pwm_Cfg->channels[i].gpio_num);
    }

    /* 释放占空比缓存 */
    free(Pwm_Duty_Arr);
    Pwm_Duty_Arr = NULL;
    Pwm_Cfg = NULL;
    Pwm_Initialized = false;
    ESP_LOGI(TAG, "PWM 已反初始化");
    return ESP_OK;
}

/* ======================== 占空比操作 ============================================= */

/**
 * @brief 设置指定通道的占空比（原始值）
 *
 * 操作流程：
 *   1. 检查初始化状态和通道索引有效性
 *   2. 截断超出最大值的 duty
 *   3. 更新本地缓存（Pwm_Duty_Arr[]）
 *   4. 写入 LEDC 硬件寄存器（ledc_set_duty）
 *   5. 更新 PWM 输出（ledc_update_duty）
 *
 * 注意：ledc_set_duty 和 ledc_update_duty 必须成对调用，
 * 前者写入新值，后者使新值生效。如果不调用 update，输出不会变化。
 *
 * @param channel_index 通道索引（0 ~ channel_count-1）
 * @param duty          占空比原始值（0 ~ Pwm_Get_Max_Duty()）
 * @return ESP_OK              设置成功
 *         ESP_ERR_INVALID_STATE 未初始化
 *         ESP_ERR_INVALID_ARG   通道索引超出范围
 *         ESP_FAIL              硬件设置失败
 */
esp_err_t Pwm_Set_Duty(uint8_t channel_index, uint32_t duty)
{
    /* 初始化状态检查 */
    if (!Pwm_Initialized)
    {
        ESP_LOGE(TAG, "PWM 未初始化, 请先调用 Pwm_Init()");
        return ESP_ERR_INVALID_STATE;
    }

    /* 通道索引有效性检查 */
    if (channel_index >= Pwm_Cfg->channel_count)
    {
        ESP_LOGE(TAG, "通道索引超出范围: %d (最大: %d)",
                 channel_index, Pwm_Cfg->channel_count - 1);
        return ESP_ERR_INVALID_ARG;
    }

    /* 截断超出最大值的占空比 */
    uint32_t maxDuty = Pwm_Get_Max_Duty();
    if (duty > maxDuty)
    {
        duty = maxDuty;
    }

    /* 更新本地缓存 */
    Pwm_Duty_Arr[channel_index] = duty;

    /* 写入 LEDC 硬件寄存器 */
    esp_err_t ret = ledc_set_duty(Pwm_Cfg->speed_mode,
                                  Pwm_Cfg->channels[channel_index].channel, duty);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "通道 %d 设置占空比失败: %s", channel_index, esp_err_to_name(ret));
        return ret;
    }

    /* 更新 PWM 输出，使新占空比生效 */
    ret = ledc_update_duty(Pwm_Cfg->speed_mode,
                           Pwm_Cfg->channels[channel_index].channel);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "通道 %d 更新占空比失败: %s", channel_index, esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

/**
 * @brief 获取指定通道的当前占空比（原始值）
 *
 * 从本地缓存 Pwm_Duty_Arr[] 读取，不访问硬件寄存器。
 * 如果未初始化或索引无效，返回 0。
 *
 * @param channel_index 通道索引
 * @return 占空比原始值
 */
uint32_t Pwm_Get_Duty(uint8_t channel_index)
{
    if (!Pwm_Initialized || channel_index >= Pwm_Cfg->channel_count)
    {
        return 0;
    }
    return Pwm_Duty_Arr[channel_index];
}

/**
 * @brief 获取最大占空比值
 *
 * 由占空比分辨率决定，计算公式：
 *   max_duty = (1 << duty_resolution) - 1
 *
 * 示例：
 *   - LEDC_TIMER_8_BIT  → 255
 *   - LEDC_TIMER_10_BIT → 1023
 *   - LEDC_TIMER_13_BIT → 8191
 *
 * @return 最大占空比原始值（未初始化时返回 0）
 */
uint32_t Pwm_Get_Max_Duty(void)
{
    if (!Pwm_Initialized)
    {
        return 0;
    }
    return ((uint32_t)1 << Pwm_Cfg->duty_resolution) - 1;
}

/**
 * @brief 获取 PWM 频率
 *
 * 返回配置时设置的频率值，未初始化时返回 0。
 *
 * @return PWM 频率（Hz）
 */
uint32_t Pwm_Get_Frequency(void)
{
    if (!Pwm_Initialized)
    {
        return 0;
    }
    return Pwm_Cfg->freq_hz;
}