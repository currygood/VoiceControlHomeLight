/**
 * @file audio_stream.c
 * @brief 多消费者音频流模块实现
 *
 * 本模块解决"多个任务同时读取同一个麦克风"的竞争问题。
 *
 * 架构设计：
 *   - 唯一的生产者任务：从 I2S 麦克风读取 PCM 数据，写入环形缓冲区
 *   - 多个消费者：各自持有独立的读指针，从同一个环形缓冲区读取数据
 *   - 每个消费者读取互不影响，慢消费者会自动丢弃旧数据
 *
 * 环形缓冲区设计：
 *   - 使用单调递增计数器（total_written）标记写入位置
 *   - 每个消费者维护自己的 total_read 计数器
 *   - 可用数据量 = total_written - reader.total_read
 *   - 环形缓冲区位置 = 计数器 % buffer_size
 *
 * 线程安全：
 *   - 互斥锁保护 write_index 和 reader 状态的读写
 *   - FreeRTOS 任务通知用于唤醒等待数据的消费者
 */

#include "Audio_Stream.h"
#include "microphone.h"
#include "audio_codec.h"
#include "amplifier.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include <string.h>
#include <inttypes.h>

#define TAG "AUDIO_STREAM"

/* ======================== 消费者结构体 =========================================== */

typedef struct
{
    TaskHandle_t task_handle;           /**< 消费者任务句柄，用于任务通知唤醒 */
    volatile uint64_t total_read;       /**< 单调递增的已读计数器（不受缓冲区大小限制） */
    bool active;                        /**< 该消费者槽位是否已注册 */
} AudioStream_Reader_t;

/* ======================== 模块静态变量 =========================================== */

static int16_t *RingBuffer = NULL;              /**< 环形缓冲区 */
static size_t RingBufferSize = 0;               /**< 环形缓冲区大小（采样数） */
static volatile uint64_t TotalWritten = 0;      /**< 单调递增的已写入计数器 */
static AudioStream_Reader_t Readers[AUDIO_STREAM_MAX_READERS];  /**< 消费者数组 */
static SemaphoreHandle_t Mutex = NULL;           /**< 互斥锁 */
static TaskHandle_t ProducerTask = NULL;         /**< 生产者任务句柄 */
static volatile bool IsRunning = false;          /**< 生产者任务运行标志 */
static volatile bool IsInit = false;             /**< 模块初始化标志 */

/* ======================== 播放流（多生产者） ======================================= */

/** 播放数据包：带类型标记的变长 OGG 数据 */
typedef struct
{
    AudioPlayback_Type_t type;  /**< 包类型：START / DATA / END */
    uint8_t *data;              /**< OGG 数据指针（仅 DATA 有效） */
    size_t len;                 /**< 数据长度（字节） */
} AudioPlayback_Packet_t;

#define AUDIO_PLAYBACK_QUEUE_LEN 32            /**< 播放队列最大包数 */
#define AUDIO_PLAYBACK_48K_BUF_SAMPLES 5760    /**< 解码输出缓冲（48kHz 采样数） */
#define AUDIO_PLAYBACK_16K_BUF_SAMPLES 1920    /**< 重采样输出缓冲（16kHz 采样数） */

static QueueHandle_t PlaybackQueue = NULL;       /**< 播放队列句柄 */
static TaskHandle_t PlaybackTask = NULL;         /**< 播放任务句柄 */
static volatile bool IsPlaybackRunning = false;  /**< 播放任务运行标志 */

static void AudioStream_Playback_Task(void *pvParameters);  /**< 播放任务前向声明 */

/* ======================== 生产者任务 ============================================= */

/**
 * @brief 音频流生产者任务
 *
 * 持续从麦克风读取 PCM 数据，写入环形缓冲区，并通知所有消费者。
 * 这是整个系统中唯一直接调用 Microphone_Read_Pcm16() 的地方。
 *
 * 写入策略：
 *   1. 从麦克风批量读取 512 个采样（MICROPHONE_READ_BUF_LEN）
 *   2. 将数据写入环形缓冲区
 *   3. 通过任务通知唤醒所有等待数据的消费者
 *
 * 溢出处理：
 *   如果消费者读取速度过慢，生产者不会阻塞，而是继续写入，
 *   消费者下次读取时会自动跳到最新数据位置。
 */
