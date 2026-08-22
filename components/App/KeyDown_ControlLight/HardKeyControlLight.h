/**
 * @file HardKeyControlLight.h
 * @brief 硬按键控制灯模块 - 公共接口声明
 *
 * 本模块实现通过物理按键控制LED灯的功能，作为KeyManager（按键检测）和
 * LED_Control（硬件驱动）之间的业务逻辑层。
 *
 * 主要功能：
 *   - 消费KeyManager产生的按键事件
 *   - 根据事件类型执行不同的灯光控制操作
 *   - 支持单击、双击、长按三种交互模式
 *   - 基于物理状态修改LED参数（先读后写）
 *
 * 按键功能映射：
 * ┌──────────┬────────────┬─────────────┬────────────────┐
 * │  按键    │   单击     │    双击     │     长按       │
 * ├──────────┼────────────┼─────────────┼────────────────┤
 * │ 客厅灯   │ 开关切换   │ 亮度+10%   │ 颜色循环切换   │
 * │ 卧室灯   │ 开关切换   │ 亮度+10%   │ 颜色循环切换   │
 * └──────────┴────────────┴─────────────┴────────────────┘
 *
 * 颜色循环顺序：
 * 红 → 绿 → 蓝 → 黄 → 青 → 紫 → 白 → 红...
 *
 * 设计原则：
 *   - 高内聚：所有按键→灯光的业务逻辑封装在此模块
 *   - 低耦合：不依赖米家、OLED等其他业务模块
 *   - 状态安全：每次操作都从LED_Control读取物理状态再修改
 *   - 错误容忍：单个灯操作失败不影响其他灯
 *
 * 使用方式：
 *   1. 调用 HardKeyControlLight_Init() 初始化内部依赖的KeyManager模块
 *   2. 调用 HardKeyControlLight_Start() 创建FreeRTOS任务开始运行
 *   3. 任务自动阻塞等待按键事件并处理
 *   4. 需要停止时调用 HardKeyControlLight_Stop()
 *
 * @note 本模块是纯消费者，不产生任何外部副作用（如上报状态）
 * @note 状态同步由BemFa等模块自行负责，保持低耦合设计
 */

#ifndef __HARD_KEY_CONTROL_LIGHT_H__
#define __HARD_KEY_CONTROL_LIGHT_H__

#include "esp_err.h"
#include <stdbool.h>

/* ======================== 公共API接口 ======================================== */

/**
 * @brief 初始化硬按键控制灯模块
 *
 * 执行初始化操作：
 *   1. 调用 KeyManager_Init() 初始化底层按键管理器
 *      （配置GPIO、创建队列、启动软件定时器）
 *
 * 此函数只做初始化工作，不启动任何任务。
 * 必须在调用 Start() 之前调用此函数。
 *
 * @return ESP_OK 初始化成功
 * @return ESP_FAIL 初始化失败（内部错误已记录日志）
 *
 * @note 通常在系统启动阶段调用一次即可
 * @note 内部依赖的LED_Control模块应在之前已初始化
 */
esp_err_t HardKeyControlLight_Init(void);

/**
 * @brief 启动硬按键控制灯任务
 *
 * 创建FreeRTOS任务，开始消费按键事件并控制LED灯。
 * 任务会以阻塞方式等待KeyManager的事件队列。
 *
 * 任务特性：
 *   - 栈大小: 4096 字节
 *   - 优先级: 12（较高优先级，保证实时响应）
 *   - 运行模式: 无限循环，直到调用Stop()
 *
 * 工作流程：
 *   1. 阻塞等待按键事件（portMAX_DELAY）
 *   2. 收到事件后根据类型分发处理
 *   3. 从LED_Control读取当前物理状态
 *   4. 计算新状态并写入LED_Control
 *   5. 返回步骤1继续等待
 *
 * @return ESP_OK 任务启动成功或已在运行
 * @return ESP_FAIL 任务创建失败（内存不足等）
 *
 * @warning 重复调用不会创建多个任务，直接返回成功
 * @warning 必须先调用Init()才能Start()
 *
 * 使用示例：
 * @code
 * HardKeyControlLight_Init();
 * // ... 其他初始化 ...
 * HardKeyControlLight_Start();  // 开始响应按键
 * @endcode
 */
esp_err_t HardKeyControlLight_Start(void);

/**
 * @brief 停止硬按键控制灯任务
 *
 * 优雅地停止按键控制任务：
 *   1. 设置停止标志位
 *   2. 等待任务自行退出（最多100ms）
 *   3. 清理资源
 *
 * 停止后任务不再响应按键事件，但KeyManager的状态机仍在运行。
 * 可以重新调用Start()再次启动。
 *
 * @return ESP_OK 停止成功或任务未在运行
 *
 * @note 如果任务正在处理事件，会等待当前事件处理完成
 * @note 停止后可以重新Start()，不需要重新Init()
 */
esp_err_t HardKeyControlLight_Stop(void);

/**
 * @brief 查询任务是否正在运行
 *
 * 检查按键控制任务的当前运行状态。
 *
 * @return true  任务正在运行
 * @return false 任务未运行或已停止
 *
 * 使用示例：
 * @code
 * if (HardKeyControlLight_IsRunning()) {
 *     printf("按键控制任务运行中\n");
 * } else {
 *     printf("按键控制任务已停止\n");
 * }
 * @endcode
 */
bool HardKeyControlLight_IsRunning(void);

#endif /* __HARD_KEY_CONTROL_LIGHT_H__ */