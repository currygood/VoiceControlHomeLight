#ifndef __I2S_DRIVER_H__
#define __I2S_DRIVER_H__

#include "driver/i2s_std.h"
#include "esp_err.h"

/* ======================== I2S TX 配置（I2S_NUM_1 - 功放播放） ==================== */

/** I2S TX 位时钟 GPIO（Bit Clock，连接到 NS4168 BCLK） */
#define I2S_TX_BCLK_GPIO      8

/** I2S TX 左右声道时钟 GPIO（Left/Right Clock，连接到 NS4168 LRCK） */
#define I2S_TX_LRCK_GPIO      9

/** I2S TX 数据输入 GPIO（Data IN，连接到 NS4168 DIN） */
#define I2S_TX_DIN_GPIO       10

/** 音频采样率（Hz）：16kHz */
#define I2S_TX_SAMPLE_RATE    16000

/** 音频位深度：16-bit */
#define I2S_TX_BIT_DEPTH      16

/** 音频通道数：1（单声道） */
#define I2S_TX_CHANNEL_NUM    1

/** DMA 缓冲区描述符数量：8 个，提供足够的缓冲深度防止音频卡顿 */
#define I2S_TX_DMA_BUF_COUNT  8

/** 每个 DMA 描述符的帧数：256 帧，平衡延迟和缓冲区利用率 */
#define I2S_TX_DMA_BUF_LEN    256

/* ======================== I2S RX 配置（I2S_NUM_0 - INMP441 麦克风） ============== */

/**
 * INMP441 麦克风 I2S 连接：
 * - SCK (Pin 1) -> BCLK GPIO7  (Serial Clock)
 * - WS  (Pin 3) -> LRCK GPIO5  (Word Select)
 * - SD  (Pin 2) -> DOUT GPIO6 (Serial Data Output)
 */
#define I2S_RX_BCLK_GPIO      7
#define I2S_RX_LRCK_GPIO      5
#define I2S_RX_DOUT_GPIO      6

/** 音频采样率（Hz）：16kHz */
#define I2S_RX_SAMPLE_RATE    16000

/** 音频位深度：32-bit（INMP441 输出 24-bit，I2S 以 32-bit 帧对齐） */
#define I2S_RX_BIT_DEPTH      32

/** 音频通道数：1（单声道，只有一个 INMP441 麦克风） */
#define I2S_RX_CHANNEL_NUM    1

/** DMA 缓冲区描述符数量：8 个 */
#define I2S_RX_DMA_BUF_COUNT  8

/** 每个 DMA 描述符的帧数：256 帧 */
#define I2S_RX_DMA_BUF_LEN    256

/**
 * @brief I2S RX 槽位掩码，用于 INMP441 声道选择。
 *
 * INMP441 L/R 引脚（Pin 4）决定麦克风在哪个声道输出数据：
 *   - L/R = GND (LOW)  -> 数据在 LEFT 声道输出
 *                          由于 ESP32 I2S 左右声道交换，实际使用 I2S_STD_SLOT_RIGHT
 *   - L/R = VDD (HIGH) -> 数据在 RIGHT 声道输出，使用 I2S_STD_SLOT_RIGHT
 *
 * 厂商测试代码在 L/R=GND 时使用 I2S_CHANNEL_FMT_ONLY_RIGHT，
 * 等效于新驱动中的 I2S_STD_SLOT_RIGHT。
 */
#define I2S_RX_SLOT_MASK      I2S_STD_SLOT_RIGHT

/* ======================== API 函数 =============================================== */

/* ---------- I2S TX 函数（音频播放，功放） ---------- */

/**
 * @brief 初始化 I2S TX 接口（I2S_NUM_1）
 *
 * 配置 I2S 标准模式作为主机，输出音频到功放芯片。
 * 初始化后 SCK 和 WS 时钟开始输出。
 *
 * @return ESP_OK  初始化成功
 *         ESP_FAIL 通道创建、模式配置或使能失败
 */
esp_err_t I2s_Tx_Init(void);

/**
 * @brief 关闭 I2S TX 接口
 *
 * 禁能 I2S 通道，停止时钟输出，释放 DMA 缓冲区。
 *
 * @return ESP_OK 关闭成功
 */
esp_err_t I2s_Tx_Deinit(void);

/**
 * @brief 通过 I2S TX 发送音频数据
 *
 * 将 PCM 音频数据通过 DMA 传输到 I2S 外设，
 * 发送到功放芯片驱动扬声器。
 *
 * @param buffer        音频数据缓冲区
 * @param size          数据大小（字节）
 * @param bytes_written 输出参数，返回实际写入的字节数
 * @param timeout       超时时间（tick 数），DMA 缓冲区满时等待
 *
 * @return ESP_OK             发送成功
 *         ESP_ERR_INVALID_STATE 接口未初始化
 *         ESP_FAIL            发送失败
 */
esp_err_t I2s_Tx_Write(const uint8_t *buffer, size_t size, size_t *bytes_written, uint32_t timeout);

/**
 * @brief 动态切换 I2S TX 采样率
 *
 * 在不重新初始化通道的情况下更改采样率时钟。
 * 用于 AI 云端播放（24000Hz）和 TTS 播放（16000Hz）之间的切换。
 *
 * @param sample_rate 采样率（Hz），如 16000 或 24000
 * @return ESP_OK 切换成功，ESP_FAIL 失败
 */
esp_err_t I2s_Tx_Set_Sample_Rate(uint32_t sample_rate);

/* ---------- I2S RX 函数（麦克风录音，INMP441） ---------- */

/**
 * @brief 初始化 I2S RX 接口（I2S_NUM_0）
 *
 * 配置 I2S 标准模式作为主机，从 INMP441 麦克风接收音频数据。
 * 初始化后 SCK 和 WS 时钟开始输出。
 *
 * 关键配置：
 *   - 32-bit 位深度（匹配 INMP441 的 24-bit + 8-bit 填充）
 *   - 立体声模式 + 槽位掩码筛选（仅接收 RIGHT 声道数据）
 *   - ws_pol = true（WS 极性反转，匹配 INMP441 时序）
 *   - bit_shift = false（不做位偏移）
 *
 * @return ESP_OK  初始化成功
 *         ESP_FAIL 通道创建、模式配置或使能失败
 */
esp_err_t I2s_Rx_Init(void);

/**
 * @brief 关闭 I2S RX 接口
 *
 * 禁能 I2S 通道，停止时钟输出，释放 DMA 缓冲区。
 *
 * @return ESP_OK 关闭成功
 */
esp_err_t I2s_Rx_Deinit(void);

/**
 * @brief 从 I2S RX 读取音频数据
 *
 * 通过 DMA 从 I2S 外设接收麦克风采集的音频数据到内存缓冲区。
 *
 * @param buffer     接收缓冲区
 * @param size       期望读取的字节数
 * @param bytes_read 输出参数，返回实际读取的字节数
 * @param timeout    超时时间（tick 数），DMA 缓冲区为空时等待
 *
 * @return ESP_OK             读取成功
 *         ESP_ERR_INVALID_STATE 接口未初始化
 *         ESP_FAIL            读取失败
 */
esp_err_t I2s_Rx_Read(uint8_t *buffer, size_t size, size_t *bytes_read, uint32_t timeout);

#endif