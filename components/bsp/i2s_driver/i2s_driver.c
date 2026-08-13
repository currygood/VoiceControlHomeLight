/**
 * @file i2s_driver.c
 * @brief I2S 音频驱动实现（TX 播放 + RX 录音）
 *
 * 本模块封装 ESP32-S3 的 I2S 外设驱动，提供两个独立的 I2S 通道：
 *   - I2S_NUM_0 (RX)：接收 INMP441 MEMS 麦克风的音频数据
 *   - I2S_NUM_1 (TX)：发送音频数据到 NS4168 功放芯片
 *
 * 两个通道均为 ESP32 作为 I2S 主机（Master），生成 SCK 和 WS 时钟信号。
 *
 * 关键设计决策：
 *   - RX 通道配置为 32-bit/立体声模式 + 槽位掩码，仅接收 RIGHT 声道数据
 *     （因为 INMP441 输出 24-bit 数据，I2S 以 32-bit 帧对齐）
 *   - TX 通道配置为 16-bit/单声道模式，匹配 NS4168 功放芯片的输入格式
 *   - 使用 DMA 传输，CPU 仅需将数据放入/取出 DMA 缓冲区，不参与逐位传输
 *
 * DMA 缓冲区设计：
 *   - 8 个描述符 × 256 帧 = 总共 2048 帧缓冲
 *   - 对于 RX（32-bit 立体声）：2048 帧 = 16KB 缓冲
 *   - 对于 TX（16-bit 单声道）：2048 帧 = 4KB 缓冲
 *   - 足够的缓冲深度可防止音频数据丢失或卡顿
 */

#include "i2s_driver.h"
#include "esp_log.h"
#include <inttypes.h>

/* ======================== 模块静态变量 =========================================== */

static const char *TAG_TX = "I2S_TX";  /**< TX 通道日志标签 */
static const char *TAG_RX = "I2S_RX";  /**< RX 通道日志标签 */

/** I2S TX 通道句柄（I2S_NUM_1，功放播放） */
static i2s_chan_handle_t Tx_Handle = NULL;

/** I2S RX 通道句柄（I2S_NUM_0，麦克风录音） */
static i2s_chan_handle_t Rx_Handle = NULL;

/* ======================== I2S TX 实现（音频播放） ================================ */

/**
 * @brief 初始化 I2S TX 通道（I2S_NUM_1）
 *
 * 初始化流程（共 3 步）：
 *   1. 创建 I2S 通道（配置为 Master 模式，分配 DMA 资源）
 *   2. 配置标准模式（Philips 标准，16-bit 单声道）
 *   3. 使能通道（启动 SCK/WS 时钟输出）
 *
 * 任意步骤失败都会回滚已分配的资源。
 *
 * @return ESP_OK  初始化成功
 *         ESP_FAIL 通道创建、模式配置或使能失败
 */
