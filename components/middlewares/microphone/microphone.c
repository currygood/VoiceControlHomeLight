/**
 * @file microphone.c
 * @brief 麦克风模块实现（基于 INMP441 MEMS 麦克风）
 *
 * 本模块封装 INMP441 MEMS 麦克风的 I2S 音频采集功能。INMP441 是一款
 * 高性能、低功耗、24-bit 数字输出的 MEMS 麦克风，通过 I2S 标准协议
 * 输出音频数据。
 *
 * 核心功能：
 *   - 初始化：配置 I2S RX 接口（I2S_NUM_0），启动 INMP441 硬件
 *   - 原始数据读取：从 I2S 读取 32-bit 原始采样数据
 *   - PCM 16-bit 转换：将 32-bit 原始数据转换为标准 16-bit PCM 格式
 *
 * 硬件连接（I2S RX）：
 *   - SCK  (Pin 1) -> BCLK GPIO7  （I2S 位时钟，Bit Clock）
 *   - WS   (Pin 3) -> LRCK GPIO5  （I2S 左右声道时钟，Word Select）
 *   - SD   (Pin 2) -> DOUT GPIO6  （I2S 数据输出，Serial Data）
 *   - L/R  (Pin 4) -> GND         （声道选择：LEFT 声道）
 *   - CHIPEN(Pin 8) -> VDD        （芯片使能：始终使能）
 *
 * INMP441 关键特性：
 *   - 24-bit 数字输出，I2S 标准以 32-bit 帧对齐
 *   - 全向拾音，信噪比 61dBA
 *   - 频率响应 60Hz ~ 15kHz
 *   - 需要 SCK/WS 时钟运行才能正常工作
 *   - 首次上电需要 2^18 SCK 周期（约 256ms）的稳定时间
 *
 * 依赖模块：
 *   - i2s_driver：提供 I2s_Rx_Init/Deinit/Read 等底层 I2S 操作
 */

#include "microphone.h"
#include "i2s_driver.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "MICROPHONE"  /**< 日志标签 */

/* ======================== INMP441 硬件控制 ======================================= */

/**
 * @brief 配置 SD（DOUT）数据引脚内部下拉
 *
 * 根据 INMP441 数据手册：当麦克风不驱动数据时（例如在另一声道的
 * 时隙中），SD 引脚处于三态（高阻态）。如果没有下拉电阻，SD 线
 * 可能悬空并产生随机噪声。
 *
 * 推荐使用 100kΩ 外部下拉电阻，这里使用 ESP32 内部约 45kΩ 下拉
 * 作为软件备用方案。
 *
 * 使用 gpio_set_pull_mode() 而非 gpio_config()，以免干扰 I2S 引脚功能。
 * 该函数只改变引脚的上下拉模式，不影响 I2S 外设的引脚配置。
 */
static void Inmp441_Sd_Pulldown_Enable(void)
{
    gpio_set_pull_mode(I2S_RX_DOUT_GPIO, GPIO_PULLDOWN_ONLY);
    ESP_LOGI(TAG, "SD pin GPIO%d pull-down enabled", I2S_RX_DOUT_GPIO);
}

/**
 * @brief 使能 INMP441 麦克风硬件
 *
 * 根据 CHIPEN 引脚配置有两种模式：
 *
 * 模式 1（INMP441_CHIPEN_GPIO >= 0）：MCU 主动控制 CHIPEN 引脚
 *   - 配置 CHIPEN GPIO 为输出模式，拉高使能
 *   - 等待 300ms 稳定时间（首次上电需 2^18 SCK 周期 ≈ 256ms）
 *
 * 模式 2（INMP441_CHIPEN_GPIO = -1）：CHIPEN 硬连接到 VDD
 *   - INMP441 始终使能，但 SCK/WS 刚刚启动
 *   - 需要等待 2^14 SCK 周期（约 16ms）从待机模式恢复
 *   - 实际等待 100ms 以确保充分稳定
 */
