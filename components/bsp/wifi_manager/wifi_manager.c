/**
 * @file wifi_manager.c
 * @brief WiFi 管理器实现（连接管理 + AP 配网 + WebSocket 服务器）
 *
 * 本模块提供完整的 WiFi 连接管理和配网功能实现。
 *
 * 核心流程：
 *   1. WifiManager_Wifi_Init() 作为主入口
 *      - 初始化网络协议栈和 WiFi 驱动
 *      - 检查 NVS 凭据：有则连接，无则启动 AP 配网
 *   2. STA 模式连接流程
 *      - 设置 WiFi 模式为 STA → 配置 SSID/密码 → 连接 → 等待获取 IP
 *      - 支持自动重连（最多 5 次）
 *   3. AP 配网流程
 *      - 切换到 AP+STA 模式 → 创建热点 → 启动 HTTP/WebSocket 服务器
 *      - 用户通过网页输入凭据 → WebSocket 接收 → 保存到 NVS → 连接 WiFi
 *
 * 事件处理：
 *   - wifi_event_handler：处理 STA 模式下的 WiFi 和 IP 事件
 *   - ap_wifi_event_handler：处理 AP 配网模式下的 WiFi 和 IP 事件
 *
 * 依赖：
 *   - ESP-IDF WiFi Driver
 *   - ESP-IDF HTTP Server
 *   - ESP-IDF NVS
 *   - ESP-IDF SPIFFS
 *   - cJSON
 */

#include "wifi_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "lwip/ip4_addr.h"
#include "esp_spiffs.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>

/* ======================== 模块内部宏定义 ========================================= */

/** NVS 凭据连接超时时间（毫秒），等待 WiFi 获取 IP 的最大时间 */
#define WAIT_NVSWIFIMESSAGE_TOCONNECTED_TIMEOUT 120000

/** 最大重连次数，超过后停止重连 */
#define MAX_RETRY_COUNT 5

/** AP 配网 HTML 页面在 SPIFFS 中的路径 */
#define INDEX_HTML_PATH "/spiffs/apcfg.html"

/* ======================== 模块静态变量 =========================================== */

static const char *TAG = "WIFI_MANAGER";                    /**< 日志标签 */

/* ---------- STA 模式相关状态 ---------- */

/** WiFi 事件组（用于等待 STA 连接结果，BIT0 = 连接成功） */
static EventGroupHandle_t wifi_event_group = NULL;

/** 是否已连接到 WiFi（获取到 IP 地址） */
static bool is_sta_connected = false;

/** 当前重连次数 */
static int retry_count = 0;

/* ---------- AP 配网相关状态 ---------- */

/** AP 配网事件组（BIT0 = 配网完成，收到用户凭据） */
static EventGroupHandle_t apcfg_event = NULL;

/** STA 网络接口句柄（配网模式使用） */
static esp_netif_t *esp_netif_sta = NULL;

/** AP 网络接口句柄（配网模式使用） */
static esp_netif_t *esp_netif_ap = NULL;

/** WiFi 状态变化回调函数指针 */
static p_wifi_state_callback wifi_state_cb = NULL;

/** 配网页面 HTML 内容（从 SPIFFS 加载） */
static char* index_html = NULL;

/** 当前 WiFi SSID（配网时从 WebSocket 接收） */
static char current_ssid[CRED_SSID_MAX_LEN] = {0};

/** 当前 WiFi 密码（配网时从 WebSocket 接收） */
static char current_password[CRED_PASS_MAX_LEN] = {0};

/** WebSocket 接收回调函数 */
static ws_receive_cb ws_receive_fn = NULL;

/** HTTP/WebSocket 服务器句柄 */
static httpd_handle_t http_ws_server = NULL;

/** WebSocket 客户端 socket 文件描述符 */
static int client_sockfd = -1;

/** 扫描任务句柄 */
static TaskHandle_t Scan_Task_Handle = NULL;

/** AP 配网任务句柄 */
static TaskHandle_t AP_Task_Handle = NULL;

