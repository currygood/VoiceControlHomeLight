/**
 * @file KeyManager.c
 * @brief 按键管理模块 - 状态机实现
 *
 * 本文件实现基于有限状态机（FSM）的按键检测算法，支持单击、双击、长按三种事件。
 * 使用ESP32的esp_timer软件定时器驱动状态机，通过FreeRTOS队列向外部传递事件。
 *
 * 模块架构：
 *   ┌─────────────────────────────────────┐
 *   │         KeyManager (BSP层)          │
 *   │                                     │
 *   │  软件定时器(10ms)                    │
 *   │       ↓                             │
 *   │  状态机引擎 (每个按键独立实例)       │
 *   │  ├─ GPIO读取 + 消抖                 │
 *   │  ├─ 单击/双击/长按检测              │
 *   │  └─ 事件生成 → FreeRTOS队列        │
 *   │                                     │
 *   │  对外接口: Init / GetEvent          │
 *   └─────────────────────────────────────┘
 *
 * 状态机设计（5个状态）：
 *
 *   ┌──────────┐
 *   │   IDLE   │ ← 初始状态/重置状态
 *   └────┬─────┘
 *        │ 检测到下降沿（按下）
 *        ↓
 *   ┌────────────────┐
 *   │ PRESS_DEBOUNCE │ ← 等待20ms消抖确认
 *   └────┬───────────┘
 *        │ 消抖成功（电平稳定）
 *        ↓
 *   ┌──────────┐     长按≥1s      ┌──────────────┐
 *   │ PRESSED  │ ──────────────→  │ 发送长按事件  │
 *   └────┬─────┘                  └──────┬───────┘
 *        │ 检测到上升沿（释放）           ↓
 *        ↓                         RELEASE_DEBOUNCE
 *   ┌──────────────────┐                ↓
 *   │ RELEASE_DEBOUNCE │ ← 等待20ms消抖确认
 *   └────┬─────────────┘
 *        │ 消抖成功
 *        ↓
 *   ┌───────────────────┐
 *   │ WAIT_DOUBLE_CLICK │ ← 等待300ms第二次按下
 *   └──┬────────────┬───┘
 *      │            │
 *   第二次按下    超时300ms
 *      ↓            ↓
 *   双击事件      单击事件
 *      └─────→ IDLE ←┘
 *
 * 时间参数配置：
 *   - 消抖时间: 20ms（2个采样周期）
 *   - 长按判定: 1000ms（100个采样周期）
 *   - 双击窗口: 300ms（30个采样周期）
 *   - 采样周期: 10ms（软件定时器频率）
 *
 * 硬件连接要求：
 *   - 按键一端接GPIO引脚，另一端接GND
 *   - GPIO配置为输入模式 + 内部上拉电阻（~40KΩ）
 *   - 低电平有效：按下=0V(GND)，松开=3.3V(VCC via pull-up)
 *
 * 多按键支持：
 *   - 每个按键维护独立的状态机实例（Key_Instance结构体）
 *   - 所有实例共享同一个定时器回调
 *   - 事件通过同一个队列发送，包含按键ID以区分来源
 *
 * 实时性保证：
 *   - 使用xQueueSendFromISR()从定时器回调发送事件
 *   - 通过portYIELD_FROM_ISR()确保高优先级任务立即唤醒
 *   - 最坏情况延迟 = 定时器周期(10ms) + 任务调度延迟(<1ms)
 *
 * 使用方式：
 *   1. 系统启动时调用 KeyManager_Init()
 *   2. 定时器自动创建并启动，状态机开始运行
 *   3. 外部任务通过 KeyManager_Get_Event() 阻塞等待事件
 *   4. 收到事件后根据 event.id 和 event.type 执行相应操作
 *
 * @note 本模块完全自主运行，不需要外部周期性调用Process函数
 * @note 内部使用esp_timer高精度定时器（微秒级），比xTaskGetTickCount更精确
 * @warning 不要在ISR中调用本模块的公共API（Init/GetEvent等）
 *
 * @see KeyManager.h - 公共API接口声明和数据类型定义
 * @see HardKeyControlLight.c - 典型的消费者实现示例
 */