static void Inmp441_Enable(void)
{
#if (INMP441_CHIPEN_GPIO >= 0)
    /*
     * 模式 1：MCU 主动控制 CHIPEN 引脚。
     * 配置 CHIPEN GPIO 为输出模式，内部上拉使能，
     * 然后拉高电平以启用麦克风。
     */
    ESP_LOGI(TAG, "Enabling INMP441 via GPIO%d...", INMP441_CHIPEN_GPIO);

    /*
     * 配置 CHIPEN 控制引脚：
     *   - 输出模式：控制 INMP441 使能/禁用
     *   - 内部上拉：防止未配置时悬空
     *   - 禁用中断：CHIPEN 是输出引脚，不需要中断
     */
    gpio_config_t chipen_cfg = {
        .pin_bit_mask = (1ULL << INMP441_CHIPEN_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&chipen_cfg);

    /* 拉高 CHIPEN 使能麦克风 */
    gpio_set_level(INMP441_CHIPEN_GPIO, 1);

    /*
     * 等待 INMP441 稳定。
     *
     * 根据 INMP441 数据手册：
     *   - 首次上电（冷启动）：需要 2^18 SCK 周期
     *     @ 1.024MHz SCK ≈ 256ms
     *   - 从休眠唤醒：需要 2^17 SCK 周期（约 128ms）
     *
     * SCK 必须在 CHIPEN 拉高之前已经运行！
     * 我们等待 300ms 以留出安全余量。
     */
    vTaskDelay(pdMS_TO_TICKS(300));

    ESP_LOGI(TAG, "INMP441 enabled (CHIPEN=HIGH)");
#else
    /*
     * 模式 2：CHIPEN 硬连接到 VDD，INMP441 始终使能。
     * 但 SCK/WS 时钟刚刚启动（I2S 刚初始化），
     * 麦克风正从待机模式恢复。
     *
     * 根据 INMP441 数据手册：
     *   - 从待机恢复需要 2^14 SCK 周期
     *     @ 1.024MHz SCK ≈ 16ms
     *
     * 我们等待 100ms 以确保充分稳定。
     */
    ESP_LOGI(TAG, "INMP441 CHIPEN is hardwired to VDD, waiting for standby recovery...");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "INMP441 standby recovery complete");
#endif
}

/* ======================== 麦克风公共 API ========================================= */

/**
 * @brief 初始化 INMP441 麦克风模块
 *
 * 初始化顺序严格按以下步骤进行，顺序错误会导致麦克风无法正常工作：
 *
 * 步骤 1：使能 SD 引脚内部下拉
 *   必须在 I2S 初始化之前完成，因为 I2S 初始化会接管 GPIO 控制权。
 *   下拉电阻防止 SD 线在 INMP441 三态输出时悬空。
 *
 * 步骤 2：初始化 I2S RX 接口
 *   启动 SCK 和 WS 时钟输出。根据 INMP441 数据手册，
 *   SCK 和 WS 必须在 CHIPEN 拉高之前运行，否则麦克风会进入待机模式。
 *
 * 步骤 2.5：重新使能 SD 引脚下拉
 *   I2S 驱动初始化时可能重置 GPIO 上下拉配置，
 *   使用 gpio_set_pull_mode() 重新使能下拉（不影响 I2S 功能）。
 *
 * 步骤 3：使能 INMP441 硬件
 *   根据 CHIPEN 配置决定是主动控制使能引脚还是等待待机恢复。
 *   内部包含必要的延迟等待麦克风稳定。
 *
 * @return ESP_OK  初始化成功
 *         ESP_FAIL I2S RX 接口初始化失败
 */
esp_err_t Microphone_Init(void)
{
    /*
     * 步骤 1：在 I2S 初始化之前使能 SD 引脚内部下拉。
     *
     * 原因：INMP441 在不驱动 SD 线时（例如未配置或另一声道时隙），
     * SD 引脚输出三态（高阻态）。如果 SD 线悬空，可能产生随机噪声
     * 或导致 I2S 接收器误触发。下拉电阻确保 SD 线在悬空时维持低电平。
     *
     * 必须在 I2S 驱动接管 GPIO 之前设置，因为 gpio_config() 后
     * 使用 gpio_set_pull_mode() 可以仅修改上下拉模式而不影响 I2S 功能。
     */
    Inmp441_Sd_Pulldown_Enable();

    /*
     * 步骤 2：初始化 I2S RX 接口，启动 SCK/WS 时钟。
     *
     * 根据 INMP441 数据手册，SCK 和 WS 时钟必须在 CHIPEN 拉高之前运行。
     * 如果 CHIPEN 拉高时 SCK 未运行，INMP441 会进入待机模式，
     * 无法正常初始化。
     */
    esp_err_t ret = I2s_Rx_Init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Microphone I2S RX init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /*
     * 步骤 2.5：重新使能 SD 引脚内部下拉。
     *
     * I2S 驱动初始化时可能重置 GPIO 上下拉配置为默认值，
     * 需要重新使能下拉。gpio_set_pull_mode() 只修改上下拉模式，
     * 不会影响 I2S 外设的引脚功能。
     */
    Inmp441_Sd_Pulldown_Enable();

    /*
     * 步骤 3：使能 INMP441 硬件。
     *
     * 此时 SCK/WS 时钟已经运行，INMP441 可以正常初始化。
     * 内部会等待必要的稳定时间（2^18 SCK 周期或 2^14 周期，
     * 取决于 CHIPEN 配置）。
     */
    Inmp441_Enable();

    ESP_LOGI(TAG, "INMP441 microphone initialized, sample_rate=%d", MICROPHONE_SAMPLE_RATE);
    return ESP_OK;
}