/** 扫描信号量（保证同一时间只有一个扫描任务） */
static SemaphoreHandle_t scan_sem = NULL;

/** 配网完成标志（WebSocket 收到凭据后置 true） */
static bool provision_done = false;

/* ======================== WiFi 事件回调 ========================================== */

/**
 * @brief STA 模式 WiFi 事件处理器
 *
 * 处理三种事件：
 *   - WIFI_EVENT_STA_START：STA 模式已启动（无需额外操作）
 *   - WIFI_EVENT_STA_DISCONNECTED：WiFi 断开，触发自动重连
 *   - IP_EVENT_STA_GOT_IP：获取到 IP 地址，标记连接成功，触发回调
 *
 * 自动重连机制：
 *   断开后延迟 1.5 秒重连，最多重试 MAX_RETRY_COUNT 次。
 *   超过最大重试次数后停止重连。
 *
 * @param arg   未使用
 * @param base  事件基（WIFI_EVENT 或 IP_EVENT）
 * @param id    事件 ID
 * @param data  事件数据
 */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
    {
        /* STA 模式启动完成，等待连接 */
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        /* WiFi 断开，触发自动重连 */
        if (retry_count < MAX_RETRY_COUNT)
        {
            ESP_LOGW(TAG, "WiFi 断开，第 %d/%d 次重试...", retry_count + 1, MAX_RETRY_COUNT);
            vTaskDelay(pdMS_TO_TICKS(1500));
            esp_wifi_connect();
            retry_count++;
        }
        else
        {
            ESP_LOGE(TAG, "达到最大重试次数，WiFi 仍无法连接");
        }
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        /* 获取到 IP 地址，连接成功 */
        retry_count = 0;
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGW(TAG, "WiFi 连接成功！IP: " IPSTR, IP2STR(&event->ip_info.ip));
        esp_wifi_set_ps(WIFI_PS_NONE);  /* 关闭省电模式，保证低延迟 */
        if (wifi_event_group)
        {
            xEventGroupSetBits(wifi_event_group, BIT0);
        }
        is_sta_connected = true;
        if (wifi_state_cb) wifi_state_cb(true);
    }
}

/* ======================== AP 配网事件回调 ======================================== */

/**
 * @brief AP 配网模式 WiFi 事件处理器
 *
 * 处理 AP 配网模式下的所有 WiFi 和 IP 事件：
 *   - WIFI_EVENT_STA_START / STA_CONNECTED / STA_DISCONNECTED：STA 侧状态变化
 *   - WIFI_EVENT_AP_STACONNECTED / AP_STADISCONNECTED：AP 侧客户端连接/断开
 *   - IP_EVENT_STA_GOT_IP：STA 侧获取到 IP（配网后连接目标 WiFi 成功）
 *
 * 与 STA 模式事件处理器的区别：
 *   本处理器同时监听 AP 和 STA 两侧事件，因为配网时设备处于 AP+STA 混合模式。
 *
 * @param arg       未使用
 * @param event_base 事件基（WIFI_EVENT 或 IP_EVENT）
 * @param event_id  事件 ID
 * @param event_data 事件数据
 */
static void ap_wifi_event_handler(void* arg, esp_event_base_t event_base,
                                  int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT)
    {
        switch (event_id)
        {
            case WIFI_EVENT_STA_START:
                break;

            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "Connected to AP");
                break;

            case WIFI_EVENT_STA_DISCONNECTED:
                /* STA 断开，通知上层并尝试重连 */
                if (is_sta_connected)
                {
                    if (wifi_state_cb) wifi_state_cb(false);
                    is_sta_connected = false;
                }
                if (retry_count < MAX_RETRY_COUNT)
                {
                    esp_wifi_connect();
                    retry_count++;
                }
                ESP_LOGI(TAG, "connect to the AP fail, retry now");
                break;

            case WIFI_EVENT_AP_STACONNECTED:
            {
                /* 有客户端连接到了设备热点 */
                wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
                ESP_LOGI(TAG, "Station "MACSTR" joined, AID=%d",
                         MAC2STR(event->mac), event->aid);
                break;
            }

            case WIFI_EVENT_AP_STADISCONNECTED:
            {
                /* 客户端从设备热点断开 */
                wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
                ESP_LOGI(TAG, "Station "MACSTR" left, AID=%d",
                         MAC2STR(event->mac), event->aid);
                break;
            }

            default:
                break;
        }
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ESP_LOGI(TAG, "Get ip address");
        is_sta_connected = true;
        if (wifi_state_cb) wifi_state_cb(true);
    }
}

