/**
 * @file asr.c
 * @brief ASR（Automatic Speech Recognition）自动语音识别模块实现
 *
 * 本模块基于 ESP-SR（Espressif Speech Recognition）框架，实现本地离线
 * 唤醒词检测功能。核心流程：
 *   1. 初始化阶段：加载模型 → 配置 AFE 音频前端 → 创建 AFE 实例
 *   2. 运行阶段：启动 FreeRTOS 任务，循环读取麦克风 PCM 数据 → 喂入 AFE →
 *               检测唤醒词 → 触发 TTS 响应
 *
 * 唤醒词：小鱼同学 实际上是“小宇同学”，但是都能唤醒就不管了，我就叫小鱼同学
 *
 * 硬件依赖：
 *   - microphone 模块：提供 PCM 音频数据输入
 *   - amplifier 模块：播放 TTS 响应（间接依赖）
 *   - ESP32-S3 PSRAM：模型和音频缓冲区需要大容量 PSRAM
 *
 * 模型存储：模型文件存储在 "model" 分区中，通过 esp_srmodel_init("model") 加载。
 */

#include "asr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_afe_config.h"
#include "model_path.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "Audio_Stream.h"
#include "amplifier.h"
#include "AI_Coud.h"
#include "OLED.h"
#include "wifi_manager.h"
#include "esp_timer.h"
#include <string.h>

/* ======================== 模块常量定义 =========================================== */

#define TAG "ASR"                        /**< 日志标签 */

#define KEY_WORD_2 "xiaoyutongxue"       /**< 唤醒词模型名称（与模型分区中的名称匹配） */

#define WAKE_WORD_STRING "小鱼同学"       /**< 唤醒词中文描述（用于日志输出） */

#define CHATTING_TIMEOUT_US (35 * 1000 * 1000) // 35秒，单位为微秒

/* ======================== 模块静态变量 =========================================== */

/** AFE（Audio Front-End）接口句柄，提供 feed/fetch/create/destroy 等方法 */
static const esp_afe_sr_iface_t *sr_AfeHandle = NULL;

/** AFE 数据实例，保存 AFE 内部状态（音频缓冲区、模型状态等） */
static esp_afe_sr_data_t *sr_AfeData = NULL;

/** 模型列表指针，指向从 "model" 分区加载的模型集合 */
static srmodel_list_t *sr_models = NULL;

/** 唤醒词检测任务的 FreeRTOS 任务句柄 */
static TaskHandle_t sr_WakeNetTask = NULL;

/** 模块初始化标志：true = 已完成初始化，false = 未初始化 */
static volatile bool isInit = false;

/** 检测运行标志：true = 检测循环正在运行，false = 已停止 */
static volatile bool is_sr_Running = false;

/** 任务创建标志：true = 任务已创建/或者重新创建了，false = 任务未创建/或者被删除了 */
static volatile bool isTaskCreated = false;

/** ASR 任务栈缓冲区（PSRAM 分配），用于 xTaskCreateStatic */
static StackType_t *sr_TaskStack = NULL;

/** ASR 任务 TCB 缓冲区（静态分配），用于 xTaskCreateStatic */
static StaticTask_t sr_TaskTCB;

/** 每次喂入 AFE 的音频采样数（由 AFE 根据模型配置决定） */
static int sr_AudioChunkSize = 0;

/** AudioStream 消费者句柄，用于从环形缓冲区读取音频数据 */
static AudioStream_ReaderHandle_t sr_AudioReader = NULL;

/** 是否正在聊天中 */
static volatile bool isToChatting = false;

static esp_timer_handle_t chatting_timer = NULL;


/**
 * @brief 定时器回调函数：CHATTING_TIMEOUT_US 到期后执行
 */
static void chatting_timer_callback(void* arg)
{
    ESP_LOGI(TAG, "%ds Timeout: Chatting timeout, stop chatting...", CHATTING_TIMEOUT_US / 1000000);
	AiCloud_Stop();	// 停止与云端AI通信
    isToChatting = false;
}

/* ======================== 唤醒词检测任务 ========================================= */

