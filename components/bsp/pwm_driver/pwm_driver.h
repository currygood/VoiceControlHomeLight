/**
 * @file pwm_driver.h
 * @brief PWM 驱动接口（基于 ESP32 LEDC 外设）
 *
 * 本模块封装 ESP32 的 LEDC（LED PWM Controller）外设，提供多通道 PWM 输出。
 * 主要用于驱动 RGB LED 灯光（通过调节各通道占空比实现颜色和亮度控制）。
 *
 * 核心功能：
 *   - 多通道 PWM 输出（同一定时器驱动多个通道，频率相同）
 *   - 独立占空比调节（每个通道可独立设置占空比）
 *   - 可配置频率和分辨率
 *
 * 设计说明：
 *   - 使用 LEDC 高速模式（ledc_mode_t），支持高精度 PWM
 *   - 所有通道共享同一个定时器，保证频率一致（RGB LED 需要同步）
 *   - 通过配置结构体传入参数，支持不同硬件配置复用
 *
 * 依赖：
 *   - ESP-IDF LEDC Driver
 */

#ifndef __PWM_DRIVER_H__
#define __PWM_DRIVER_H__

#include <stdint.h>
#include "driver/ledc.h"
#include "esp_err.h"

/* ======================== PWM 通道配置结构体 ===================================== */

/**
 * @brief 单个 PWM 通道的硬件配置
 *
 * 每个通道绑定一个 GPIO 引脚和一个 LEDC 通道。
 * 所有通道共享同一个定时器（频率相同），但可独立调节占空比。
 */
typedef struct {
    gpio_num_t      gpio_num;   /**< GPIO 引脚号（输出 PWM 信号） */
    ledc_channel_t  channel;    /**< LEDC 通道编号 */
} Pwm_Channel_Cfg_t;

/* ======================== PWM 驱动配置结构体 ===================================== */

/**
 * @brief PWM 驱动初始化配置（由调用方传入）
 *
 * 定义 PWM 驱动的全局参数和所有通道的硬件映射。
 * 调用方填充此结构体后传入 Pwm_Init() 完成初始化。
 *
 * 示例（3 通道 RGB LED）：
 *   - timer_num = LEDC_TIMER_0
 *   - speed_mode = LEDC_LOW_SPEED_MODE
 *   - freq_hz = 5000（5kHz 无闪烁）
 *   - duty_resolution = LEDC_TIMER_8_BIT（0-255 精度）
 *   - channel_count = 3（R、G、B 各一个通道）
 *   - channels = { {GPIO_R, LEDC_CHANNEL_0}, {GPIO_G, LEDC_CHANNEL_1}, ... }
 */
typedef struct {
    ledc_timer_t        timer_num;        /**< 定时器编号 */
    ledc_mode_t         speed_mode;       /**< 速度模式（高速/低速） */
    uint32_t            freq_hz;          /**< PWM 频率（Hz） */
    ledc_timer_bit_t    duty_resolution;  /**< 占空比分辨率（如 8-bit = 0-255） */
    uint8_t             channel_count;    /**< 通道数量 */
    const Pwm_Channel_Cfg_t *channels;    /**< 通道配置数组指针 */
} Pwm_Driver_Cfg_t;

/* ======================== API 函数声明 =========================================== */

/**
 * @brief 初始化 PWM 驱动
 *
 * 初始化流程：
 *   1. 配置 LEDC 定时器（频率、分辨率、速度模式）
 *   2. 分配占空比存储数组
 *   3. 逐个配置 LEDC 通道（绑定 GPIO、初始占空比为 0）
 *
 * 注意：初始化后所有通道占空比均为 0，需调用 Pwm_Set_Duty() 设置具体值。
 *
 * @param cfg 驱动配置结构体指针（由调用方传入，决定引脚、通道、频率等）
 * @return ESP_OK             初始化成功
 *         ESP_ERR_INVALID_ARG 参数无效（NULL 指针或通道数为 0）
 *         ESP_ERR_NO_MEM      内存分配失败
 *         ESP_FAIL            定时器或通道配置失败
 */
esp_err_t Pwm_Init(const Pwm_Driver_Cfg_t *cfg);

/**
 * @brief 反初始化 PWM 驱动
 *
 * 停止所有通道的 PWM 输出，复位 GPIO 引脚，释放占空比存储数组。
 * 反初始化后可重新调用 Pwm_Init() 切换配置。
 *
 * @return ESP_OK 反初始化成功
 */
esp_err_t Pwm_Deinit(void);

/**
 * @brief 设置指定通道的占空比（原始值）
 *
 * 将占空比写入 LEDC 硬件并更新输出。占空比范围取决于分辨率配置：
 *   - 8-bit 分辨率：0 ~ 255
 *   - 10-bit 分辨率：0 ~ 1023
 *   - 13-bit 分辨率：0 ~ 8191
 *
 * 超过最大值的 duty 会被自动截断为最大值。
 *
 * @param channel_index 通道索引（0 ~ channel_count-1）
 * @param duty          占空比原始值（0 ~ Pwm_Get_Max_Duty()）
 * @return ESP_OK              设置成功
 *         ESP_ERR_INVALID_STATE 未初始化
 *         ESP_ERR_INVALID_ARG   通道索引超出范围
 *         ESP_FAIL              硬件设置失败
 */
esp_err_t Pwm_Set_Duty(uint8_t channel_index, uint32_t duty);

/**
 * @brief 获取指定通道的当前占空比（原始值）
 *
 * 从内部缓存读取，不访问硬件寄存器。
 *
 * @param channel_index 通道索引
 * @return 占空比原始值（参数无效时返回 0）
 */
uint32_t Pwm_Get_Duty(uint8_t channel_index);

/**
 * @brief 获取最大占空比值
 *
 * 由分辨率决定，计算公式：max_duty = (1 << duty_resolution) - 1。
 * 例如 8-bit 分辨率返回 255。
 *
 * @return 最大占空比原始值（未初始化时返回 0）
 */
uint32_t Pwm_Get_Max_Duty(void);

/**
 * @brief 获取 PWM 频率
 *
 * @return PWM 频率（Hz），未初始化时返回 0
 */
uint32_t Pwm_Get_Frequency(void);

#endif