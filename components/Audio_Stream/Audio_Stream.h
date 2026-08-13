#ifndef __AUDIO_STREAM_H__
#define __AUDIO_STREAM_H__

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

#define AUDIO_STREAM_MAX_READERS 4

typedef void* AudioStream_ReaderHandle_t;

/**
 * @brief 初始化音频流模块
 *
 * 初始化麦克风硬件，创建环形缓冲区。
 * 注意：仅完成初始化，不会启动数据采集，需调用 AudioStream_Start() 启动生产者任务。
 *
 * @param buffer_size_samples 环形缓冲区大小（采样数），如 32000 表示 2 秒 @ 16kHz
 * @return ESP_OK  初始化成功
 *         ESP_FAIL 麦克风初始化失败或内存分配失败
 */
esp_err_t AudioStream_Init(size_t buffer_size_samples);

/**
 * @brief 反初始化音频流模块
 *
 * 停止生产者任务，释放环形缓冲区，关闭麦克风。
 *
 * @return ESP_OK 反初始化成功
 */
esp_err_t AudioStream_Deinit(void);

/**
 * @brief 注册一个消费者（Reader）
 *
 * 每个消费者调用此函数获取独立的读句柄，后续通过该句柄读取音频数据。
 * 最大支持 AUDIO_STREAM_MAX_READERS 个消费者。
 *
 * @return 非 NULL 注册成功，返回读句柄
 *         NULL    已达到最大消费者数量
 */
AudioStream_ReaderHandle_t AudioStream_Reader_Register(void);

/**
 * @brief 注销一个消费者
 *
 * @param handle 读句柄
 * @return ESP_OK 注销成功
 */
esp_err_t AudioStream_Reader_Unregister(AudioStream_ReaderHandle_t handle);

/**
 * @brief 从音频流中读取 PCM 16-bit 数据
 *
 * 从环形缓冲区中读取指定数量的采样数据。如果数据不足，会阻塞等待
 * 直到超时或有新数据到达。
 *
 * @param handle       读句柄
 * @param buffer       接收缓冲区（int16_t 数组）
 * @param sample_count 期望读取的采样数
 * @param samples_read 输出参数，返回实际读取的采样数
 * @param timeout_ms   超时时间（毫秒）
 *
 * @return ESP_OK             读取成功
 *         ESP_ERR_INVALID_ARG 参数无效
 *         ESP_ERR_TIMEOUT     超时，未读取到足够数据
 */
esp_err_t AudioStream_Read(AudioStream_ReaderHandle_t handle, int16_t *buffer,
                           size_t sample_count, size_t *samples_read, uint32_t timeout_ms);

/**
 * @brief 启动音频流生产者任务
 *
 * 创建 FreeRTOS 任务，开始从麦克风持续采集音频数据并写入环形缓冲区。
 *
 * @return ESP_OK  启动成功
 *         ESP_FAIL 任务创建失败
 */
esp_err_t AudioStream_Start(void);

/**
 * @brief 停止音频流生产者任务
 *
 * 停止麦克风数据采集，但保留环形缓冲区和已注册的消费者。
 *
 * @return ESP_OK 停止成功
 */
esp_err_t AudioStream_Stop(void);

/* ======================== 播放流（多生产者）API =================================== */

/**
 * @brief 播放数据包类型
 *
 * 播放流是一段有状态的 OGG Opus 音频流。调用方通过写入带类型标记的
 * 数据包，让播放任务知道何时创建/销毁解码器。
 */
typedef enum
{
    AUDIO_PLAYBACK_START = 0,   /**< 标记新 OGG 音频流开始（创建解码器） */
    AUDIO_PLAYBACK_DATA,        /**< OGG 音频数据块（原始 Opus 字节） */
    AUDIO_PLAYBACK_END,         /**< 标记 OGG 音频流结束（销毁解码器） */
} AudioPlayback_Type_t;

/**
 * @brief 向播放流写入一个数据包
 *
 * 将带类型标记的音频数据写入播放队列，由内部播放任务负责解码（OGG Opus
 * → PCM 16k）并播放。数据在函数内部被拷贝，调用方无需保持 data 有效。
 *
 * 多生产者安全：函数内部加锁/使用队列，可被多个任务并发调用（如 AI 云端、
 * 本地 TTS、提示音等）。
 *
 * @param type 数据包类型（START / DATA / END）
 * @param data 音频数据指针（START/END 时可为 NULL）
 * @param len  音频数据长度（字节，START/END 时为 0）
 *
 * @return ESP_OK              写入成功
 *         ESP_ERR_INVALID_STATE 播放流未初始化
 *         ESP_ERR_NO_MEM       内存分配失败
 *         ESP_ERR_TIMEOUT      队列满，数据被丢弃
 */
esp_err_t AudioStream_Playback_Write(AudioPlayback_Type_t type, const uint8_t *data, size_t len);

#endif