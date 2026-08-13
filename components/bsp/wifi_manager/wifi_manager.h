/**
 * @file wifi_manager.h
 * @brief WiFi 管理器接口（连接管理 + AP 配网 + WebSocket 服务器）
 *
 * 本模块提供完整的 WiFi 连接管理和配网功能，是设备网络通信的核心模块。
 * 支持两种工作模式：
 *   - STA 模式：作为普通 WiFi 客户端连接路由器，用于 AI 语音对话
 *   - AP 配网模式：创建热点提供 Web 配网页面，用于首次配置 WiFi 凭据
 *
 * 核心功能：
 *   - WiFi 连接管理（STA 模式连接/重连/断开）
 *   - AP 配网模式（创建热点 + WebSocket 服务器 + 配网页面）
 *   - NVS 凭据存储（WiFi SSID/密码持久化存储）
 *   - WiFi 扫描（获取周围热点列表）
 *   - 连接状态回调（通知上层应用 WiFi 连接状态变化）
 *
 * 工作流程：
 *   首次启动 → 无 NVS 凭据 → 启动 AP 配网 → 用户通过网页输入凭据 → 保存到 NVS → 连接 WiFi
 *   后续启动 → 有 NVS 凭据 → 直接连接 WiFi → 连接成功 → 进入正常工作模式
 *
 * 依赖：
 *   - ESP-IDF WiFi Driver
 *   - ESP-IDF HTTP Server
 *   - ESP-IDF NVS
 *   - cJSON
 */

#ifndef __WIFI_MANAGER_H__
#define __WIFI_MANAGER_H__

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ======================== NVS 存储键名 =========================================== */

/** NVS 命名空间（所有 WiFi 配置存储在此命名空间下） */
#define NVS_NAMESPACE           "watch_cfg"
/** NVS 键名：WiFi SSID */
#define NVS_WIFI_SSID           "wifi_ssid"
/** NVS 键名：WiFi 密码 */
#define NVS_WIFI_PASS           "wifi_pass"
/** NVS 键名：配网完成标志（1 = 已完成配网，0 = 未配网） */
#define NVS_PROV_DONE           "prov_done"

/* ======================== 凭据缓冲区大小 ========================================= */

/** SSID 最大长度（含结束符），WiFi 标准 SSID 最长 32 字节 */
#define CRED_SSID_MAX_LEN       64
/** 密码最大长度（含结束符），WPA2 密码最长 63 字节 */
#define CRED_PASS_MAX_LEN       128

/* ======================== AP 配网配置 ============================================ */

/** AP 配网热点名称（设备创建的热点，用户手机连接此热点进行配网） */
#define AP_SSID                 "EpiWatch_AP"
/** AP 配网热点密码 */
#define AP_PASSWORD             "watch1234"
/** AP 配网热点 IP 地址（设备作为网关的 IP） */
#define AP_Provision_IP         "192.168.100.1"

/* ======================== 回调函数类型 =========================================== */

/**
 * @brief WiFi 连接状态变化回调
 * @param connected true = 已连接，false = 已断开
 */
typedef void (*p_wifi_state_callback)(bool connected);

/**
 * @brief WiFi 扫描完成回调
 * @param num        扫描到的 AP 数量
 * @param ap_records AP 信息数组指针
 */
typedef void (*p_wifi_scan_callback)(int num, wifi_ap_record_t *ap_records);

/**
 * @brief WebSocket 接收数据回调
 * @param payload 接收到的数据负载
 * @param len     数据长度
 */
typedef void (*ws_receive_cb)(uint8_t* payload, int len);

/* ======================== WebSocket 配置结构体 =================================== */

/**
 * @brief WebSocket 服务器配置
 *
 * 用于启动 AP 配网时的 WebSocket 服务，提供配网页面和接收配网数据。
 */
typedef struct {
    const char* html_code;      /**< 配网页面 HTML 内容 */
    ws_receive_cb receive_fn;   /**< WebSocket 接收回调（处理用户输入的 WiFi 凭据） */
} ws_cfg_t;

/* ======================== WiFi 核心功能 API ====================================== */

/**
 * @brief 初始化 WiFi 并尝试使用 NVS 中的凭据连接
 *
 * 初始化流程：
 *   1. 创建 WiFi 事件组（用于等待连接结果）
 *   2. 初始化 TCP/IP 协议栈和网络接口
 *   3. 初始化 WiFi 驱动
 *   4. 注册 IP 事件回调
 *   5. 检查 NVS 中是否有已保存的凭据
 *      - 有凭据：尝试连接 WiFi，等待连接结果
 *      - 无凭据或连接失败：启动 AP 配网模式
 *
 * @return ESP_OK 连接成功或进入配网模式
 *         ESP_FAIL 连接失败或初始化失败
 */
esp_err_t WifiManager_Wifi_Init(void);