/**
 * @brief 反初始化 INMP441 麦克风模块
 *
 * 关闭 I2S RX 接口，停止 SCK/WS 时钟，释放 DMA 缓冲区。
 * 注意：未主动控制 CHIPEN 引脚（当前配置为硬连接 VDD），
 *       反初始化后 INMP441 仍处于使能状态，但不再有时钟输入。
 *
 * @return ESP_OK 反初始化成功
 */
esp_err_t Microphone_Deinit(void)
{
    esp_err_t ret = I2s_Rx_Deinit();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Microphone deinit failed: %s", esp_err_to_name(ret));
    }
    else
    {
        ESP_LOGI(TAG, "INMP441 microphone deinitialized");
    }
    return ret;
}

/**
 * @brief 从麦克风读取原始 32-bit 音频数据
 *
 * 直接从 I2S RX 接口读取原始 32-bit 采样数据，不做任何格式转换。
 * INMP441 输出 24-bit 数据，I2S 标准以 32-bit 帧对齐存储。
 *
 * 每个 32-bit 帧的位布局（MSB first）：
 *   [31:8]  24-bit 有效音频数据（INMP441 输出）
 *   [7:0]   8-bit 零填充（I2S 标准帧对齐）
 *
 * @param buffer     接收缓冲区（uint8_t 数组）
 * @param size       缓冲区大小（字节）
 * @param bytes_read 输出参数，返回实际读取的字节数
 * @param timeout    超时时间（FreeRTOS tick 数）
 *
 * @return ESP_OK             读取成功
 *         ESP_ERR_INVALID_ARG buffer 为 NULL 或 size 为 0
 *         ESP_FAIL            读取失败
 */
