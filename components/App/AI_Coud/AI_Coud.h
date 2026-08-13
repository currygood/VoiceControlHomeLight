/**
 * @file AI_Coud.h
 * @brief AI Cloud 云端语音对话模块接口
 *
 * 本模块基于 WebSocket 协议与豆包（ByteDance）云端大模型进行实时语音对话。
 * 通过麦克风采集的 PCM 音频上传至云端，云端返回 OGG_OPUS 格式的 AI 语音回复，
 * 同时支持 Function Call（函数调用）机制，实现本地灯光设备的智能控制。
 *
 * 核心流程：
 *   1. 初始化阶段：配置 WebSocket 连接参数 → 创建上传任务
 *   2. 连接阶段：建立 WebSocket 连接 → 发送 session.create 配置 AI 角色与工具
 *   3. 对话阶段：持续上传 PCM 音频 → 接收云端文本/音频/函数调用事件
 *   4. 断连阶段：清理部分缓冲区 → 通知播放模块结束
 *
 * 支持的 Function Call：
 *   - set_light：控制灯光开关、亮度、颜色（卧室/客厅）
 *   - get_light：查询当前灯光状态
 *
 * 依赖模块：
 *   - Audio_Stream：音频环形缓冲区（上传 PCM 和播放 OGG）
 *   - LED_Control：灯光设备控制
 *   - WifiManager：Wi-Fi 连接状态检测
 */

#ifndef __AI_CLOUD_H__
#define __AI_CLOUD_H__

#include "esp_err.h"
#include "esp_log.h"
#include <stdbool.h>

/**
 * @brief 初始化 AI Cloud 模块
 *
 * 配置 WebSocket 客户端参数，保存 API Key，注册事件回调，
 * 创建音频上传任务，为后续的云端对话做好准备。
 *
 * 注意：初始化仅完成连接参数配置和任务创建，不会建立 WebSocket 连接，
 *       需调用 AiCloud_Start() 启动连接。
 *
 * @param url     WebSocket 服务器地址（如 "wss://openspeech.bytedance.com/api/v3/ws/realtime"）
 * @param token   API 认证令牌（X-Api-Key 请求头）
 * @return ESP_OK 初始化成功
 */
esp_err_t AiCloud_Init(const char *url, const char *token);

/**
 * @brief 启动 WebSocket 连接
 *
 * 建立与云端服务器的 WebSocket 连接，连接成功后自动发送
 * session.create 事件配置 AI 角色和工具。
 *
 * @return ESP_OK   启动成功
 *         ESP_FAIL 客户端未初始化
 */
esp_err_t AiCloud_Start(void);

/**
 * @brief 停止 WebSocket 连接
 *
 * 断开与云端服务器的 WebSocket 连接，停止音频上传和接收。
 * 播放任务会先播完队列中剩余数据，再销毁解码器。
 *
 * @return ESP_OK 停止成功
 */
esp_err_t AiCloud_Stop(void);

/**
 * @brief AI Cloud 音频上传任务的主循环
 *
 * 该任务作为 FreeRTOS 线程运行，持续执行以下流程：
 *   1. 从 AudioStream 环形缓冲区读取 PCM 16-bit 音频数据
 *   2. 将 PCM 数据进行 Base64 编码
 *   3. 构造 input_audio_buffer.append JSON 消息
 *   4. 通过 WebSocket 发送至云端
 *
 * 当 WebSocket 未连接时，持续读取并丢弃音频数据以推进读指针，
 * 避免积压旧数据导致唤醒后上传乱序音频。
 *
 * @param pvParameters FreeRTOS 任务参数（本任务未使用，传入 NULL）
 */
void AiCloud_task(void *pvParameters);

/**
 * @brief 查询当前是否正在与 AI 对话中
 *
 * @return true  正在对话
 *         false 未在对话
 */
bool AiCloud_IsChat(void);

#endif