static void AudioStream_Producer_Task(void *pvParameters)
{
    (void)pvParameters;

    ESP_LOGI(TAG, "Producer task started");

    int16_t *localBuffer = malloc(MICROPHONE_READ_BUF_LEN * sizeof(int16_t));
    if (localBuffer == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate producer local buffer");
        vTaskDelete(NULL);
        return;
    }

    while (IsRunning)
    {
        size_t samplesRead = 0;

        esp_err_t ret = Microphone_Read_Pcm16(
            localBuffer,
            MICROPHONE_READ_BUF_LEN,
            &samplesRead,
            pdMS_TO_TICKS(100)
        );

        if (ret != ESP_OK || samplesRead == 0)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        xSemaphoreTake(Mutex, portMAX_DELAY);

        for (size_t i = 0; i < samplesRead; i++)
        {
            size_t writePos = (size_t)(TotalWritten % RingBufferSize);
            RingBuffer[writePos] = localBuffer[i];
            TotalWritten++;
        }

        xSemaphoreGive(Mutex);

        for (int r = 0; r < AUDIO_STREAM_MAX_READERS; r++)
        {
            if (Readers[r].active && Readers[r].task_handle != NULL)
            {
                xTaskNotifyGive(Readers[r].task_handle);
            }
        }
    }

    free(localBuffer);
    ESP_LOGI(TAG, "Producer task stopped");
    vTaskDelete(NULL);
}

/* ======================== 公共 API =============================================== */

/**
 * @brief 初始化音频流模块
 */
esp_err_t AudioStream_Init(size_t buffer_size_samples)
{
    if (IsInit)
    {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    if (buffer_size_samples == 0)
    {
        ESP_LOGE(TAG, "Buffer size must be > 0");
        return ESP_ERR_INVALID_ARG;
    }

    RingBufferSize = buffer_size_samples;

    RingBuffer = heap_caps_malloc(
        RingBufferSize * sizeof(int16_t),
        MALLOC_CAP_SPIRAM
    );
    if (RingBuffer == NULL)
    {
        RingBuffer = malloc(RingBufferSize * sizeof(int16_t));
    }
    if (RingBuffer == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate ring buffer (%u samples)", (unsigned int)RingBufferSize);
        return ESP_FAIL;
    }

    memset(RingBuffer, 0, RingBufferSize * sizeof(int16_t));

    Mutex = xSemaphoreCreateMutex();
    if (Mutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create mutex");
        free(RingBuffer);
        RingBuffer = NULL;
        return ESP_FAIL;
    }

    memset(Readers, 0, sizeof(Readers));
    TotalWritten = 0;

    esp_err_t ret = Microphone_Init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Microphone init failed");
        vSemaphoreDelete(Mutex);
        Mutex = NULL;
        free(RingBuffer);
        RingBuffer = NULL;
        return ESP_FAIL;
    }

    /* 创建播放队列（独立于麦克风采集流，支持多生产者写入） */
    PlaybackQueue = xQueueCreate(AUDIO_PLAYBACK_QUEUE_LEN, sizeof(AudioPlayback_Packet_t));
    if (PlaybackQueue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create playback queue");
        Microphone_Deinit();
        vSemaphoreDelete(Mutex);
        Mutex = NULL;
        free(RingBuffer);
        RingBuffer = NULL;
        return ESP_FAIL;
    }

    /* 启动播放任务，负责从播放队列取包 → OGG 解码 → 播放 */
    IsPlaybackRunning = true;
    if (xTaskCreate(AudioStream_Playback_Task, "audio_playback", 8192, NULL, 7, &PlaybackTask) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create playback task");
        IsPlaybackRunning = false;
        vQueueDelete(PlaybackQueue);
        PlaybackQueue = NULL;
        Microphone_Deinit();
        vSemaphoreDelete(Mutex);
        Mutex = NULL;
        free(RingBuffer);
        RingBuffer = NULL;
        return ESP_FAIL;
    }

    IsInit = true;
    ESP_LOGI(TAG, "Initialized, buffer_size=%u samples (%.1f sec @ 16kHz)",
             (unsigned int)RingBufferSize, (float)RingBufferSize / 16000.0f);
    return ESP_OK;
}

/**
 * @brief 反初始化音频流模块
 */
esp_err_t AudioStream_Deinit(void)
{
    if (!IsInit)
    {
        return ESP_OK;
    }

    AudioStream_Stop();

    if (Mutex != NULL)
    {
        vSemaphoreDelete(Mutex);
        Mutex = NULL;
    }

    if (RingBuffer != NULL)
    {
        free(RingBuffer);
        RingBuffer = NULL;
    }

    Microphone_Deinit();

    memset(Readers, 0, sizeof(Readers));
    IsInit = false;
    ESP_LOGI(TAG, "Deinitialized");
    return ESP_OK;
}

/**
 * @brief 注册一个消费者
 */