/**
 * @brief ASR 唤醒词检测任务的主循环
 *
 * 该任务作为 FreeRTOS 线程运行，持续执行以下流程：
 *   1. 从 AudioStream 环形缓冲区读取 PCM 16-bit 音频数据
 *   2. 将音频数据喂入 AFE 处理流水线
 *   3. 从 AFE 获取检测结果
 *   4. 如果检测到唤醒词（WAKENET_DETECTED），通过 TTS 播报响应
 *
 * @param pvParameters FreeRTOS 任务参数（本任务未使用，传入 NULL）
 */
static void ASR_Task(void *pvParameters)
{
    (void)pvParameters;  /* 抑制未使用参数的编译器警告 */

    ESP_LOGI(TAG, "WakeNet detection task started");

    /*
     * 注册为 AudioStream 的消费者，获取独立的读句柄。
     * 此后通过 AudioStream_Read() 从环形缓冲区读取音频数据，
     * 与其他消费者（如 AI Cloud）互不干扰。
     */
    sr_AudioReader = AudioStream_Reader_Register();
    if (sr_AudioReader == NULL)
    {
        ESP_LOGE(TAG, "Failed to register as audio stream reader");
        vTaskDelete(NULL);
        return;
    }

    /*
     * 在 PSRAM 中分配音频喂入缓冲区。
     * 使用 heap_caps_malloc 而非普通 malloc，因为：
     *   - 音频缓冲区较大（通常数千采样点），内部 SRAM 有限
     *   - 分配在 PSRAM 中可避免占用宝贵的内部 SRAM
     *   - MALLOC_CAP_SPIRAM 确保内存来自外部 SPI RAM
     */
    int16_t *feedBuffer = heap_caps_malloc(
        sr_AudioChunkSize * sizeof(int16_t),
        MALLOC_CAP_SPIRAM
    );

    if (feedBuffer == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate feed buffer in PSRAM");
        AudioStream_Reader_Unregister(sr_AudioReader);
        sr_AudioReader = NULL;
        vTaskDelete(NULL);  /* 无法分配缓冲区，任务无法继续运行 */
        return;
    }

    /* 主检测循环：持续读取音频 → 喂入 AFE → 检测唤醒词 */
    while (is_sr_Running)
    {
        size_t samplesRead = 0;

        /*
         * 从 AudioStream 环形缓冲区读取 PCM 16-bit 音频数据。
         * 超时时间 100ms：避免在无数据时永久阻塞，
         * 同时允许主循环及时响应停止信号。
         */
        esp_err_t ret = AudioStream_Read(
            sr_AudioReader,
            feedBuffer,
            sr_AudioChunkSize,
            &samplesRead,
            100
        );

        /* 读取失败或无数据则短暂休眠后重试 */
        if (ret != ESP_OK || samplesRead == 0)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* AFE 句柄检查：防止在反初始化过程中访问空指针 */
        if (sr_AfeHandle == NULL || sr_AfeData == NULL)
        {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /*
         * 将音频数据喂入 AFE 处理流水线。
         * AFE 内部会进行：AEC（回声消除）、NS（降噪）、
         * VAD（语音活动检测）等处理（取决于配置），
         * 然后将处理后的音频送入 WakeNet 唤醒词引擎。
         */
        sr_AfeHandle->feed(sr_AfeData, feedBuffer);

        /*
         * 从 AFE 获取检测结果。
         * fetch() 是非阻塞调用，返回 NULL 表示尚未有结果。
         * 当 wakeup_state == WAKENET_DETECTED 时表示唤醒词被检测到。
         */
        afe_fetch_result_t *result = sr_AfeHandle->fetch(sr_AfeData);

        if (result != NULL && result->wakeup_state == WAKENET_DETECTED)
        {
            /* 唤醒词检测到！打印检测到的唤醒词索引 */
            ESP_LOGI(TAG, "Wake word detected! Index: %d", result->wake_word_index);

            // 唤醒了，开始与云端ai进行实时语音聊天
			if(WifiManager_IsConnected() && !isToChatting)
			{
				AiCloud_Start();
				// 开始倒计时
				esp_timer_start_once(chatting_timer, CHATTING_TIMEOUT_US);
				isToChatting = true;
			}
        }
    }

    /* 循环退出：注销消费者，释放缓冲区并删除任务 */
    AudioStream_Reader_Unregister(sr_AudioReader);
    sr_AudioReader = NULL;
    free(feedBuffer);
    ESP_LOGI(TAG, "ASR detection task stopped");
    vTaskDelete(NULL);
}

/* ======================== ASR 公共 API =========================================== */

/**
 * @brief 初始化 ASR 唤醒词检测模块
 *
 * 初始化流程（共 7 步）：
 *   1. 检查重复初始化
 *   2. 从 "model" 分区加载模型列表
 *   3. 按名称过滤唤醒词模型
 *   4. 创建并配置 AFE 配置结构体
 *   5. 从配置创建 AFE 句柄和数据实例
 *   6. 添加唤醒词模型到 AFE 实例
 *   7. 获取音频参数（chunk 大小、采样率）
 *
 * 每步失败都会回滚已分配的资源，保证不会内存泄漏。
 *
 * @return ESP_OK  初始化成功
 *         ESP_FAIL 模型加载/配置/实例创建失败
 */
esp_err_t ASR_Init(void)
{
	// 创建定时器
    const esp_timer_create_args_t timer_args = {
        .callback = &chatting_timer_callback,
        .name = "chatting_watchdog"
    };
    esp_timer_create(&timer_args, &chatting_timer);

    /* 防止重复初始化 */
    if (isInit)
    {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing WakeNet (wake word: %s)...", WAKE_WORD_STRING);

    /*
     * 从 "model" 分区加载模型列表。
     * 模型文件通过 ESP-IDF 分区表存储在 flash 中，
     * esp_srmodel_init() 会解析分区内的所有模型文件。
	 * 因为乐鑫把esp-sr的模型文件放在了"model"分区，所以这里需要指定"model"分区，注意：分区表就必须要包含"model"分区。
     */
    sr_models = esp_srmodel_init("model");
    if (sr_models == NULL)
    {
        ESP_LOGE(TAG, "Failed to load models from 'model' partition");
        return ESP_FAIL;
    }

    /*
     * 从模型列表中按前缀过滤，找到匹配的唤醒词模型。
     * ESP_WN_PREFIX 是 WakeNet 模型的标准前缀，
     * "xiaoyutongxue" 是具体的唤醒词模型名称。
     */
    char *modelName = esp_srmodel_filter(sr_models, ESP_WN_PREFIX, "xiaoyutongxue");
    if (modelName == NULL)
    {
        ESP_LOGE(TAG, "Wake word model not found in partition");
        esp_srmodel_deinit(sr_models);
        sr_models = NULL;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Found model: %s", modelName);

    /*
     * 创建 AFE 配置结构体。
     *
     * 参数说明：
     *   "M"         - 使用多麦克风模式（Multi-mic），单麦也适用
     *   sr_models   - 模型列表，AFE 从中读取声学参数
     *   AFE_TYPE_SR - 语音识别模式（Speech Recognition），区别于语音增强
     *   AFE_MODE_HIGH_PERF - 高性能模式，延迟更低但功耗更高
     *
     * 配置优化说明：
     *   本应用仅需唤醒词检测，不需要命令词识别，因此关闭了所有
     *   非必要的音频处理模块，以节省 CPU 和内存：
     *   - aec_init = false（不需要回声消除，无扬声器回采）
     *   - se_init  = false（不需要语音增强）
     *   - ns_init  = false（不需要降噪）
     *   - vad_init = false（不需要语音活动检测，WakeNet 内部自带）
     *   - agc_init = false（不需要自动增益控制）
     */
    afe_config_t *afeConfig = afe_config_init(
        "M", sr_models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF
    );

    if (afeConfig == NULL)
    {
        ESP_LOGE(TAG, "Failed to create AFE config");
        esp_srmodel_deinit(sr_models);
        sr_models = NULL;
        return ESP_FAIL;
    }

    /* 配置唤醒词检测：启用唤醒词引擎，指定模型名称 */
    afeConfig->wakenet_init = true;
    afeConfig->wakenet_model_name = modelName;

    /* 关闭所有非必要模块以节省资源 */
    afeConfig->aec_init = false;   /* 回声消除：关闭 */
    afeConfig->se_init = false;    /* 语音增强：关闭 */
    afeConfig->ns_init = false;    /* 降噪：关闭 */
    afeConfig->vad_init = false;   /* 语音活动检测：关闭 */
    afeConfig->agc_init = false;   /* 自动增益控制：关闭 */

    /*
     * 调用 afe_config_check() 验证配置合法性。
     * 该函数会检查配置之间的一致性，并填充默认值。
     */
    afeConfig = afe_config_check(afeConfig);

    /*
     * 从配置创建 AFE 句柄。
     * AFE 句柄包含所有处理接口（feed/fetch/destroy 等）。
     */
    sr_AfeHandle = esp_afe_handle_from_config(afeConfig);
    if (sr_AfeHandle == NULL)
    {
        ESP_LOGE(TAG, "Failed to get AFE handle");
        afe_config_free(afeConfig);
        esp_srmodel_deinit(sr_models);
        sr_models = NULL;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "AFE handle obtained");

    /*
     * 从配置创建 AFE 数据实例。
     * AFE 数据实例保存运行时状态（音频缓冲区、模型状态等）。
     * 每个 AFE 句柄可以创建多个数据实例（多通道场景），这里只创建一个。
     */
    sr_AfeData = sr_AfeHandle->create_from_config(afeConfig);
    if (sr_AfeData == NULL)
    {
        ESP_LOGE(TAG, "Failed to create AFE instance");
        afe_config_free(afeConfig);
        esp_srmodel_deinit(sr_models);
        sr_models = NULL;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "AFE instance created");

    /* 配置已使用完毕，释放配置结构体 */
    afe_config_free(afeConfig);

    /*
     * 向 AFE 实例添加唤醒词模型。
     *
     * 注意：某些版本的 AFE 接口可能不支持 add_wakenet_model 方法
     * （模型已在 create_from_config 时加载），这里做兼容性检查。
     */
    if (sr_AfeHandle->add_wakenet_model == NULL)
    {
        ESP_LOGE(TAG, "add_wakenet_model not supported by this AFE handle");
        sr_AfeHandle->destroy(sr_AfeData);
        sr_AfeData = NULL;
        esp_srmodel_deinit(sr_models);
        sr_models = NULL;
        return ESP_FAIL;
    }

    int ret = sr_AfeHandle->add_wakenet_model(sr_AfeData, modelName);
    if (ret < 0)
    {
        ESP_LOGE(TAG, "Failed to add WakeNet model: %s", modelName);
        sr_AfeHandle->destroy(sr_AfeData);
        sr_AfeData = NULL;
        esp_srmodel_deinit(sr_models);
        sr_models = NULL;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "WakeNet model added: %s", modelName);

    /*
     * 获取 AFE 音频参数。
     * - get_feed_chunksize()：每次 feed() 调用需要的采样点数
     * - get_samp_rate()：AFE 期望的音频采样率
     * 这些参数用于后续音频读取和缓冲区分配。
     */
    sr_AudioChunkSize = sr_AfeHandle->get_feed_chunksize(sr_AfeData);
    int sampleRate = sr_AfeHandle->get_samp_rate(sr_AfeData);
    ESP_LOGI(TAG, "Audio chunk size: %d samples, sample rate: %d Hz",
             sr_AudioChunkSize, sampleRate);

    /* 标记初始化完成 */
    isInit = true;
    ESP_LOGI(TAG, "ASR initialized successfully");
    return ESP_OK;
}

/**
 * @brief 反初始化 ASR 模块，释放所有资源
 *
 * 操作顺序：
 *   1. 停止检测任务（如果正在运行）
 *   2. 销毁 AFE 数据实例
 *   3. 释放模型资源
 *   4. 重置初始化标志
 *
 * @return ESP_OK 反初始化成功
 */
esp_err_t ASR_Deinit(void)
{
    /* 未初始化则直接返回，无须操作 */
    if (!isInit)
    {
        return ESP_OK;
    }

    /* 先停止检测任务，确保不再访问 AFE 资源 */
    ASR_Stop();

    /* 销毁 AFE 数据实例，释放内部状态和缓冲区 */
    if (sr_AfeData != NULL)
    {
        sr_AfeHandle->destroy(sr_AfeData);
        sr_AfeData = NULL;
    }

    /* 释放模型列表资源 */
    if (sr_models != NULL)
    {
        esp_srmodel_deinit(sr_models);
        sr_models = NULL;
    }

    /* 重置初始化标志 */
    isInit = false;
    ESP_LOGI(TAG, "WakeNet deinitialized");
    return ESP_OK;
}

/**
 * @brief 启动 ASR 唤醒词检测任务
 *
 * 创建一个优先级为 8、栈大小为 32KB 的 FreeRTOS 任务来运行检测循环。
 * 任务优先级较高，确保音频数据能被及时处理，避免缓冲区溢出。
 *
 * @return ESP_OK           启动成功
 *         ESP_ERR_INVALID_STATE 尚未初始化
 *         ESP_ERR_NO_MEM    任务创建失败（内存不足）
 */
esp_err_t ASR_Start(void)
{
    /* 检查初始化状态：未初始化不能启动 */
    if (!isInit)
    {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* 检查运行状态：已在运行则无需重复启动 */
    if (is_sr_Running)
    {
        ESP_LOGW(TAG, "Already running");
        return ESP_OK;
    }

    /* 设置运行标志，通知任务循环开始工作 */
    is_sr_Running = true;

    /* 任务已创建则直接返回，无需重复创建 */
    if (isTaskCreated)
    {
        return ESP_OK;
    }

    isTaskCreated = true;

    /*
     * 从 PSRAM 分配任务栈（32KB），避免消耗宝贵的内部 SRAM。
     * 使用 xTaskCreateStatic 而非 xTaskCreate，这样栈内存从 PSRAM 分配，
     * 内部 SRAM 留给系统关键任务和中断处理。
     */
    #define ASR_TASK_STACK_SIZE (32 * 1024)
    sr_TaskStack = heap_caps_malloc(ASR_TASK_STACK_SIZE, MALLOC_CAP_SPIRAM);
    if (sr_TaskStack == NULL)
    {
        is_sr_Running = false;
        isTaskCreated = false;
        ESP_LOGE(TAG, "Failed to allocate ASR task stack from PSRAM");
        return ESP_ERR_NO_MEM;
    }

    TaskHandle_t taskRet = xTaskCreateStatic(
        ASR_Task,
        "asr_task",
        ASR_TASK_STACK_SIZE,
        NULL,
        8,
        sr_TaskStack,
        &sr_TaskTCB
    );

    if (taskRet == NULL)
    {
        free(sr_TaskStack);
        sr_TaskStack = NULL;
        is_sr_Running = false;
        isTaskCreated = false;
        ESP_LOGE(TAG, "Failed to create ASR task");
        return ESP_ERR_NO_MEM;
    }

    sr_WakeNetTask = taskRet;

    ESP_LOGI(TAG, "ASR detection started");
    return ESP_OK;
}

/**
 * @brief 停止 ASR 唤醒词检测任务
 *
 * 通过设置 is_sr_Running = false 来通知检测循环退出。
 * 等待 200ms 确保任务有机会检测到停止标志并退出。
 * 注意：我们不直接删除任务（vTaskDelete），而是让任务自行退出，
 *       这样可以确保任务内部的资源（如 feedBuffer）被正确释放。
 *
 * @return ESP_OK 停止成功
 */
esp_err_t ASR_Stop(void)
{
    /* 未在运行则无需停止 */
    if (!is_sr_Running)
    {
        return ESP_OK;
    }

    /*
     * 设置停止标志。
     * 任务循环在每次迭代开始时检查此标志，发现为 false 即退出循环。
     */
    is_sr_Running = false;

    /*
     * 等待 200ms，让任务有机会退出循环。
     * 任务循环的超时时间为 100ms（麦克风读取超时），
     * 200ms 足够让任务完成当前迭代并退出。
     */
    if (sr_WakeNetTask != NULL)
    {
        vTaskDelay(pdMS_TO_TICKS(200));
        sr_WakeNetTask = NULL;
    }

    if (sr_TaskStack != NULL)
    {
        free(sr_TaskStack);
        sr_TaskStack = NULL;
    }

    /* 重置任务创建标志，允许下次 Start 时重新创建任务 */
    isTaskCreated = false;
    ESP_LOGI(TAG, "ASR detection stopped");
    return ESP_OK;
}

/**
 * @brief 查询 ASR 检测任务是否正在运行
 *
 * @return true  正在运行
 *         false 未运行
 */
bool ASR_IsRunning(void)
{
    return is_sr_Running ? true : false;
}