esp_err_t I2s_Tx_Init(void)
{
    /*
     * 步骤 1：创建 I2S TX 通道。
     *
     * 配置说明：
     *   - id = I2S_NUM_1：使用 I2S 控制器 1
     *   - role = I2S_ROLE_MASTER：ESP32 作为主机，生成 SCK 和 WS 时钟
     *   - dma_desc_num = 8：8 个 DMA 描述符
     *   - dma_frame_num = 256：每个描述符 256 帧
     *   - auto_clear = true：TX 发送完成后自动清除 DMA 缓冲区
     */
    i2s_chan_config_t chanConfig = {
        .id = I2S_NUM_1,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = I2S_TX_DMA_BUF_COUNT,
        .dma_frame_num = I2S_TX_DMA_BUF_LEN,
        .auto_clear = true,
    };

    /*
     * 创建 TX 通道。
     * 参数 2：指向 TX 通道句柄的指针（非 NULL）
     * 参数 3：RX 通道句柄（NULL，此通道仅用于 TX）
     */
    esp_err_t ret = i2s_new_channel(&chanConfig, &Tx_Handle, NULL);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG_TX, "I2S new channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /*
     * 步骤 2：配置 I2S 标准模式（Philips 标准）。
     *
     * 信号时序说明（Philips 标准）：
     *   - WS 信号在每帧开始前翻转，左声道时 WS=低，右声道时 WS=高
     *   - 数据在 SCK 上升沿被采样，下降沿被更新
     *   - MSB 先传输，在 WS 变化后的第二个 SCK 上升沿开始
     */
    i2s_std_config_t stdConfig = {
        /* 时钟配置：16kHz 采样率，I2S_STD_CLK_DEFAULT_CONFIG 自动计算 SCK 频率 */
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_TX_SAMPLE_RATE),

        /*
         * 槽位配置：
         *   - 16-bit 位宽：匹配 NS4168 功放芯片
         *   - 单声道模式：语音播放只需单声道
         */
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO
        ),

        /*
         * GPIO 配置：
         *   - mclk = I2S_GPIO_UNUSED：不使用主时钟（NS4168 不需要 MCLK）
         *   - bclk = GPIO8：位时钟
         *   - ws   = GPIO9：左右声道时钟
         *   - dout = GPIO10：数据输出（连接到 NS4168 DIN）
         *   - din  = I2S_GPIO_UNUSED：TX 通道不需要数据输入
         */
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_TX_BCLK_GPIO,
            .ws = I2S_TX_LRCK_GPIO,
            .dout = I2S_TX_DIN_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    /* 应用标准模式配置到 TX 通道 */
    ret = i2s_channel_init_std_mode(Tx_Handle, &stdConfig);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG_TX, "I2S STD mode init failed: %s", esp_err_to_name(ret));

        /* 配置失败，回滚：删除已创建的通道 */
        i2s_del_channel(Tx_Handle);
        Tx_Handle = NULL;
        return ret;
    }

    /*
     * 步骤 3：使能 I2S TX 通道。
     * 使能后 SCK 和 WS 时钟开始输出，DMA 开始工作。
     */
    ret = i2s_channel_enable(Tx_Handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG_TX, "I2S channel enable failed: %s", esp_err_to_name(ret));

        /* 使能失败，回滚：删除已配置的通道 */
        i2s_del_channel(Tx_Handle);
        Tx_Handle = NULL;
        return ret;
    }

    ESP_LOGI(TAG_TX, "I2S TX initialized, sample_rate=%d, bit_depth=%d, channels=%d",
             I2S_TX_SAMPLE_RATE, I2S_TX_BIT_DEPTH, I2S_TX_CHANNEL_NUM);
    return ESP_OK;
}

/**
 * @brief 关闭 I2S TX 通道
 *
 * 操作顺序：
 *   1. 禁能通道（停止 SCK/WS 时钟输出）
 *   2. 删除通道（释放 DMA 缓冲区和 GPIO 资源）
 *
 * @return ESP_OK 关闭成功
 */
esp_err_t I2s_Tx_Deinit(void)
{
    /* 通道未创建则直接返回 */
    if (Tx_Handle == NULL)
    {
        return ESP_OK;
    }

    /* 禁能通道：停止时钟输出，停止 DMA 传输 */
    i2s_channel_disable(Tx_Handle);

    /* 删除通道：释放 DMA 缓冲区、GPIO 引脚和其他资源 */
    esp_err_t ret = i2s_del_channel(Tx_Handle);
    Tx_Handle = NULL;

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG_TX, "I2S del channel failed: %s", esp_err_to_name(ret));
    }
    else
    {
        ESP_LOGI(TAG_TX, "I2S TX deinitialized");
    }
    return ret;
}

/**
 * @brief 通过 I2S TX 发送音频数据
 *
 * 将 PCM 音频数据通过 DMA 传输到 I2S 外设。
 * 函数是阻塞的，如果 DMA 缓冲区满则等待至超时。
 *
 * @param buffer        音频数据缓冲区
 * @param size          数据大小（字节）
 * @param bytes_written 输出参数，返回实际写入的字节数
 * @param timeout       超时时间（tick 数）
 *
 * @return ESP_OK             发送成功
 *         ESP_ERR_INVALID_STATE 通道未初始化
 *         ESP_FAIL            发送失败
 */