#include "KeyManager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"

static const char *TAG = "KeyManager";

/* ======================== 私有数据类型 ======================================= */

/**
 * @brief 按键实例结构体（每个按键一个）
 *
 * 维护单个按键的完整状态信息，包括当前状态机状态、时间戳、计数器等。
 * 支持双阈值消抖算法，需要额外记录疑似释放时刻。
 * 所有字段由状态机内部使用，对外部不可见。
 */
typedef struct
{
	Key_State_Machine state;                /**< 当前状态机状态 */
	Key_ID id;                              /**< 按键ID标识 */
	int gpioNum;                            /**< GPIO引脚号 */
	int64_t pressStartTime;                 /**< 按下时刻的时间戳（毫秒） */
	int64_t releaseTime;                    /**< 释放时刻的时间戳（毫秒） */
	int64_t suspectedReleaseStartTime;      /**< 疑似释放时刻的时间戳（毫秒，用于双阈值消抖） */
	uint8_t pressCount;                     /**< 按下次数计数器（用于双击检测） */
	bool lastGpioLevel;                     /**< 上一次采样的GPIO电平（用于边沿检测） */
	bool isValidPress;                      /**< 当前按压是否有效（防止重复触发长按） */
} Key_Instance;

/* ======================== 全局变量 =========================================== */

/** @brief 所有按键的实例数组，索引对应Key_ID枚举值 */
static Key_Instance keyInstanceArr[KEY_NUM];

/** @brief 按键事件队列句柄（FreeRTOS队列） */
static QueueHandle_t keyEventQueue = NULL;

/** @brief 软件定时器句柄（esp_timer） */
static esp_timer_handle_t keyTimerHandle = NULL;

/* ======================== 私有函数实现 ======================================= */

/**
 * @brief 初始化GPIO硬件配置
 *
 * 配置所有按键使用的GPIO引脚为输入模式，使能内部上拉电阻，
 * 禁用GPIO中断（本模块使用轮询模式）。
 *
 * GPIO配置参数：
 *   - 模式: GPIO_MODE_INPUT（输入）
 *   - 上拉: GPIO_PULLUP_ENABLE（使能，约40KΩ）
 *   - 下拉: GPIO_PULLDOWN_DISABLE（禁用）
 *   - 中断: GPIO_INTR_DISABLE（不使用中断）
 */
static void KeyManager_GPIO_Init(void)
{
	gpio_config_t io_conf = {};
	io_conf.intr_type = GPIO_INTR_DISABLE;
	io_conf.mode = GPIO_MODE_INPUT;
	io_conf.pin_bit_mask = (1ULL << KEY_BEDROOM_GPIO) | (1ULL << KEY_LIVINGROOM_GPIO);
	io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
	io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
	gpio_config(&io_conf);
}

/**
 * @brief 重置按键实例到初始状态
 *
 * 将指定按键实例的所有状态变量重置为初始值。
 * 在以下情况下调用：
 *   - 初始化时
 *   - 事件处理完成后（单击/双击后返回IDLE）
 *   - 异常状态恢复时
 *
 * @param[in,out] instance 要重置的按键实例指针
 */
static void KeyManager_Reset_Instance(Key_Instance *instance)
{
	instance->state = KEY_STATE_IDLE;
	instance->pressStartTime = 0;
	instance->releaseTime = 0;
	instance->suspectedReleaseStartTime = 0;
	instance->pressCount = 0;
	instance->isValidPress = false;
}

/**
 * @brief 读取GPIO电平并转换为逻辑值
 *
 * 读取指定GPIO引脚的电平状态，并将其转换为逻辑上的"按下/未按下"值。
 * 由于使用低电平有效的硬件设计，GPIO读数为0表示按下。
 *
 * @param[in] gpioNum GPIO引脚号
 *
 * @return true  按键被按下（GPIO低电平，0V）
 * @return false 按键未按下（GPIO高电平，3.3V via pull-up）
 */