esp_err_t Microphone_Read_Raw(uint8_t *buffer, size_t size, size_t *bytes_read, uint32_t timeout)
{
    /* 参数校验：缓冲区不能为空，大小不能为 0 */
    if (buffer == NULL || size == 0)
    {
        ESP_LOGE(TAG, "Invalid buffer parameters");
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * 从 I2S RX 读取原始数据。
     * i2s_channel_read() 内部使用 DMA 从 I2S 外设搬运数据到内存，
     * 如果 DMA 缓冲区为空，会阻塞等待直到有数据或超时。
     */
    esp_err_t ret = I2s_Rx_Read(buffer, size, bytes_read, timeout);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Read raw data failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

/**
 * @brief 从麦克风读取并转换为 PCM 16-bit 格式
 *
 * 该函数是麦克风模块最常用的 API，用于获取标准 16-bit PCM 音频数据，
 * 供 ASR 等语音处理模块使用。
 *
 * 转换原理：
 *   INMP441 输出 24-bit 数据，存放在 I2S 32-bit 帧的高 24 位中。
 *   I2S RX 配置为 32-bit 接收模式，每个 32-bit 帧包含：
 *     [31:8]  = 24-bit 有效音频数据
 *     [7:0]   = 8-bit 零填充
 *
 *   取 rawBuf[i] >> 16 即可获得高 16 位有效数据，
 *   转换为标准 16-bit 有符号 PCM 格式。
 *
 * 读取策略：
 *   采用分批读取策略，每次最多读取 MICROPHONE_READ_BUF_LEN（512）个采样，
 *   循环读取直到满足请求的采样数。这种策略避免了一次性分配过大的缓冲区，
 *   同时可以在每批之间检查是否满足需求。
 *
 * @param pcmBuffer   接收缓冲区（int16_t 数组，16-bit 有符号 PCM）
 * @param sampleCount 期望读取的采样数
 * @param samplesRead 输出参数，返回实际读取的采样数
 * @param timeout     超时时间（FreeRTOS tick 数）
 *
 * @return ESP_OK             读取成功（至少读取了 1 个采样）
 *         ESP_ERR_INVALID_ARG pcmBuffer 为 NULL 或 sampleCount 为 0
 *         ESP_FAIL            未读取到任何采样
 */
esp_err_t Microphone_Read_Pcm16(int16_t *pcmBuffer, size_t sampleCount, size_t *samplesRead, uint32_t timeout)
{
    /* 参数校验：PCM 缓冲区不能为空，采样数不能为 0 */
    if (pcmBuffer == NULL || sampleCount == 0)
    {
        ESP_LOGE(TAG, "Invalid PCM buffer parameters");
        return ESP_ERR_INVALID_ARG;
    }

    size_t totalSamplesRead = 0;    /* 累计已读取的采样数 */
    size_t remainingSamples = sampleCount;  /* 剩余需要读取的采样数 */
    int16_t *pcmPtr = pcmBuffer;    /* 当前写入位置的指针 */

    /*
     * 分批读取循环：每次最多读取 512 个采样，
     * 循环直到满足请求的采样数或读取失败。
     */
    while (remainingSamples > 0)
    {
        /*
         * 计算本次批量读取的采样数：取剩余需求数和 MAX_BUF_LEN 的最小值。
         * 分批读取的好处：
         *   - 每次缓冲区较小（512 × 4 = 2KB），可放在栈上
         *   - 避免一次性分配大缓冲区导致内存碎片
         *   - 可以在每批之间响应超时
         */
        size_t batchSize = (remainingSamples < MICROPHONE_READ_BUF_LEN)
            ? remainingSamples
            : MICROPHONE_READ_BUF_LEN;

        /*
         * 计算需要从 I2S 读取的原始字节数。
         * 每个采样 32-bit = 4 字节，batchSize 个采样需要 batchSize × 4 字节。
         */
        size_t rawBytesToRead = batchSize * (MICROPHONE_BIT_DEPTH / 8);

        /* 原始数据临时缓冲区（32-bit 采样，栈上分配） */
        int32_t rawBuf[MICROPHONE_READ_BUF_LEN];
        size_t bytesRead = 0;  /* 实际读取的字节数 */

        /*
         * 从 I2S RX 读取原始 32-bit 音频数据。
         * i2s_channel_read() 会将数据从 I2S DMA 缓冲区搬运到 rawBuf。
         */
        esp_err_t ret = I2s_Rx_Read(
            (uint8_t *)rawBuf, rawBytesToRead, &bytesRead, timeout
        );

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Read PCM failed at offset %d: %s",
                     (int)(totalSamplesRead), esp_err_to_name(ret));
            break;  /* 读取失败，退出循环，返回已读取的数据 */
        }

        /* 计算本批次实际读取的采样数 */
        size_t samplesInBatch = bytesRead / (MICROPHONE_BIT_DEPTH / 8);

        /*
         * 调试：仅打印第一批数据的前 10 个原始采样值。
         * 使用 static 变量确保只打印一次（整个程序生命周期内）。
         * 用于验证 INMP441 数据格式和 I2S 配置是否正确。
         */
        static int firstBatchPrinted = 0;
        if (firstBatchPrinted == 0)
        {
            firstBatchPrinted = 1;
            ESP_LOGI(TAG, "First 10 raw samples (hex, 32-bit):");
            for (size_t i = 0; i < 10 && i < samplesInBatch; i++)
            {
                ESP_LOGI(TAG, "  [%d] 0x%08lX (%ld)",
                         (int)i, (uint32_t)rawBuf[i], rawBuf[i]);
            }
        }

        /*
         * 格式转换：将 32-bit 原始数据转换为 16-bit PCM。
         *
         * 转换逻辑（rawBuf[i] >> 16）：
         *   INMP441 输出 24-bit 数据，存放在 I2S 32-bit 帧的高 24 位。
         *   右移 16 位后，高 16 位包含 24-bit 数据的高 16 位有效部分。
         *   低 8 位精度被丢弃，但对于语音识别（16kHz 采样率），
         *   16-bit 精度已经足够。
         *
         * 示例：
         *   原始 32-bit：0x00123456 （INMP441 24-bit 数据 = 0x123456）
         *   右移 16 位： 0x00000012 = 18（16-bit PCM 值）
         */
        for (size_t i = 0; i < samplesInBatch; i++)
        {
            pcmPtr[i] = (int16_t)(rawBuf[i] >> 16);
        }

        /* 更新指针和计数器 */
        pcmPtr += samplesInBatch;              /* 移动写入指针 */
        totalSamplesRead += samplesInBatch;    /* 累计已读采样数 */
        remainingSamples -= samplesInBatch;    /* 更新剩余需求 */
    }

    /* 输出实际读取的采样数 */
    if (samplesRead != NULL)
    {
        *samplesRead = totalSamplesRead;
    }

    /*
     * 返回值判断：
     *   - totalSamplesRead > 0：至少读取了部分数据，返回 ESP_OK
     *   - totalSamplesRead == 0：完全未读取到数据，返回 ESP_FAIL
     */
    return (totalSamplesRead > 0) ? ESP_OK : ESP_FAIL;
}