esp_err_t I2s_Tx_Write(const uint8_t *buffer, size_t size, size_t *bytes_written, uint32_t timeout)
{
    /* 通道状态检查 */
    if (Tx_Handle == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * 将数据写入 I2S TX DMA 缓冲区。
     * 内部实现：将数据拷贝到 DMA 描述符，由 DMA 控制器自动
     * 搬运到 I2S 外设的移位寄存器，按 SCK 时钟逐位发送。
     */
    esp_err_t ret = i2s_channel_write(
        Tx_Handle, (const void *)buffer, size, bytes_written, timeout
    );

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG_TX, "I2S write failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

/**
 * @brief 动态切换 I2S TX 采样率
 */
esp_err_t I2s_Tx_Set_Sample_Rate(uint32_t sample_rate)
{
    if (Tx_Handle == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    i2s_channel_disable(Tx_Handle);

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    esp_err_t ret = i2s_channel_reconfig_std_clock(Tx_Handle, &clk_cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG_TX, "Failed to set sample rate to %" PRIu32 ": %s",
                 sample_rate, esp_err_to_name(ret));
        i2s_channel_enable(Tx_Handle);
        return ret;
    }

    i2s_channel_enable(Tx_Handle);
    ESP_LOGI(TAG_TX, "Sample rate changed to %" PRIu32 " Hz", sample_rate);
    return ret;
}

/* ======================== I2S RX 实现（INMP441 麦克风录音） ====================== */

/**
 * @brief 初始化 I2S RX 通道（I2S_NUM_0）
 *
 * 初始化流程与 TX 类似，但配置不同：
 *   - 32-bit 位深度：INMP441 输出 24-bit 数据，I2S 以 32-bit 帧对齐
 *   - 立体声模式 + 槽位掩码：仅接收特定声道的数据
 *   - WS 极性反转（ws_pol = true）：匹配 INMP441 的 WS 时序
 *   - auto_clear = false：RX 通道不自动清除，等待应用层读取
 *
 * @return ESP_OK  初始化成功
 *         ESP_FAIL 通道创建、模式配置或使能失败
 */
esp_err_t I2s_Rx_Init(void)
{
    /*
     * 步骤 1：创建 I2S RX 通道。
     *
     * 配置说明：
     *   - id = I2S_NUM_0：使用 I2S 控制器 0
     *   - role = I2S_ROLE_MASTER：ESP32 作为主机
     *   - auto_clear = false：RX 通道不自动清除 DMA 缓冲区，
     *     等待应用层通过 i2s_channel_read() 读取
     */
    i2s_chan_config_t chanConfig = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = I2S_RX_DMA_BUF_COUNT,
        .dma_frame_num = I2S_RX_DMA_BUF_LEN,
        .auto_clear = false,
    };

    /*
     * 创建 RX 通道。
     * 参数 2：TX 通道句柄（NULL，此通道仅用于 RX）
     * 参数 3：指向 RX 通道句柄的指针（非 NULL）
     */
    esp_err_t ret = i2s_new_channel(&chanConfig, NULL, &Rx_Handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG_RX, "I2S new channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /*
     * 步骤 2：配置 I2S 标准模式。
     *
     * 关键配置说明：
     *   - 32-bit 位宽：INMP441 输出 24-bit 数据，I2S 标准以 32-bit 帧对齐
     *   - 立体声模式：虽然只有一个麦克风，但 INMP441 在左右声道中仅占一个，
     *     需要通过槽位掩码筛选目标声道
     */
    i2s_std_config_t stdConfig = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_RX_SAMPLE_RATE),

        /*
         * 槽位配置：
         *   - 32-bit 位宽：匹配 INMP441 的 24-bit 输出 + 8-bit 填充
         *   - 立体声模式：INMP441 占用其中一个声道
         */
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO
        ),

        /*
         * GPIO 配置：
         *   - bclk = GPIO7：位时钟（连接到 INMP441 SCK）
         *   - ws   = GPIO5：左右声道时钟（连接到 INMP441 WS）
         *   - din  = GPIO6：数据输入（连接到 INMP441 SD）
         *   - dout = I2S_GPIO_UNUSED：RX 通道不需要数据输出
         *   - mclk = I2S_GPIO_UNUSED：INMP441 不需要 MCLK
         */
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_RX_BCLK_GPIO,
            .ws = I2S_RX_LRCK_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = I2S_RX_DOUT_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    /*
     * 覆盖槽位配置的特定参数（这些参数不在 DEFAULT_CONFIG 宏中设置）：
     *
     * slot_mask = I2S_STD_SLOT_RIGHT：
     *   仅接收 RIGHT 声道的数据。INMP441 的 L/R 引脚决定数据在哪个声道
     *   输出。由于 ESP32 I2S 左右声道交换，L/R=GND 时数据在 RIGHT 声道。
     *
     * bit_shift = false：
     *   不做位偏移。INMP441 数据已在 I2S 帧的正确位置（高 24 位），
     *   不需要额外的位偏移。
     *
     * ws_pol = true：
     *   WS 极性反转。INMP441 的 WS 信号时序与标准 I2S 相反，
     *   需要反转 WS 极性以正确对齐数据帧。
     */
    stdConfig.slot_cfg.slot_mask = I2S_RX_SLOT_MASK;
    stdConfig.slot_cfg.bit_shift = false;
    stdConfig.slot_cfg.ws_pol = true;

    /* 应用标准模式配置到 RX 通道 */
    ret = i2s_channel_init_std_mode(Rx_Handle, &stdConfig);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG_RX, "I2S STD mode init failed: %s", esp_err_to_name(ret));

        /* 配置失败，回滚：删除已创建的通道 */
        i2s_del_channel(Rx_Handle);
        Rx_Handle = NULL;
        return ret;
    }

    /*
     * 步骤 3：使能 I2S RX 通道。
     * 使能后 SCK 和 WS 时钟开始输出，DMA 开始从 I2S 外设接收数据。
     */
    ret = i2s_channel_enable(Rx_Handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG_RX, "I2S channel enable failed: %s", esp_err_to_name(ret));

        /* 使能失败，回滚：删除已配置的通道 */
        i2s_del_channel(Rx_Handle);
        Rx_Handle = NULL;
        return ret;
    }

    ESP_LOGI(TAG_RX, "I2S RX initialized (I2S_NUM_0, INMP441), "
             "bclk=GPIO%d, lrck=GPIO%d, dout=GPIO%d, "
             "slot_mask=RIGHT, bit_shift=false, ws_pol=true",
             I2S_RX_BCLK_GPIO, I2S_RX_LRCK_GPIO, I2S_RX_DOUT_GPIO);

    return ESP_OK;
}