static bool KeyManager_Read_GPIO(int gpioNum)
{
	return gpio_get_level(gpioNum) == 0;
}

/**
 * @brief 发送按键事件到队列
 *
 * 将检测到的按键事件通过FreeRTOS队列发送给消费者任务。
 * 此函数可能在定时器回调上下文中调用，因此使用FromISR版本的API。
 *
 * 发送机制：
 *   1. 构建Key_Event结构体（ID + Type）
 *   2. 调用xQueueSendFromISR()写入队列
 *   3. 如果唤醒了更高优先级的任务，请求上下文切换
 *
 * @param[in] id   触发事件的按键ID
 * @param[in] type 事件类型（单击/双击/长按）
 *
 * @note 如果队列已满或未创建，事件将被丢弃（不阻塞）
 * @note 通过portYIELD_FROM_ISR()保证实时响应
 */
static void KeyManager_Send_Event(Key_ID id, Key_Type type)
{
	if (keyEventQueue != NULL)
	{
		BaseType_t highTaskWakeup = pdFALSE;
		Key_Event event = { .id = id, .type = type };
		xQueueSendFromISR(keyEventQueue, &event, &highTaskWakeup);

		if (highTaskWakeup != pdFALSE)
		{
			portYIELD_FROM_ISR();
		}
	}
}

/**
 * @brief 按键状态机核心处理函数（单次采样）- 双阈值消抖版本
 *
 * 对指定的按键实例执行一次状态机转换。此函数每10ms被定时器回调调用一次，
 * 负责读取GPIO、更新状态、检测事件。
 *
 * 采用**双阈值消抖算法**，完美解决机械按键抖动问题。
 * 核心思想：按下和松开都有独立的确认期，中间的抖动不会导致状态误判。
 *
 * 状态转换详细说明（6个状态）：
 *
 * 1. IDLE状态：
 *    - 检测下降沿（lastGpioLevel=1 && currentLevel=0）→ 进入PRESS_DEBOUNCE
 *    - 记录pressStartTime用于后续消抖判断
 *
 * 2. PRESS_DEBOUNCE状态（按下确认期）：
 *    - 电平保持低且持续≥20ms → 进入PRESSED，标记为有效按压 ✅
 *    - 电平变回高（中途松开或抖动）→ 进入SUSPECTED_RELEASE ⭐新增！
 *      （不是直接回IDLE，而是先怀疑是否真松开）
 *
 * 3. SUSPECTED_RELEASE状态（疑似释放确认期）⭐新增状态：
 *    - 电平保持高且持续≥20ms → 确认是真松开 → 回到IDLE ✅
 *    - 电平又变回低 → 说明刚才的高电平只是抖动 → 回到PRESS_DEBOUNCE继续等待 ✅
 *      （关键改进：不会丢失之前的pressStartTime上下文）
 *
 * 4. PRESSED状态：
 *    - 检测上升沿（释放）→ 进入RELEASE_DEBOUNCE
 *    - 持续按下且≥1s且之前未触发过长按 → 发送KEY_LONG_PRESS事件
 *      （发送后进入RELEASE_DEBOUNCE，避免重复触发）
 *
 * 5. RELEASE_DEBOUNCE状态（释放确认期）：
 *    - 电平保持高且持续≥20ms → 确认释放有效
 *      - 第1次释放 → 进入WAIT_DOUBLE_CLICK
 *      - 第2次及以上释放 → 发送KEY_DOUBLE事件，重置到IDLE
 *    - 电平变低 → 再次按下，返回PRESSED继续处理（防抖动）
 *
 * 6. WAIT_DOUBLE_CLICK状态：
 *    - 检测到第2次按下 → 返回PRESS_DEBOUNCE开始新一轮检测
 *    - 超过300ms无第2次按下 → 发送KEY_SINGLE事件，重置到IDLE
 *
 * 双阈值消抖的优势（对比传统单阈值）：
 *
 * 场景：用户按下按键，但在20ms消抖窗口内出现抖动
 *
 * ❌ 传统算法的问题：
 *    T=0ms:   按下 → 进入PRESS_DEBOUNCE
 *    T=10ms:  抖动变高 → 直接回IDLE！（误判为松开）
 *    T=15ms:  又变低 → 重新进入PRESS_DEBOUNCE（重新计时）
 *    T=30ms:  又抖动变高 → 又回IDLE...
 *    结果：反复横跳，可能永远无法确认按下！
 *
 * ✅ 双阈值算法的解决：
 *    T=0ms:   按下 → 进入PRESS_DEBOUNCE
 *    T=10ms:  抖动变高 → 进入SUSPECTED_RELEASE（不急着重置）
 *    T=15ms:  又变低 → 回到PRESS_DEBOUNCE（继续原来的计时！）
 *    T=25ms:  保持低电平满20ms → 进入PRESSED（确认真按下）✅
 *    T=30ms:  用户真正松开 → 进入RELEASE_DEBOUNCE
 *    T=50ms:  保持高电平满20ms → 确认释放有效 ✅
 *    结果：正确识别按下和松开，不受抖动影响！
 *
 * @param[in,out] instance 要处理的按键实例指针
 *
 * @note 使用esp_timer_get_time()获取微秒级时间戳，除以1000转为毫秒
 * @note 边沿检测通过比较currentLevel和lastGpioLevel实现
 * @note 本算法能容忍复杂的抖动模式，工业级可靠性
 */
