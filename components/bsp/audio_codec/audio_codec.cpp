/**
 * @file audio_codec.cpp
 * @brief OGG_OPUS 音频解码器实现
 *
 * 本模块封装了 micro_opus 库的 OggOpusDecoder，提供 C 兼容接口，
 * 用于将云端 AI 下发的 OGG_OPUS 格式音频数据解码为 PCM 16-bit 格式。
 *
 * 核心流程：
 *   1. 创建解码器：通过 new 分配 OggDecoder 实例（内部包含 OggOpusDecoder）
 *   2. 喂入数据：调用 feed() 将 OGG_OPUS 帧送入解码器，获取 PCM 输出
 *   3. 销毁解码器：通过 delete 释放解码器及相关资源
 *
 * 解码器特性：
 *   - 支持流式解码：可分多次喂入连续的 OGG_OPUS 数据流
 *   - 内部维护状态：解码器自动处理 OGG 容器和 Opus 帧的边界
 *   - 输出格式：PCM 16-bit 单声道
 *
 * 依赖：
 *   - micro_opus 库：提供 OGG_OPUS 解码核心实现
 */

#include "audio_codec.h"
#include "micro_opus/ogg_opus_decoder.h"
#include <new>

/* ======================== 解码器结构体定义 ======================================= */

/**
 * @brief OGG_OPUS 解码器内部结构
 *
 * 封装 micro_opus::OggOpusDecoder 实例。
 * 通过不透明指针（OggDecoder*）对外暴露，隐藏 C++ 实现细节。
 */
struct OggDecoder
{
    micro_opus::OggOpusDecoder decoder;  /**< micro_opus 库的 OGG_OPUS 解码器实例 */
};

/* ======================== 公共 API =============================================== */

/**
 * @brief 创建 OGG_OPUS 解码器实例
 *
 * 通过 C++ new 操作符在堆上分配 OggDecoder 实例，
 * 内部自动调用 OggOpusDecoder 的默认构造函数完成初始化。
 *
 * @return 非 NULL  解码器句柄
 *         NULL    内存分配失败（new 抛出 std::bad_alloc 异常）
 */
OggDecoder* AudioCodec_ogg_decoder_create(void)
{
    OggDecoder *d = new OggDecoder();
    return d;
}

/**
 * @brief 销毁 OGG_OPUS 解码器实例
 *
 * 通过 C++ delete 操作符释放解码器内存，
 * 内部自动调用 OggOpusDecoder 的析构函数释放 Opus 解码器状态和缓冲区。
 *
 * @param d 解码器句柄（由 AudioCodec_ogg_decoder_create 创建）
 */
void AudioCodec_ogg_decoder_destroy(OggDecoder *d)
{
    delete d;
}

/**
 * @brief 喂入 OGG_OPUS 数据并解码为 PCM
 *
 * 将 OGG_OPUS 编码的音频数据块送入解码器，解码为 PCM 16-bit 格式。
 *
 * 解码流程：
 *   1. 调用 OggOpusDecoder::decode() 处理输入数据
 *   2. 获取实际消费的输入字节数（通过 bytes_consumed）
 *   3. 获取解码出的 PCM 样本数（通过 samples_decoded）
 *   4. 将消费字节数写入 consumed 输出参数
 *   5. 根据解码结果返回样本数或错误码
 *
 * 返回值说明：
 *   - 正数：成功解码，返回 PCM 样本数（int16_t 个数）
 *   - 0：数据已消费但尚未产生 PCM 输出（需要继续喂入更多数据）
 *   - -1：解码失败（数据损坏、格式错误等）
 *
 * @param d            解码器句柄
 * @param data         输入数据指针（OGG_OPUS 编码数据）
 * @param len          输入数据长度（字节）
 * @param pcm_out      输出缓冲区（PCM 16-bit，调用方分配）
 * @param pcm_capacity 输出缓冲区容量（int16_t 样本数）
 * @param consumed     [输出] 实际消费的输入字节数
 *
 * @return >0  成功解码的 PCM 样本数
 *         =0  数据已消费但尚未产生 PCM 输出
 *         -1  解码失败
 */
int AudioCodec_ogg_decoder_feed(OggDecoder *d, const uint8_t *data, size_t len,
                     int16_t *pcm_out, int pcm_capacity,
                     size_t *consumed)
{
    size_t bytes_consumed = 0;
    size_t samples_decoded = 0;

    /*
     * 调用 micro_opus 解码器。
     * 输出缓冲区类型转换：int16_t* → uint8_t*，
     * 容量转换：样本数 → 字节数（pcm_capacity * sizeof(int16_t)）。
     */
    micro_opus::OggOpusResult result = d->decoder.decode(
        data, len,
        reinterpret_cast<uint8_t *>(pcm_out),
        (size_t)pcm_capacity * sizeof(int16_t),
        bytes_consumed, samples_decoded);

    /* 将消费字节数写入输出参数 */
    *consumed = bytes_consumed;

    if (result == micro_opus::OGG_OPUS_OK)
    {
        if (samples_decoded > 0)
        {
            /* 解码成功且有 PCM 输出 */
            return (int)samples_decoded;
        }
        /* 解码成功但无 PCM 输出（需要更多输入数据） */
        return 0;
    }

    /* 解码失败 */
    return -1;
}