/**
 * @brief 关闭 I2S RX 通道
 *
 * 操作顺序：
 *   1. 禁能通道（停止 SCK/WS 时钟输出，停止 DMA 接收）
 *   2. 删除通道（释放 DMA 缓冲区和 GPIO 资源）
 *
 * @return ESP_OK 关闭成功
 */
esp_err_t I2s_Rx_Deinit(void)
{
    /* 通道未创建则直接返回 */
    if (Rx_Handle == NULL)
    {
        return ESP_OK;
    }

    /* 禁能通道：停止时钟输出，停止 DMA 接收 */
    i2s_channel_disable(Rx_Handle);

    /* 删除通道：释放所有资源 */
    esp_err_t ret = i2s_del_channel(Rx_Handle);
    Rx_Handle = NULL;

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG_RX, "I2S del channel failed: %s", esp_err_to_name(ret));
    }
    else
    {
        ESP_LOGI(TAG_RX, "I2S RX deinitialized");
    }
    return ret;
}

/**
 * @brief 从 I2S RX 读取麦克风音频数据
 *
 * 通过 DMA 从 I2S 外设接收数据到内存缓冲区。
 * 函数是阻塞的，如果 DMA 缓冲区为空则等待至超时。
 *
 * @param buffer     接收缓冲区
 * @param size       期望读取的字节数
 * @param bytes_read 输出参数，返回实际读取的字节数
 * @param timeout    超时时间（tick 数）
 *
 * @return ESP_OK             读取成功
 *         ESP_ERR_INVALID_STATE 通道未初始化
 *         ESP_FAIL            读取失败
 */
esp_err_t I2s_Rx_Read(uint8_t *buffer, size_t size, size_t *bytes_read, uint32_t timeout)
{
    /* 通道状态检查 */
    if (Rx_Handle == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * 从 I2S RX DMA 缓冲区读取数据。
     * 内部实现：DMA 控制器自动将 I2S 外设接收到的数据搬运到
     * DMA 缓冲区，i2s_channel_read() 从 DMA 缓冲区拷贝到用户缓冲区。
     */
    esp_err_t ret = i2s_channel_read(
        Rx_Handle, (void *)buffer, size, bytes_read, timeout
    );

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG_RX, "I2S read failed: %s", esp_err_to_name(ret));
    }
    return ret;
}