static void KeyManager_State_Machine_Process(Key_Instance *instance)
{
	int64_t currentTime = esp_timer_get_time() / 1000;
	bool currentLevel = KeyManager_Read_GPIO(instance->gpioNum);

	switch (instance->state)
	{
		case KEY_STATE_IDLE:
			if (currentLevel && !instance->lastGpioLevel)
			{
				instance->state = KEY_STATE_PRESS_DEBOUNCE;
				instance->pressStartTime = currentTime;
			}
			break;

		case KEY_STATE_PRESS_DEBOUNCE:
			if (currentLevel)
			{
				if ((currentTime - instance->pressStartTime) >= KEY_DEBOUNCE_TIME_MS)
				{
					instance->state = KEY_STATE_PRESSED;
					instance->isValidPress = true;
				}
			}
			else
			{
				instance->state = KEY_STATE_SUSPECTED_RELEASE;
				instance->suspectedReleaseStartTime = currentTime;
			}
			break;

		case KEY_STATE_SUSPECTED_RELEASE:
			if (!currentLevel)
			{
				if ((currentTime - instance->suspectedReleaseStartTime) >= KEY_DEBOUNCE_TIME_MS)
				{
					instance->state = KEY_STATE_IDLE;
				}
			}
			else
			{
				instance->state = KEY_STATE_PRESS_DEBOUNCE;
			}
			break;

		case KEY_STATE_PRESSED:
			if (!currentLevel && instance->lastGpioLevel)
			{
				instance->state = KEY_STATE_RELEASE_DEBOUNCE;
				instance->releaseTime = currentTime;
			}
			else if (currentLevel && instance->isValidPress)
			{
				if ((currentTime - instance->pressStartTime) >= KEY_LONG_PRESS_TIME_MS)
				{
					KeyManager_Send_Event(instance->id, KEY_LONG_PRESS);
					instance->isValidPress = false;
					instance->state = KEY_STATE_RELEASE_DEBOUNCE;
					instance->releaseTime = currentTime;
				}
			}
			break;

		case KEY_STATE_RELEASE_DEBOUNCE:
			if (!currentLevel)
			{
				if ((currentTime - instance->releaseTime) >= KEY_DEBOUNCE_TIME_MS)
				{
					instance->pressCount++;
					if (instance->pressCount == 1)
					{
						instance->state = KEY_STATE_WAIT_DOUBLE_CLICK;
					}
					else if (instance->pressCount >= 2)
					{
						KeyManager_Send_Event(instance->id, KEY_DOUBLE);
						KeyManager_Reset_Instance(instance);
					}
				}
			}
			else
			{
				instance->state = KEY_STATE_PRESSED;
			}
			break;

		case KEY_STATE_WAIT_DOUBLE_CLICK:
			if (currentLevel && !instance->lastGpioLevel)
			{
				instance->state = KEY_STATE_PRESS_DEBOUNCE;
				instance->pressStartTime = currentTime;
			}
			else if ((currentTime - instance->releaseTime) >= KEY_DOUBLE_CLICK_TIME_MS)
			{
				if (instance->pressCount == 1)
				{
					KeyManager_Send_Event(instance->id, KEY_SINGLE);
				}
				KeyManager_Reset_Instance(instance);
			}
			break;

		default:
			KeyManager_Reset_Instance(instance);
			break;
	}

	instance->lastGpioLevel = currentLevel;
}