/**
 * @brief 启动 AP 配网模式
 *
 * 创建 WiFi 热点并启动 WebSocket 服务器，提供配网页面。
 * 用户通过手机连接热点，在浏览器中打开配网页面，输入 WiFi 凭据。
 *
 * @param enable 固定传入 true（保留参数，兼容旧接口）
 */
void WifiManager_ap_wifi_apcfg(bool enable);

/**
 * @brief 连接指定的 WiFi 热点
 *
 * 直接使用传入的 SSID 和密码连接 WiFi。如果当前是 AP 模式，会先切换到 STA 模式。
 * 支持重连机制（最多重试 MAX_RETRY_COUNT 次）。
 *
 * @param ssid     WiFi 热点名称（最长 31 字节）
 * @param password WiFi 密码（最长 63 字节）
 * @return ESP_OK 连接已发起
 *         ESP_FAIL 参数无效（SSID 或密码过长）
 */
esp_err_t WifiManager_WifiConnect(const char* ssid, const char* password);

/**
 * @brief 扫描周围 WiFi 热点
 *
 * 异步扫描，完成后通过回调函数返回扫描结果。
 * 使用信号量保证同一时间只有一个扫描任务。
 *
 * @param f 扫描结果回调函数
 * @return ESP_OK 扫描任务已创建
 *         ESP_FAIL 扫描任务创建失败（可能已有扫描正在进行）
 */
esp_err_t WifiManager_WifiScan(p_wifi_scan_callback f);

/**
 * @brief 检查当前是否已连接 WiFi
 *
 * @return true 已连接（获取到 IP 地址）
 *         false 未连接
 */
bool WifiManager_IsConnected(void);

/**
 * @brief 启动 AP+STA 模式（配网时使用）
 *
 * 切换 WiFi 为 AP+STA 混合模式，配置热点参数（SSID、密码、IP 地址）。
 * 设备同时作为热点（供手机连接）和客户端（用于后续连接目标 WiFi）。
 *
 * @return ESP_OK 模式切换成功
 */
esp_err_t WifiManager_AP(void);

/* ======================== NVS 凭据管理 =========================================== */

/**
 * @brief 保存 WiFi 凭据到 NVS
 *
 * 将 SSID 和密码保存到 NVS 闪存，设备重启后仍然有效。
 *
 * @param ssid WiFi SSID
 * @param pass WiFi 密码
 * @return ESP_OK 保存成功
 *         其他值 保存失败
 */
esp_err_t NVS_Save_Wifi_Credentials(const char *ssid, const char *pass);

/**
 * @brief 从 NVS 读取 WiFi 凭据
 *
 * @param ssid     输出缓冲区（存放 SSID）
 * @param ssid_len SSID 缓冲区大小
 * @param pass     输出缓冲区（存放密码）
 * @param pass_len 密码缓冲区大小
 * @return ESP_OK 读取成功
 *         其他值 读取失败（凭据不存在或 NVS 错误）
 */
esp_err_t NVS_Load_Wifi_Credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len);

/**
 * @brief 检查 NVS 中是否有有效 WiFi 凭据
 *
 * 通过读取 NVS_PROV_DONE 标志判断是否已完成配网。
 *
 * @return true 有凭据（已完成配网）
 *         false 无凭据（未配网）
 */
bool NVS_Has_Wifi_Credentials(void);

/**
 * @brief 清除所有 NVS 配置（含配网标志）
 *
 * 清除 WiFi SSID、密码和配网完成标志，恢复为未配网状态。
 * 下次启动将进入 AP 配网模式。
 *
 * @return ESP_OK 清除成功
 */
esp_err_t NVS_Clear_All_Credentials(void);

/* ======================== WebSocket 辅助 ========================================= */

/**
 * @brief 启动 WebSocket 服务器
 *
 * 注册 HTTP GET 处理器（返回配网页面）和 WebSocket 处理器（接收配网数据）。
 *
 * @param cfg WebSocket 配置（HTML 页面内容和接收回调）
 * @return ESP_OK 启动成功
 *         ESP_FAIL 启动失败
 */
esp_err_t web_ws_start(ws_cfg_t *cfg);

/**
 * @brief 停止 WebSocket 服务器
 *
 * @return ESP_OK 停止成功
 */
esp_err_t web_ws_stop(void);

/**
 * @brief 通过 WebSocket 发送数据
 *
 * @param data 数据缓冲区
 * @param len  数据长度
 * @return ESP_OK 发送成功
 */
esp_err_t web_ws_send(uint8_t* data, int len);

/* ======================== 内部初始化 ============================================= */

/**
 * @brief 初始化 AP 配网模式
 *
 * 加载配网 HTML 页面，创建事件组，注册事件回调，创建配网任务。
 * 通常由 WifiManager_Wifi_Init() 在无有效凭据时自动调用。
 *
 * @param f WiFi 状态变化回调函数（可为 NULL）
 */
void ap_wifi_init(p_wifi_state_callback f);

#ifdef __cplusplus
}
#endif

#endif