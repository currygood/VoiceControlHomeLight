#ifndef __ASR_H__
#define __ASR_H__

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief 初始化 ASR（自动语音识别）模块
 *
 * 加载唤醒词模型（"小鱼同学"），配置 AFE（Audio Front-End）音频前端，
 * 创建 AFE 实例，为后续的语音唤醒检测做好准备。
 *
 * 注意：初始化仅完成模型加载和 AFE 配置，不会启动检测任务，
 *       需调用 ASR_Start() 启动后台检测线程。
 *
 * @return ESP_OK  初始化成功
 *         ESP_FAIL 模型加载失败、AFE 配置失败或实例创建失败
 */
esp_err_t ASR_Init(void);

/**
 * @brief 反初始化 ASR 模块
 *
 * 停止检测任务（如果正在运行），销毁 AFE 实例，释放模型资源，
 * 将模块恢复到未初始化状态。
 *
 * @return ESP_OK 反初始化成功
 */
esp_err_t ASR_Deinit(void);

/**
 * @brief 启动 ASR 唤醒词检测任务
 *
 * 创建一个 FreeRTOS 任务，循环从麦克风读取 PCM 数据，
 * 喂入 AFE 进行唤醒词检测。当检测到唤醒词时，通过 TTS 播报响应。
 *
 * 要求：调用前必须先调用 ASR_Init() 完成初始化。
 *
 * @return ESP_OK           启动成功
 *         ESP_ERR_INVALID_STATE 尚未初始化
 *         ESP_ERR_NO_MEM    任务创建失败（内存不足）
 */
esp_err_t ASR_Start(void);

/**
 * @brief 停止 ASR 唤醒词检测任务
 *
 * 设置停止标志，通知检测任务退出循环。任务会在下一次循环迭代时
 * 检测到停止标志并自行删除。
 *
 * @return ESP_OK 停止成功（或本就不在运行）
 */
esp_err_t ASR_Stop(void);

/**
 * @brief 查询 ASR 检测任务是否正在运行
 *
 * @return true  正在运行
 *         false 未运行
 */
bool ASR_IsRunning(void);


#endif