/**
 * @brief 软件定时器回调函数
 *
 * 由esp_timer每10ms自动调用一次，遍历所有按键实例并执行状态机处理。
 * 这是整个模块的"心跳"，驱动所有按键的状态检测。
 *
 * @param[in] arg 用户参数（未使用，保留给将来扩展）
 *
 * @note 此函数在定时器任务上下文中运行，不是真正的ISR
 * @note 但仍需使用FromISR版本的队列API以保证兼容性
 * @note 执行时间应尽量短（<1ms），避免影响其他定时器
 */
static void KeyManager_Timer_Callback(void *arg)
{
	for (int idx = 0; idx < KEY_NUM; idx++)
	{
		KeyManager_State_Machine_Process(&keyInstanceArr[idx]);
	}
}

/* ======================== 公共API实现 ========================================= */

void KeyManager_Init(void)
{
	ESP_LOGI(TAG, "Initializing KeyManager with software timer");

	keyEventQueue = xQueueCreate(10, sizeof(Key_Event));
	if (keyEventQueue == NULL)
	{
		ESP_LOGE(TAG, "Failed to create event queue");
		return;
	}

	KeyManager_GPIO_Init();

	keyInstanceArr[0].id = KEY_LIVINGROOM;
	keyInstanceArr[0].gpioNum = KEY_LIVINGROOM_GPIO;
	KeyManager_Reset_Instance(&keyInstanceArr[0]);
	keyInstanceArr[0].lastGpioLevel = KeyManager_Read_GPIO(KEY_LIVINGROOM_GPIO);

	keyInstanceArr[1].id = KEY_BEDROOM;
	keyInstanceArr[1].gpioNum = KEY_BEDROOM_GPIO;
	KeyManager_Reset_Instance(&keyInstanceArr[1]);
	keyInstanceArr[1].lastGpioLevel = KeyManager_Read_GPIO(KEY_BEDROOM_GPIO);

	esp_timer_create_args_t timerConfig =
	{
		.callback = &KeyManager_Timer_Callback,
		.arg = NULL,
		.dispatch_method = ESP_TIMER_TASK,
		.name = "KeyTimer",
		.skip_unhandled_events = false,
	};

	if (esp_timer_create(&timerConfig, &keyTimerHandle) != ESP_OK)
	{
		ESP_LOGE(TAG, "Failed to create software timer");
		return;
	}

	if (esp_timer_start_periodic(keyTimerHandle, KEY_TIMER_PERIOD_MS * 1000) != ESP_OK)
	{
		ESP_LOGE(TAG, "Failed to start software timer");
		return;
	}

	ESP_LOGI(TAG, "KeyManager initialized successfully (timer period: %dms)", KEY_TIMER_PERIOD_MS);
}

bool KeyManager_Get_Event(Key_Event *event, uint32_t timeoutMs)
{
	if (keyEventQueue == NULL || event == NULL)
	{
		return false;
	}

	TickType_t ticks = (timeoutMs == 0) ? 0 : pdMS_TO_TICKS(timeoutMs);
	return xQueueReceive(keyEventQueue, event, ticks) == pdTRUE;
}

QueueHandle_t KeyManager_Get_QueueHandle(void)
{
	return keyEventQueue;
}