/* ======================== NVS 凭据管理 =========================================== */

/**
 * @brief 保存 WiFi 凭据到 NVS
 *
 * 将 SSID 和密码保存到 NVS 闪存，设备重启后仍然有效。
 * 操作流程：打开 NVS → 写入 SSID → 写入密码 → 提交 → 关闭 NVS。
 *
 * @param ssid WiFi SSID
 * @param pass WiFi 密码
 * @return ESP_OK 保存成功
 *         其他值 保存失败
 */
esp_err_t NVS_Save_Wifi_Credentials(const char *ssid, const char *pass)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(handle, NVS_WIFI_SSID, ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, NVS_WIFI_PASS, pass);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "WiFi 凭据已保存到 NVS");
    }
    else
    {
        ESP_LOGE(TAG, "WiFi 凭据保存失败: %s", esp_err_to_name(err));
    }
    return err;
}

/**
 * @brief 从 NVS 读取 WiFi 凭据
 *
 * 操作流程：打开 NVS（只读）→ 读取 SSID → 读取密码 → 关闭 NVS。
 *
 * @param ssid     输出缓冲区
 * @param ssid_len SSID 缓冲区大小
 * @param pass     输出缓冲区
 * @param pass_len 密码缓冲区大小
 * @return ESP_OK 读取成功
 *         其他值 读取失败
 */
esp_err_t NVS_Load_Wifi_Credentials(char *ssid, size_t ssid_len,
                                    char *pass, size_t pass_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    err = nvs_get_str(handle, NVS_WIFI_SSID, ssid, &ssid_len);
    if (err == ESP_OK)
    {
        err = nvs_get_str(handle, NVS_WIFI_PASS, pass, &pass_len);
    }
    nvs_close(handle);
    return err;
}

/**
 * @brief 检查 NVS 中是否有有效 WiFi 凭据
 *
 * 通过读取 NVS_PROV_DONE 标志判断是否已完成配网。
 * 标志值为 1 表示已完成配网，0 或不存在表示未配网。
 *
 * @return true 有凭据
 *         false 无凭据
 */
bool NVS_Has_Wifi_Credentials(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
    {
        return false;
    }
    uint8_t done = 0;
    esp_err_t err = nvs_get_u8(handle, NVS_PROV_DONE, &done);
    nvs_close(handle);
    return (err == ESP_OK && done == 1);
}

/**
 * @brief 清除所有 NVS 配置
 *
 * 清除 WiFi SSID、密码和配网完成标志，恢复为未配网状态。
 * 下次启动将进入 AP 配网模式。
 *
 * @return ESP_OK 清除成功
 */
esp_err_t NVS_Clear_All_Credentials(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    nvs_erase_key(handle, NVS_WIFI_SSID);
    nvs_erase_key(handle, NVS_WIFI_PASS);
    nvs_erase_key(handle, NVS_PROV_DONE);
    err = nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGW(TAG, "NVS 凭据已全部清除");
    return err;
}

/* ======================== AP 配网辅助函数 ======================================== */

/**
 * @brief 从 SPIFFS 加载配网 HTML 页面
 *
 * 加载流程：
 *   1. 注册 SPIFFS 分区（如果未挂载）
 *   2. 检查 apcfg.html 文件是否存在
 *   3. 分配内存并读取文件内容
 *
 * @return HTML 页面字符串指针（调用方负责释放），失败返回 NULL
 */