AudioStream_ReaderHandle_t AudioStream_Reader_Register(void)
{
    if (!IsInit)
    {
        ESP_LOGE(TAG, "Not initialized, cannot register reader");
        return NULL;
    }

    xSemaphoreTake(Mutex, portMAX_DELAY);

    AudioStream_ReaderHandle_t handle = NULL;

    for (int r = 0; r < AUDIO_STREAM_MAX_READERS; r++)
    {
        if (!Readers[r].active)
        {
            Readers[r].active = true;
            Readers[r].task_handle = xTaskGetCurrentTaskHandle();
            Readers[r].total_read = TotalWritten;
            handle = (AudioStream_ReaderHandle_t)(uintptr_t)(r + 1);
            ESP_LOGI(TAG, "Reader %d registered, starting at offset %" PRIu64,
                     (int)handle, Readers[r].total_read);
            break;
        }
    }

    xSemaphoreGive(Mutex);

    if (handle == NULL)
    {
        ESP_LOGE(TAG, "Maximum readers (%d) reached", AUDIO_STREAM_MAX_READERS);
    }

    return handle;
}

/**
 * @brief 注销一个消费者
 */
esp_err_t AudioStream_Reader_Unregister(AudioStream_ReaderHandle_t handle)
{
    if (handle == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    int readerIndex = (int)(uintptr_t)handle - 1;
    if (readerIndex < 0 || readerIndex >= AUDIO_STREAM_MAX_READERS)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(Mutex, portMAX_DELAY);
    Readers[readerIndex].active = false;
    Readers[readerIndex].task_handle = NULL;
    xSemaphoreGive(Mutex);

    ESP_LOGI(TAG, "Reader %d unregistered", (int)(uintptr_t)handle);
    return ESP_OK;
}

/**
 * @brief 从音频流中读取 PCM 数据
 */
esp_err_t AudioStream_Read(AudioStream_ReaderHandle_t handle, int16_t *buffer,
                           size_t sample_count, size_t *samples_read, uint32_t timeout_ms)
{
    if (handle == NULL || buffer == NULL || sample_count == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    int readerIndex = (int)(uintptr_t)handle - 1;
    if (readerIndex < 0 || readerIndex >= AUDIO_STREAM_MAX_READERS)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!Readers[readerIndex].active)
    {
        ESP_LOGE(TAG, "Reader %d is not active", (int)(uintptr_t)handle);
        return ESP_ERR_INVALID_STATE;
    }

    size_t totalRead = 0;
    TickType_t startTicks = xTaskGetTickCount();
    TickType_t timeoutTicks = pdMS_TO_TICKS(timeout_ms);

    while (totalRead < sample_count)
    {
        size_t available = 0;

        xSemaphoreTake(Mutex, portMAX_DELAY);
        uint64_t currentWritten = TotalWritten;
        uint64_t readerTotalRead = Readers[readerIndex].total_read;
        xSemaphoreGive(Mutex);

        if (currentWritten > readerTotalRead)
        {
            available = (size_t)(currentWritten - readerTotalRead);
        }

        if (available == 0)
        {
            TickType_t elapsed = xTaskGetTickCount() - startTicks;
            if (elapsed >= timeoutTicks)
            {
                break;
            }

            ulTaskNotifyTake(pdTRUE, timeoutTicks - elapsed);
            continue;
        }

        size_t toRead = (available < (sample_count - totalRead))
            ? available
            : (sample_count - totalRead);

        xSemaphoreTake(Mutex, portMAX_DELAY);
        uint64_t baseRead = Readers[readerIndex].total_read;
        xSemaphoreGive(Mutex);

        for (size_t i = 0; i < toRead; i++)
        {
            size_t pos = (size_t)((baseRead + i) % RingBufferSize);
            buffer[totalRead + i] = RingBuffer[pos];
        }

        xSemaphoreTake(Mutex, portMAX_DELAY);
        Readers[readerIndex].total_read += toRead;
        xSemaphoreGive(Mutex);

        totalRead += toRead;
    }

    if (samples_read != NULL)
    {
        *samples_read = totalRead;
    }

    return (totalRead > 0) ? ESP_OK : ESP_ERR_TIMEOUT;
}

/**
 * @brief 启动音频流生产者任务
 */
esp_err_t AudioStream_Start(void)
{
    if (!IsInit)
    {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (IsRunning)
    {
        ESP_LOGW(TAG, "Already running");
        return ESP_OK;
    }

    IsRunning = true;

    BaseType_t ret = xTaskCreate(
        AudioStream_Producer_Task,
        "audio_stream_prod",
        8 * 1024,
        NULL,
        9,
        &ProducerTask
    );

    if (ret != pdPASS)
    {
        IsRunning = false;
        ESP_LOGE(TAG, "Failed to create producer task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Producer task started");
    return ESP_OK;
}

/**
 * @brief 停止音频流生产者任务
 */
esp_err_t AudioStream_Stop(void)
{
    if (!IsRunning)
    {
        return ESP_OK;
    }

    IsRunning = false;

    if (ProducerTask != NULL)
    {
        vTaskDelay(pdMS_TO_TICKS(200));
        ProducerTask = NULL;
    }

    ESP_LOGI(TAG, "Producer task stopped");
    return ESP_OK;
}

/* ======================== 播放流（多生产者）实现 =================================== */

/**
 * @brief 将 48kHz PCM 简单抽取为 16kHz（每 3 采样取均值）
 */
static size_t AudioStream_Resample_48k_To_16k(const int16_t *input, size_t input_samples, int16_t *output)
{
    size_t n = input_samples / 3;
    for (size_t i = 0; i < n; i++)
    {
        int32_t sum = (int32_t)input[i * 3] + (int32_t)input[i * 3 + 1] + (int32_t)input[i * 3 + 2];
        output[i] = (int16_t)(sum / 3);
    }
    return n;
}

/**
 * @brief 播放任务：从播放队列取包，解码 OGG 并播放
 */
static void AudioStream_Playback_Task(void *pvParameters)
{
    (void)pvParameters;

    OggDecoder *dec = NULL;
    int16_t *pcm_48k = NULL;
    int16_t *pcm_16k = NULL;
    AudioPlayback_Packet_t pkt;

    ESP_LOGI(TAG, "Playback task started");

    while (IsPlaybackRunning)
    {
        if (xQueueReceive(PlaybackQueue, &pkt, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        switch (pkt.type)
        {
        case AUDIO_PLAYBACK_START:
            if (dec != NULL)
            {
                AudioCodec_ogg_decoder_destroy(dec);
                dec = NULL;
            }
            free(pcm_48k); pcm_48k = NULL;
            free(pcm_16k); pcm_16k = NULL;
            dec = AudioCodec_ogg_decoder_create();
            pcm_48k = malloc(AUDIO_PLAYBACK_48K_BUF_SAMPLES * sizeof(int16_t));
            pcm_16k = malloc(AUDIO_PLAYBACK_16K_BUF_SAMPLES * sizeof(int16_t));
            if (dec == NULL || pcm_48k == NULL || pcm_16k == NULL)
            {
                ESP_LOGE(TAG, "Failed to alloc decoder/buffers");
            }
            break;

        case AUDIO_PLAYBACK_DATA:
            /* 懒创建兜底：若 START 丢失则在此补建解码器 */
            if (dec == NULL)
            {
                dec = AudioCodec_ogg_decoder_create();
                pcm_48k = malloc(AUDIO_PLAYBACK_48K_BUF_SAMPLES * sizeof(int16_t));
                pcm_16k = malloc(AUDIO_PLAYBACK_16K_BUF_SAMPLES * sizeof(int16_t));
            }
            if (dec == NULL || pcm_48k == NULL || pcm_16k == NULL || pkt.data == NULL)
            {
                break;
            }

            size_t offset = 0;
            while (offset < pkt.len)
            {
                size_t consumed = 0;
                int decoded = AudioCodec_ogg_decoder_feed(dec, pkt.data + offset, pkt.len - offset,
                                                          pcm_48k, AUDIO_PLAYBACK_48K_BUF_SAMPLES, &consumed);
                offset += consumed;
                if (decoded > 0)
                {
                    size_t out_16k = AudioStream_Resample_48k_To_16k(pcm_48k, (size_t)decoded, pcm_16k);
                    Amplifier_Play_Buffer((const uint8_t *)pcm_16k, out_16k * sizeof(int16_t), NULL, portMAX_DELAY);
                }
                else if (decoded < 0)
                {
                    ESP_LOGE(TAG, "OGG decode error: ret=%d", decoded);
                }
                if (consumed == 0)
                {
                    break;
                }
            }
            break;

        case AUDIO_PLAYBACK_END:
            if (dec != NULL)
            {
                AudioCodec_ogg_decoder_destroy(dec);
                dec = NULL;
            }
            free(pcm_48k); pcm_48k = NULL;
            free(pcm_16k); pcm_16k = NULL;
            break;
        }

        free(pkt.data);
        pkt.data = NULL;
    }

    if (dec != NULL)
    {
        AudioCodec_ogg_decoder_destroy(dec);
    }
    free(pcm_48k);
    free(pcm_16k);
    ESP_LOGI(TAG, "Playback task stopped");
    vTaskDelete(NULL);
}

/**
 * @brief 向播放流写入一个数据包
 */
esp_err_t AudioStream_Playback_Write(AudioPlayback_Type_t type, const uint8_t *data, size_t len)
{
    if (PlaybackQueue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    AudioPlayback_Packet_t pkt;
    pkt.type = type;
    pkt.len = len;
    pkt.data = NULL;

    if (len > 0 && data != NULL)
    {
        pkt.data = malloc(len);
        if (pkt.data == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
        memcpy(pkt.data, data, len);
    }

    /* 队列满则不阻塞，丢弃本包，避免拖慢生产者（AI 云端接收） */
    if (xQueueSend(PlaybackQueue, &pkt, 0) != pdTRUE)
    {
        free(pkt.data);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}