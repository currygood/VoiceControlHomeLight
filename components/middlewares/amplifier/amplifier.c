/**
 * @file amplifier.c
 * @brief 功放模块实现（基于 NS4168 D 类功放芯片）
 *
 * 本模块封装 NS4168 功放芯片的 I2S 音频输出功能。NS4168 是一款
 * 5W 单声道 D 类功放芯片，通过 I2S 标准协议接收 PCM 音频数据，
 * 驱动扬声器发声。
 *
 * 核心功能：
 *   - 初始化：配置 I2S TX 接口（I2S_NUM_1），启动时钟输出
 *   - 播放：将 PCM 16-bit 单声道音频数据通过 I2S 发送到 NS4168
 *   - 音量控制：保存音量值（软件层面，NS4168 本身不支持 I2C 音量调节）
 *
 * 硬件连接（I2S TX）：
 *   - BCLK  GPIO8  （I2S 位时钟，Bit Clock）
 *   - LRCK  GPIO9  （I2S 左右声道时钟，Left/Right Clock）
 *   - DIN   GPIO10 （I2S 数据输入，Data IN）
 *
 * 依赖模块：
 *   - i2s_driver：提供 I2s_Tx_Init/Deinit/Write 等底层 I2S 操作
 */

#include "amplifier.h"
#include "i2s_driver.h"
#include "esp_log.h"

/* ======================== 模块静态变量 =========================================== */

static const char *TAG = "AMPLIFIER";  /**< 日志标签 */

/** 当前音量值（0~100），默认 80% */
static uint8_t Amplifier_Volume = 80;

/* ======================== 功放公共 API =========================================== */

/**
 * @brief 初始化功放模块
 *
 * 初始化 I2S TX 接口作为音频输出通道。
 * NS4168 功放芯片通过 I2S 标准协议接收 PCM 数据，
 * 因此初始化过程就是配置 I2S TX 接口。
 *
 * @return ESP_OK  初始化成功（I2S TX 接口就绪）
 *         ESP_FAIL I2S TX 初始化失败
 */
esp_err_t Amplifier_Init(void)
{
    /*
     * 初始化 I2S TX（I2S_NUM_1）接口。
     * I2s_Tx_Init() 会配置：
     *   - 采样率 16kHz、16-bit、单声道
     *   - DMA 缓冲区（8 个描述符，每个 256 帧）
     *   - GPIO 引脚（BCLK=8, LRCK=9, DIN=10）
     *   - 主机模式（ESP32 生成 BCLK 和 LRCK 时钟）
     */
    esp_err_t ret = I2s_Tx_Init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Amplifier I2S TX init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "NS4168 amplifier initialized, volume=%d%%", Amplifier_Volume);
    return ESP_OK;
}

/**
 * @brief 反初始化功放模块
 *
 * 关闭 I2S TX 接口，停止时钟输出，释放 DMA 缓冲区。
 * 反初始化后功放芯片不再接收音频数据，扬声器静音。
 *
 * @return ESP_OK 反初始化成功
 */
esp_err_t Amplifier_Deinit(void)
{
    esp_err_t ret = I2s_Tx_Deinit();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Amplifier deinit failed: %s", esp_err_to_name(ret));
    }
    else
    {
        ESP_LOGI(TAG, "NS4168 amplifier deinitialized");
    }
    return ret;
}

/**
 * @brief 通过功放播放 PCM 音频缓冲区
 *
 * 将 PCM 16-bit 单声道音频数据通过 I2S DMA 发送到 NS4168 功放芯片。
 * 底层使用 i2s_channel_write() 进行 DMA 传输，数据直接从内存搬运到
 * I2S 外设，不占用 CPU。
 *
 * 数据格式要求：
 *   - 采样率：16kHz
 *   - 位深度：16-bit（小端字节序）
 *   - 通道数：1（单声道）
 *
 * @param buffer        指向 PCM 音频数据的指针
 * @param size          缓冲区大小（字节）
 * @param bytes_written 输出参数，返回实际写入的字节数
 * @param timeout       超时时间（FreeRTOS tick 数），若 DMA 缓冲区满则等待
 *
 * @return ESP_OK             播放成功
 *         ESP_ERR_INVALID_ARG buffer 为 NULL 或 size 为 0
 *         ESP_FAIL            I2S 写入失败
 */
esp_err_t Amplifier_Play_Buffer(const uint8_t *buffer, size_t size, size_t *bytes_written, uint32_t timeout)
{
    /* 参数校验：缓冲区不能为空，大小不能为 0 */
    if (buffer == NULL || size == 0)
    {
        ESP_LOGE(TAG, "Invalid buffer parameters");
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * 将 PCM 数据写入 I2S TX 发送缓冲区。
     * i2s_channel_write() 内部使用 DMA 将数据从内存搬运到 I2S 外设，
     * 如果 DMA 缓冲区已满，会阻塞等待直到有空间或超时。
     */
    esp_err_t ret = I2s_Tx_Write(buffer, size, bytes_written, timeout);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Play buffer failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

/**
 * @brief 设置功放音量
 *
 * 当前实现仅保存音量值到静态变量，未实现硬件音量控制。
 * 原因：NS4168 功放芯片不支持 I2C/SPI 软件音量调节，
 * 音量只能通过外部电位器或 PWM 控制功放使能引脚来实现。
 *
 * @param volume 音量值（0~100），超出范围自动截断到 100
 *
 * @return ESP_OK 设置成功
 */
esp_err_t Amplifier_Set_Volume(uint8_t volume)
{
    /* 音量上限保护：超出最大值则截断到最大值 */
    if (volume > AMPLIFIER_VOLUME_MAX)
    {
        volume = AMPLIFIER_VOLUME_MAX;
    }

    Amplifier_Volume = volume;
    ESP_LOGI(TAG, "Volume set to %d%%", Amplifier_Volume);
    return ESP_OK;
}

/**
 * @brief 获取当前音量值
 *
 * @return 当前音量值（0~100），默认 80
 */
uint8_t Amplifier_Get_Volume(void)
{
    return Amplifier_Volume;
}