static char* initi_web_page_buffer(void)
{
    /* 注册 SPIFFS 分区 */
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = false
    };
    if (!esp_spiffs_mounted("storage"))
    {
        esp_vfs_spiffs_register(&conf);
    }

    /* 检查文件是否存在 */
    struct stat st;
    if (stat(INDEX_HTML_PATH, &st))
    {
        ESP_LOGE(TAG, "apcfg.html not found");
        return NULL;
    }

    /* 分配内存并读取文件 */
    char* page = (char*)malloc(st.st_size + 1);
    if (!page) return NULL;
    memset(page, 0, st.st_size + 1);

    FILE *fp = fopen(INDEX_HTML_PATH, "r");
    if (fread(page, st.st_size, 1, fp) == 0)
    {
        free(page);
        page = NULL;
        ESP_LOGE(TAG, "fread failed");
    }
    fclose(fp);
    return page;
}

/**
 * @brief WebSocket 请求处理器
 *
 * 处理两种 HTTP 请求：
 *   - HTTP_GET：WebSocket 握手，记录客户端 socket 文件描述符
 *   - HTTPD_WS_TYPE_TEXT：接收 WebSocket 文本帧，调用 ws_receive_fn 回调
 *
 * @param req HTTP 请求句柄
 * @return ESP_OK 处理成功
 */
static esp_err_t handle_ws_req(httpd_req_t *req)
{
    if (req->method == HTTP_GET)
    {
        /* WebSocket 握手请求 */
        ESP_LOGI(TAG, "Handshake done, new WebSocket connection");
        client_sockfd = httpd_req_to_sockfd(req);
        return ESP_OK;
    }

    /* 接收 WebSocket 数据帧 */
    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));

    /* 第一步：获取帧长度 */
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) return ret;

    if (ws_pkt.len)
    {
        /* 分配缓冲区接收数据 */
        buf = calloc(1, ws_pkt.len + 1);
        if (!buf) return ESP_ERR_NO_MEM;
        ws_pkt.payload = buf;

        /* 第二步：接收实际数据 */
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK)
        {
            free(buf);
            return ret;
        }
        ESP_LOGI(TAG, "Got WebSocket packet: %s", ws_pkt.payload);
    }

    /* 处理文本帧 */
    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT)
    {
        if (ws_receive_fn) ws_receive_fn(ws_pkt.payload, ws_pkt.len);
        free(buf);
    }
    return ESP_OK;
}

/**
 * @brief HTTP GET 请求处理器（返回配网 HTML 页面）
 *
 * @param req HTTP 请求句柄
 * @return ESP_OK 发送成功
 *         ESP_FAIL 发送失败
 */
static esp_err_t get_req_handler(httpd_req_t *req)
{
    if (index_html)
    {
        return httpd_resp_send(req, index_html, HTTPD_RESP_USE_STRLEN);
    }
    return ESP_FAIL;
}

/**
 * @brief 启动 WebSocket 服务器
 *
 * 注册两个 URI 处理器：
 *   - "/"：HTTP GET，返回配网 HTML 页面
 *   - "/ws"：WebSocket 端点，用于接收配网数据
 *
 * @param cfg WebSocket 配置
 * @return ESP_OK 启动成功
 *         ESP_FAIL 启动失败
 */
esp_err_t web_ws_start(ws_cfg_t *cfg)
{
    if (!cfg || !cfg->html_code) return ESP_FAIL;

    index_html = (char*)cfg->html_code;
    ws_receive_fn = cfg->receive_fn;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    /* 配网页面 URI */
    httpd_uri_t uri_get = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = get_req_handler,
    };

    /* WebSocket 端点 */
    httpd_uri_t ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = handle_ws_req,
        .is_websocket = true
    };

    if (httpd_start(&http_ws_server, &config) == ESP_OK)
    {
        httpd_register_uri_handler(http_ws_server, &uri_get);
        httpd_register_uri_handler(http_ws_server, &ws);
    }
    return ESP_OK;
}

/**
 * @brief 停止 WebSocket 服务器
 *
 * @return ESP_OK 停止成功
 */
