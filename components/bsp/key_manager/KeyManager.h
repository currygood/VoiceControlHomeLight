/**
 * @file KeyManager.h
 * @brief 按键管理模块 - 公共接口声明
 *
 * 本模块提供基于状态机的按键检测功能，支持单击、双击、长按三种事件类型。
 * 使用软件定时器驱动状态机，通过FreeRTOS队列向外部模块传递按键事件。
 *
 * 主要特性：
 *   - 基于有限状态机（FSM）的按键检测算法
 *   - 软件消抖处理（20ms）
 *   - 支持多按键同时管理
 *   - 低电平有效（Active Low）按键输入
 *   - 非阻塞式设计（定时器+队列模式）
 *   - 实时响应（ISR级事件发送）
 *
 * 支持的事件类型：
 *   KEY_SINGLE     单击（短按后松开，无第二次按下）
 *   KEY_DOUBLE     双击（两次快速点击，间隔<300ms）
 *   KEY_LONG_PRESS 长按（持续按下超过1000ms）
 *
 * 使用方式：
 *   1. 调用 KeyManager_Init() 初始化GPIO、队列和软件定时器
 *   2. 定时器自动启动，状态机开始运行（无需外部调用Process）
 *   3. 通过 KeyManager_Get_Event() 获取按键事件（阻塞/非阻塞）
 *   4. 或直接获取队列句柄自行处理
 *
 * @note 初始化后状态机完全自主运行，不需要外部轮询
 * @see KeyManager.c - 状态机实现细节
 */

#ifndef __KEY_MANAGER_H__
#define __KEY_MANAGER_H__

#include "esp_err.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* ======================== 配置参数 =========================================== */

/** @brief 按键数量 */
#define KEY_NUM 					2

/** @brief 卧室灯按键 GPIO 引脚号 */
#define KEY_BEDROOM_GPIO 			40

/** @brief 客厅灯按键 GPIO 引脚号 */
#define KEY_LIVINGROOM_GPIO 		41

/** @brief 按键消抖时间（毫秒），需连续稳定20ms才确认有效 */
#define KEY_DEBOUNCE_TIME_MS 		20

/** @brief 长按判定时间（毫秒），持续按下超过此时间触发长按事件 */
#define KEY_LONG_PRESS_TIME_MS 		1000

/** @brief 双击最大间隔时间（毫秒），两次点击间隔小于此值才算双击 */
#define KEY_DOUBLE_CLICK_TIME_MS 	300

/** @brief 软件定时器周期（毫秒），状态机采样频率=100Hz */
#define KEY_TIMER_PERIOD_MS 		10

/* ======================== 数据类型定义 ======================================= */

/**
 * @brief 按键ID枚举
 */
typedef enum
{
	KEY_LIVINGROOM,    /**< 客厅灯按键 */
	KEY_BEDROOM,       /**< 卧室灯按键 */
} Key_ID;

/**
 * @brief 按键事件类型枚举
 */
typedef enum
{
	KEY_NONE = -1,     /**< 无事件（初始值或无效） */
	KEY_SINGLE,        /**< 单击事件 */
	KEY_DOUBLE,        /**< 双击事件 */
	KEY_LONG_PRESS,    /**< 长按事件 */
} Key_Type;

/**
 * @brief 按键状态机状态枚举
 *
 * 采用双阈值消抖算法，6个状态的完整状态机：
 *
 * 状态转换流程（带消抖保护）：
 *
 * ┌──────────┐
 * │   IDLE   │ ← 初始/重置状态
 * └────┬─────┘
 *      │ 检测到下降沿（按下）
 *      ↓
 * ┌────────────────────┐
 * │  PRESS_DEBOUNCE    │ ← 等待CONFIRM_PRESS_TIME_MS确认按下
 * ├────────────────────┤
 * │ 连续低电平≥20ms     │ → 进入PRESSED（确认真按下）
 * │ 中途变高电平        │ → 进入SUSPECTED_RELEASE（疑似松开）
 * └────┬───────────────┘
 *      │ 消抖成功
 *      ↓
 * ┌──────────┐     长按≥1s      ┌──────────────┐
 * │ PRESSED  │ ──────────────→  │ 发送长按事件  │
 * └────┬─────┘                  └──────┬───────┘
 *      │ 检测到上升沿（释放）           ↓
 *      ↓                         RELEASE_DEBOUNCE
 * ┌────────────────────┐                ↓
 * │ RELEASE_DEBOUNCE    │ ← 等待CONFIRM_RELEASE_TIME_MS确认释放
 * ├────────────────────┤
 * │ 连续高电平≥20ms     │ → 第1次→WAIT_DOUBLE_CLICK / 第2次→发送双击事件
 * │ 中途变低电平        │ → 回到PRESSED继续检测长按
 * └────┬───────────────┘
 *      │ 第1次释放确认
 *      ↓
 * ┌─────────────────────┐
 * │ SUSPECTED_RELEASE    │ ← 新增！疑似释放状态（防抖动误判）
 * ├─────────────────────┤
 * │ 用于PRESS_DEBOUNCE   │
 * │ 阶段中途电平变高时   │
 * ├─────────────────────┤
 * │ 连续高电平≥20ms      │ → 确认是真松开 → 回到IDLE
 * │ 又变回低电平         │ → 说明是抖动 → 回到PRESS_DEBOUNCE
 * └────┬────────────────┘
 *      │ (其他转换与原逻辑相同)
 *
 * 双阈值设计优势：
 * - 完美解决机械按键抖动问题
 * - 对称性：按下和松开都有独立的确认期
 * - 工业级可靠性：能容忍复杂的抖动模式
 */
