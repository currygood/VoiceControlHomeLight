/**
 * @file audio_codec.h
 * @brief OGG_OPUS 音频解码器接口
 *
 * 本模块封装了 micro_opus 库的 OggOpusDecoder，提供 C 兼容接口，
 * 用于将云端 AI 下发的 OGG_OPUS 格式音频数据解码为 PCM 16-bit 格式，
 * 供播放模块输出到扬声器。
 *
 * 音频数据流向：
 *   云端 AI → WebSocket → Base64 解码 → OGG_OPUS 帧 → 本解码器 → PCM 16-bit → 扬声器
 *
 * 依赖：
 *   - micro_opus 库：提供 OGG_OPUS 解码核心实现
 *
 * 注意：本模块使用 C++ 实现，但通过 extern "C" 提供 C 兼容接口，
 *       确保可被项目中其他 C 模块调用。
 */

#ifndef AUDIO_CODEC_H
#define AUDIO_CODEC_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** OGG_OPUS 解码器不透明句柄（内部为 micro_opus::OggOpusDecoder 实例） */
typedef struct OggDecoder OggDecoder;

/**
 * @brief 创建 OGG_OPUS 解码器实例
 *
 * 分配并初始化一个新的 OggOpusDecoder 实例。
 * 使用完毕后需调用 AudioCodec_ogg_decoder_destroy() 释放资源。
 *
 * @return 非 NULL  解码器句柄
 *         NULL    内存分配失败
 */
OggDecoder* AudioCodec_ogg_decoder_create(void);

/**
 * @brief 销毁 OGG_OPUS 解码器实例
 *
 * 释放解码器占用的所有资源（内部 Opus 解码器状态、缓冲区等）。
 *
 * @param d 解码器句柄（由 AudioCodec_ogg_decoder_create 创建）
 */
void AudioCodec_ogg_decoder_destroy(OggDecoder *d);

/**
 * @brief 喂入 OGG_OPUS 数据并解码为 PCM
 *
 * 将 OGG_OPUS 编码的音频数据块解码为 PCM 16-bit 格式。
 * 解码器内部维护状态，支持流式输入（可分多次喂入数据）。
 *
 * 调用约定：
 *   - 可多次调用 feed() 喂入连续的 OGG_OPUS 数据流
 *   - 每次调用可能消费部分输入数据，也可能解码出部分 PCM 样本
 *   - 通过 consumed 参数获取实际消费的输入字节数，便于调用方管理缓冲区
 *
 * @param d            解码器句柄
 * @param data         输入数据指针（OGG_OPUS 编码数据）
 * @param len          输入数据长度（字节）
 * @param pcm_out      输出缓冲区（PCM 16-bit，调用方分配）
 * @param pcm_capacity 输出缓冲区容量（int16_t 样本数，即最大可存放的采样点数）
 * @param consumed     [输出] 实际消费的输入字节数
 *
 * @return >0  成功解码的 PCM 样本数
 *         =0  数据已消费但尚未产生 PCM 输出（需继续喂入更多数据）
 *         -1  解码失败（数据损坏或格式错误）
 */
int AudioCodec_ogg_decoder_feed(OggDecoder *d, const uint8_t *data, size_t len,
                     int16_t *pcm_out, int pcm_capacity,
                     size_t *consumed);

#ifdef __cplusplus
}
#endif

#endif