esp_err_t web_ws_stop(void)
{
    if (http_ws_server)
    {
        esp_err_t ret = httpd_stop(http_ws_server);
        http_ws_server = NULL;
        return ret;
    }
    return ESP_OK;
}

/**
 * @brief 通过 WebSocket 发送数据
 *
 * 向已连接的客户端发送文本帧。
 *
 * @param data 数据缓冲区
 * @param len  数据长度
 * @return ESP_OK 发送成功
 */
esp_err_t web_ws_send(uint8_t* data, int len)
{
    httpd_ws_frame_t pkt = {
        .payload = data,
        .len = len,
        .type = HTTPD_WS_TYPE_TEXT
    };
    return httpd_ws_send_data(http_ws_server, client_sockfd, &pkt);
}

/* ======================== WiFi 扫描 ============================================== */

/**
 * @brief WiFi 扫描完成回调
 *
 * 将扫描结果构造为 JSON 格式，通过 WebSocket 发送给配网页面。
 * JSON 格式：{ "wifi_list": [ { "ssid": "...", "rssi": -50, "encrypted": true }, ... ] }
 *
 * @param numbers    扫描到的 AP 数量
 * @param ap_records AP 信息数组
 */
static void wifi_scan_finish_handle(int numbers, wifi_ap_record_t *ap_records)
{
    /* 构造 JSON 响应 */
    cJSON* root = cJSON_CreateObject();
    cJSON* wifilist = cJSON_AddArrayToObject(root, "wifi_list");

    for (int i = 0; i < numbers; i++)
    {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", (char*)ap_records[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", ap_records[i].rssi);
        cJSON_AddBoolToObject(item, "encrypted", ap_records[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(wifilist, item);
    }

    char* data = cJSON_Print(root);
    web_ws_send((uint8_t*)data, strlen(data));
    cJSON_free(data);
    cJSON_Delete(root);
}

/**
 * @brief 扫描任务函数（在独立 Task 中运行）
 *
 * 执行阻塞式 WiFi 扫描，完成后调用回调函数并释放信号量。
 * 扫描完成后自动删除自身任务。
 *
 * @param param 扫描完成回调函数指针
 */
static void scan_task(void* param)
{
    p_wifi_scan_callback callback = (p_wifi_scan_callback)param;
    uint16_t number = 20;
    wifi_ap_record_t *ap_info = malloc(sizeof(wifi_ap_record_t) * number);
    uint16_t ap_count = 0;

    /* 执行阻塞式扫描 */
    esp_wifi_scan_start(NULL, true);
    esp_wifi_scan_get_ap_num(&ap_count);
    esp_wifi_scan_get_ap_records(&number, ap_info);

    /* 回调通知扫描结果 */
    if (callback) callback(number, ap_info);

    /* 释放信号量，允许下次扫描 */
    xSemaphoreGive(scan_sem);
    vTaskDelete(NULL);
}

/**
 * @brief 启动 WiFi 扫描
 *
 * 使用信号量保证同一时间只有一个扫描任务。
 * 如果已有扫描任务在进行中，返回 ESP_FAIL。
 *
 * @param f 扫描结果回调函数
 * @return ESP_OK 扫描已启动
 *         ESP_FAIL 扫描正在进行中或创建任务失败
 */
esp_err_t WifiManager_WifiScan(p_wifi_scan_callback f)
{
    /* 初始化信号量（仅首次） */
    if (!scan_sem)
    {
        scan_sem = xSemaphoreCreateBinary();
        xSemaphoreGive(scan_sem);
    }

    /* 尝试获取信号量（非阻塞），防止重复扫描 */
    if (pdTRUE == xSemaphoreTake(scan_sem, 0))
    {
        esp_wifi_clear_ap_list();
        if (pdTRUE == xTaskCreatePinnedToCore(scan_task, "scan", 8192, f, 3, &Scan_Task_Handle, 0))
        {
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

/* ======================== WiFi 连接管理 ========================================== */

/**
 * @brief 启动 AP+STA 模式（配网时使用）
 *
 * 配置流程：
 *   1. 断开当前 WiFi 连接
 *   2. 切换到 AP+STA 混合模式
 *   3. 配置 AP 热点参数（SSID、密码、信道、最大连接数）
 *   4. 配置 AP 静态 IP（192.168.100.1/24）
 *   5. 启动 DHCP 服务器
 *   6. 启动 WiFi
 *
 * @return ESP_OK 模式切换成功
 */
esp_err_t WifiManager_AP(void)
{
    /* 断开当前连接，切换到 AP+STA 混合模式 */
    esp_wifi_disconnect();
    esp_wifi_set_mode(WIFI_MODE_APSTA);

    /* 配置 AP 热点 */
    wifi_config_t wifi_config = {
        .ap = {
            .channel = 1,
            .max_connection = 2,
            .authmode = WIFI_AUTH_WPA2_PSK,
        }
    };
    snprintf((char*)wifi_config.ap.ssid, 31, "%s", AP_SSID);
    wifi_config.ap.ssid_len = strlen(AP_SSID);
    snprintf((char*)wifi_config.ap.password, 63, "%s", AP_PASSWORD);
    esp_wifi_set_config(WIFI_IF_AP, &wifi_config);

    /* 配置 AP 静态 IP：192.168.100.1/24 */
    esp_netif_ip_info_t ipInfo;
    IP4_ADDR(&ipInfo.ip, 192,168,100,1);
    IP4_ADDR(&ipInfo.gw, 192,168,100,1);
    IP4_ADDR(&ipInfo.netmask, 255,255,255,0);
    esp_netif_dhcps_stop(esp_netif_ap);
    esp_netif_set_ip_info(esp_netif_ap, &ipInfo);
    esp_netif_dhcps_start(esp_netif_ap);

    /* 启动 WiFi */
    esp_wifi_start();
    return ESP_OK;
}

/**
 * @brief 连接指定的 WiFi 热点
 *
 * 连接流程：
 *   1. 验证 SSID/密码长度
 *   2. 重置重连计数
 *   3. 填充 WiFi 配置结构体
 *   4. 如果当前模式不是 STA，则先停止 WiFi 再切换到 STA 模式
 *   5. 发起连接
 *
 * @param ssid     WiFi 热点名称（最长 31 字节）
 * @param password WiFi 密码（最长 63 字节）
 * @return ESP_OK 连接已发起
 *         ESP_FAIL 参数无效
 */
esp_err_t WifiManager_WifiConnect(const char* ssid, const char* password)
{
    /* 验证参数长度 */
    if (strlen(ssid) > 31 || strlen(password) > 63) return ESP_FAIL;

    retry_count = 0;

    /* 填充配置 */
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));

    /* 断开当前连接 */
    esp_wifi_disconnect();

    /* 检查当前模式，非 STA 模式需要切换 */
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode != WIFI_MODE_STA)
    {
        esp_wifi_stop();
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        esp_wifi_start();
        esp_wifi_connect();
    }
    else
    {
        esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        esp_wifi_connect();
    }
    return ESP_OK;
}

/**
 * @brief 检查当前是否已连接 WiFi
 *
 * @return true 已连接
 *         false 未连接
 */
bool WifiManager_IsConnected(void)
{
    return is_sta_connected;
}

/* ======================== WebSocket 数据处理 ===================================== */

/**
 * @brief WebSocket 接收数据处理
 *
 * 解析配网页面发送的 JSON 数据，支持两种消息类型：
 *   - { "scan": "start" }：触发 WiFi 扫描
 *   - { "ssid": "...", "password": "..." }：接收 WiFi 凭据
 *
 * 收到凭据后的处理流程：
 *   1. 保存凭据到 NVS
 *   2. 设置配网完成标志（NVS_PROV_DONE = 1）
 *   3. 标记 provision_done = true
 *   4. 通知配网任务（apcfg_event BIT0）
 *
 * @param payload WebSocket 接收到的数据
 * @param len     数据长度
 */
static void ws_receive_handle(uint8_t* payload, int len)
{
    cJSON* root = cJSON_Parse((char*)payload);
    if (!root)
    {
        ESP_LOGE(TAG, "Invalid JSON received");
        return;
    }

    /* 处理扫描请求 */
    cJSON* scan_js = cJSON_GetObjectItem(root, "scan");
    if (scan_js && strcmp(cJSON_GetStringValue(scan_js), "start") == 0)
    {
        WifiManager_WifiScan(wifi_scan_finish_handle);
    }

    /* 处理 WiFi 凭据 */
    cJSON* ssid_js = cJSON_GetObjectItem(root, "ssid");
    cJSON* pass_js = cJSON_GetObjectItem(root, "password");
    if (ssid_js && pass_js)
    {
        strncpy(current_ssid, ssid_js->valuestring, sizeof(current_ssid)-1);
        strncpy(current_password, pass_js->valuestring, sizeof(current_password)-1);
        ESP_LOGI(TAG, "Received WiFi credentials: SSID=%s", current_ssid);

        /* 保存到 NVS */
        NVS_Save_Wifi_Credentials(current_ssid, current_password);

        /* 设置配网完成标志 */
        nvs_handle_t handle;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK)
        {
            nvs_set_u8(handle, NVS_PROV_DONE, 1);
            nvs_commit(handle);
            nvs_close(handle);
        }

        provision_done = true;
        if (apcfg_event)
        {
            xEventGroupSetBits(apcfg_event, BIT0);
        }
    }
    cJSON_Delete(root);
}