typedef enum
{
	KEY_STATE_IDLE,              /**< 空闲状态，等待按下 */
	KEY_STATE_PRESS_DEBOUNCE,    /**< 按下消抖中，等待确认有效按下 */
	KEY_STATE_PRESSED,           /**< 已确认按下，检测长按或释放 */
	KEY_STATE_RELEASE_DEBOUNCE,  /**< 释放消抖中，等待确认有效释放 */
	KEY_STATE_WAIT_DOUBLE_CLICK, /**< 等待第二次按下（双击检测窗口期） */
	KEY_STATE_SUSPECTED_RELEASE, /**< 疑似释放状态（防抖动误判） */
} Key_State_Machine;

/**
 * @brief 按键事件结构体
 *
 * 当检测到有效按键操作时，通过队列发送此结构体通知外部模块。
 */
typedef struct
{
	Key_ID id;       /**< 按键ID（哪个按键触发） */
	Key_Type type;   /**< 事件类型（单击/双击/长按） */
} Key_Event;

/* ======================== 公共API接口 ======================================== */

/**
 * @brief 初始化按键管理器
 *
 * 执行以下初始化操作：
 *   1. 创建FreeRTOS队列用于事件传递（容量10个事件）
 *   2. 配置GPIO为输入模式 + 内部上拉电阻
 *   3. 初始化所有按键实例的状态机
 *   4. 创建并启动10ms周期的软件定时器
 *
 * 调用后状态机将完全自主运行，无需外部干预。
 *
 * @note 必须在创建任务前调用此函数
 * @note 此函数只需调用一次，重复调用可能导致资源泄漏
 *
 * @return 无返回值（内部错误会记录日志但不影响程序继续运行）
 */
void KeyManager_Init(void);

/**
 * @brief 获取按键事件（阻塞/非阻塞模式）
 *
 * 从事件队列中读取一个按键事件。如果队列为空，根据timeoutMs参数决定行为：
 *   - timeoutMs = 0：立即返回，不等待
 *   - timeoutMs > 0：等待指定的毫秒数
 *   - timeoutMs = portMAX_DELAY（或0xFFFFFFFF）：永久阻塞直到收到事件
 *
 * @param[out] event 用于接收按键事件的指针（调用者提供内存）
 * @param[in]  timeoutMs 超时时间（毫秒），0表示非阻塞，portMAX_DELAY表示无限等待
 *
 * @return true  成功获取到一个事件（event已被填充）
 * @return false 超时无事件、参数无效或队列未创建
 *
 * @warning event指针不能为NULL，否则立即返回false
 *
 * 使用示例：
 * @code
 * Key_Event event;
 * if (KeyManager_Get_Event(&event, portMAX_DELAY)) {
 *     switch (event.type) {
 *         case KEY_SINGLE:    handle_single_click(event.id); break;
 *         case KEY_DOUBLE:    handle_double_click(event.id); break;
 *         case KEY_LONG_PRESS:handle_long_press(event.id); break;
 *     }
 * }
 * @endcode
 */
bool KeyManager_Get_Event(Key_Event *event, uint32_t timeoutMs);

/**
 * @brief 获取事件队列句柄
 *
 * 返回内部使用的FreeRTOS队列句柄，允许调用者使用更灵活的方式处理事件。
 * 例如可以使用 xQueueSelectFromSet() 同时监听多个队列。
 *
 * @return QueueHandle_t 有效的队列句柄，如果未初始化则返回NULL
 *
 * @note 一般情况下建议使用 KeyManager_Get_Event() 接口
 * @note 返回的句柄不应被删除或修改，仅供读取
 *
 * 使用示例：
 * @code
 * QueueHandle_t keyQueue = KeyManager_Get_QueueHandle();
 * if (keyQueue != NULL) {
 *     // 将keyQueue加入队列集，与其他事件源统一处理
 *     xQueueAddToSet(keyQueue, queueSet);
 * }
 * @endcode
 */
QueueHandle_t KeyManager_Get_QueueHandle(void);

#endif /* __KEY_MANAGER_H__ */