#ifndef __LED_CONTROL_H__
#define __LED_CONTROL_H__

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/* ======================== LED ID 宏定义 ========================================== */

#define LED_ID_BEDROOM     0  /**< 卧室灯 ID */
#define LED_ID_LIVINGROOM  1  /**< 客厅灯 ID */
#define LED_ID_MAX         2  /**< 灯的总数量 */

/* ======================== LED GPIO 引脚定义 ======================================== */

/** 卧室灯 - 红色通道 GPIO */
#define LED_BEDROOM_R_GPIO    GPIO_NUM_1
/** 卧室灯 - 绿色通道 GPIO */
#define LED_BEDROOM_G_GPIO    GPIO_NUM_2
/** 卧室灯 - 蓝色通道 GPIO */
#define LED_BEDROOM_B_GPIO    GPIO_NUM_20
/** 客厅灯 - 红色通道 GPIO */
#define LED_LIVINGROOM_R_GPIO GPIO_NUM_21
/** 客厅灯 - 绿色通道 GPIO */
#define LED_LIVINGROOM_G_GPIO GPIO_NUM_17
/** 客厅灯 - 蓝色通道 GPIO */
#define LED_LIVINGROOM_B_GPIO GPIO_NUM_18

/* ======================== LED PWM 配置 ============================================ */

/** PWM 频率：4kHz，远超人眼可见闪烁频率 */
#define LED_PWM_FREQ_HZ          4000
/** PWM 占空比分辨率：10-bit，范围 0~1023 */
#define LED_PWM_DUTY_RESOLUTION  LEDC_TIMER_10_BIT
/** LEDC 定时器编号：6 个通道共用一个 Timer */
#define LED_PWM_TIMER            LEDC_TIMER_0
/** LEDC 速度模式 */
#define LED_PWM_SPEED_MODE       LEDC_LOW_SPEED_MODE
/** PWM 通道总数：卧室 3 路 + 客厅 3 路 */
#define LED_PWM_CHANNEL_COUNT    6

/* ======================== LED 颜色/亮度范围 ======================================== */

/** RGB 颜色分量最大值 */
#define LED_COLOR_MAX            255
/** LED 灯最小颜色值（RGB 分量） */
#define LED_COLOR_MIN            0
/** LED 灯亮度最大值（百分比） */
#define LED_BRIGHTNESS_MAX       100
/** LED 灯亮度最小值（百分比），0 表示关闭 */
#define LED_BRIGHTNESS_MIN       0

/* ======================== 数据结构 ================================================ */

/**
 * @brief LED 灯标识
 *
 * 通过 id 和 name 唯一标识一个灯（卧室灯或客厅灯）。
 * id 用于内部索引，name 用于日志输出。
 */
typedef struct
{
    uint8_t     id;    /**< LED 灯编号（0=卧室, 1=客厅） */
    const char *name;  /**< LED 灯名称（如 "卧室"、"客厅"） */
} LED_ID;

/**
 * @brief LED 灯控制状态
 *
 * 包含开关状态、亮度和 RGB 颜色值。
 * 颜色由 RGB 三个分量决定，不限制为预定义颜色。
 * 亮度通过等比例缩放 RGB 值实现，颜色比例不变。
 */
typedef struct
{
    bool    is_on;      /**< 开关状态：true=开, false=关 */
    uint8_t brightness; /**< 亮度百分比（0~100），等比例缩放 RGB */
    uint8_t color_r;    /**< 红色分量（0~255） */
    uint8_t color_g;    /**< 绿色分量（0~255） */
    uint8_t color_b;    /**< 蓝色分量（0~255） */
} LED_Control_State;

/* ======================== API 函数 ================================================ */

/**
 * @brief 初始化 LED 控制模块
 *
 * 初始化 LEDC PWM 定时器和 6 个通道（卧室 3 路 + 客厅 3 路），
 * 所有通道初始占空比为 0（灯关闭）。
 *
 * @return ESP_OK  初始化成功
 *         ESP_FAIL PWM 驱动初始化失败
 */
esp_err_t LED_Control_Init(void);

/**
 * @brief 设置指定灯的颜色和亮度
 *
 * 根据 RGB 颜色值和亮度百分比计算 PWM 占空比，控制灯的开关、颜色和亮度。
 *
 * 占空比计算公式：
 *   duty = (color_value × brightness × max_duty) / (255 × 100)
 *
 * 当 is_on 为 false 时，duty = 0（灯关闭）。
 * 亮度等比例缩放 RGB 三个分量，保持颜色比例不变。
 *
 * @param led_id 灯标识（卧室或客厅）
 * @param state  目标状态（开关、亮度、RGB 颜色）
 *
 * @return ESP_OK              设置成功
 *         ESP_ERR_INVALID_ARG  led_id 无效
 *         ESP_FAIL             PWM 设置失败
 */
esp_err_t LED_Control_Set_Light(LED_ID led_id, LED_Control_State state);

/**
 * @brief 获取指定灯的当前状态
 *
 * 读取上一次通过 LED_Control_Set_Light() 设置的状态，
 * 包括开关状态、亮度和 RGB 颜色值。
 *
 * @param led_id 灯标识（卧室或客厅）
 * @param state  输出参数，返回当前状态
 *
 * @return ESP_OK              获取成功
 *         ESP_ERR_INVALID_ARG  led_id 无效或 state 为 NULL
 */
esp_err_t LED_Control_Get_Light(LED_ID led_id, LED_Control_State *state);

#endif