/* ======================== AP 配网任务 ============================================ */

/**
 * @brief AP 配网任务函数
 *
 * 等待配网完成事件（apcfg_event BIT0），收到后：
 *   1. 停止 WebSocket 服务器
 *   2. 使用收到的凭据连接 WiFi
 *   3. 删除自身任务
 *
 * 此任务在 ap_wifi_init() 中创建，在配网完成或设备重启前持续运行。
 *
 * @param param 未使用
 */
static void ap_wifi_task(void* param)
{
    /* 阻塞等待配网完成（用户通过网页提交凭据） */
    EventBits_t ev = xEventGroupWaitBits(apcfg_event, BIT0, pdTRUE, pdFALSE, portMAX_DELAY);
    if (ev & BIT0)
    {
        /* 配网完成，关闭 WebSocket 服务器并连接目标 WiFi */
        web_ws_stop();
        WifiManager_WifiConnect(current_ssid, current_password);
        vTaskDelete(NULL);
    }
}

/**
 * @brief 初始化 AP 配网模式
 *
 * 初始化流程：
 *   1. 保存状态回调函数指针
 *   2. 加载配网 HTML 页面（从 SPIFFS）
 *   3. 创建配网事件组
 *   4. 注册 WiFi 和 IP 事件回调
 *   5. 创建配网任务（等待用户提交凭据）
 *
 * @param f WiFi 状态变化回调函数（可为 NULL）
 */
void ap_wifi_init(p_wifi_state_callback f)
{
    wifi_state_cb = f;
    index_html = initi_web_page_buffer();
    apcfg_event = xEventGroupCreate();

    /* 注册事件回调 */
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &ap_wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ap_wifi_event_handler, NULL, NULL);

    /* 创建配网任务（在 Core 1 上运行，避免与 Core 0 上的 WiFi 任务冲突） */
    xTaskCreatePinnedToCore(ap_wifi_task, "apcfg", 4096, NULL, 2, &AP_Task_Handle, 1);
}

/**
 * @brief 启动 AP 配网模式（外部调用入口）
 *
 * 调用 WifiManager_AP() 创建热点，然后启动 WebSocket 服务器。
 * 配网页面通过 WebSocket 接收用户输入的 WiFi 凭据。
 *
 * @param enable 固定传入 true（保留参数）
 */
void WifiManager_ap_wifi_apcfg(bool enable)
{
    if (!enable) return;

    ESP_LOGW(TAG, "启动 AP 配网模式");

    /* 创建 AP 热点 */
    WifiManager_AP();

    /* 启动 WebSocket 服务器 */
    ws_cfg_t ws = {
        .html_code = index_html,
        .receive_fn = ws_receive_handle,
    };
    web_ws_start(&ws);
}

/* ======================== WiFi 初始化（主入口） ================================== */

/**
 * @brief 初始化 WiFi（主入口函数）
 *
 * 这是 WiFi 模块的对外主入口，应用层调用此函数即可完成 WiFi 初始化。
 *
 * 初始化流程：
 *   1. 创建 WiFi 事件组（用于等待连接结果）
 *   2. 初始化 TCP/IP 协议栈和网络接口（STA + AP）
 *   3. 初始化 WiFi 驱动
 *   4. 注册 IP 事件回调
 *   5. 检查 NVS 凭据：
 *      - 有凭据：尝试连接 WiFi，等待获取 IP（超时 120 秒）
 *        - 连接成功：返回 ESP_OK
 *        - 连接超时：返回 ESP_FAIL
 *      - 无凭据或连接失败：启动 AP 配网模式，返回 ESP_OK
 *
 * 返回值说明：
 *   - ESP_OK（0）：连接成功 或 进入配网模式
 *   - ESP_FAIL（1）：连接失败（凭据可能已失效）
 *
 * @return ESP_OK 连接成功或进入配网模式
 *         ESP_FAIL 连接失败
 */
esp_err_t WifiManager_Wifi_Init(void)
{
    /* 步骤 1：创建事件组 */
    if (wifi_event_group == NULL)
    {
        wifi_event_group = xEventGroupCreate();
        if (wifi_event_group == NULL)
        {
            ESP_LOGE(TAG, "创建 WiFi 事件组失败");
            return ESP_FAIL;
        }
    }

    /* 步骤 2：初始化 TCP/IP 协议栈和网络接口 */
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_sta = esp_netif_create_default_wifi_sta();
    esp_netif_ap = esp_netif_create_default_wifi_ap();

    /* 步骤 3：初始化 WiFi 驱动 */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    /* 步骤 4：注册 IP 事件回调 */
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);

    /* 步骤 5：检查 NVS 凭据 */
    if (NVS_Has_Wifi_Credentials())
    {
        char ssid[CRED_SSID_MAX_LEN] = {0};
        char pass[CRED_PASS_MAX_LEN] = {0};

        if (NVS_Load_Wifi_Credentials(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK)
        {
            ESP_LOGW(TAG, "使用 NVS 凭据连接 WiFi: %s", ssid);

            /* 注册 WiFi 事件回调（用于重连） */
            esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);

            /* 配置并连接 WiFi */
            wifi_config_t wifi_config = {0};
            strlcpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
            strlcpy((char*)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));
            wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

            esp_wifi_set_mode(WIFI_MODE_STA);
            esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
            esp_wifi_start();
            esp_wifi_connect();

            /* 等待连接结果 */
            ESP_LOGW(TAG, "等待 WiFi 连接（最长 %d 秒）...",
                     WAIT_NVSWIFIMESSAGE_TOCONNECTED_TIMEOUT / 1000);
            EventBits_t bits = xEventGroupWaitBits(wifi_event_group, BIT0, pdFALSE, pdFALSE,
                                                    pdMS_TO_TICKS(WAIT_NVSWIFIMESSAGE_TOCONNECTED_TIMEOUT));
            if (bits & BIT0)
            {
                ESP_LOGW(TAG, "WiFi 连接成功（NVS 凭据）");
                esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
                return ESP_OK;
            }
            else
            {
                ESP_LOGE(TAG, "WiFi 连接超时（NVS 凭据失效）");
                return ESP_FAIL;
            }
        }
    }

    /* 无有效凭据或连接失败，启动 AP 配网 */
    ESP_LOGW(TAG, "无有效 NVS 凭据，启动 AP 配网");
    esp_wifi_stop();
    ap_wifi_init(NULL);
    WifiManager_ap_wifi_apcfg(true);
    return ESP_OK;
}