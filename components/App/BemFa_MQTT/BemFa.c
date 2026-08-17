/**
 * @file BemFa.c
 * @brief 巴法云 MQTT 通信模块
 *
 * 本模块实现 ESP32 与巴法云（bemfa.com）的 MQTT 通信，
 * 用于接入米家 App / 小爱同学控制智能家居灯光设备。
 *
 * 主要功能：
 *   - 连接巴法云 MQTT 服务器（端口 9501）
 *   - 订阅灯控主题，接收米家指令
 *   - 解析指令并调用 LED_Control 模块控制灯光
 *   - 定时上报设备状态到巴法云，同步米家 App 显示
 *
 * 支持的消息格式：
 *   on                    开灯（使用默认亮度）
 *   off                   关灯
 *   on#亮度值              开灯并设置亮度（范围 1~100），如 on#80
 *   on#亮度值#RGB颜色值     开灯 + 亮度 + RGB 颜色
 *
 * ⚠️ 颜色格式说明（重要）：
 *   米家 App 发送的 RGB 值为**十进制整数**，需通过位运算拆分：
 *     - 示例：on#30#16711680 （16711680 = 0xFF0000 = 纯红色）
 *     - 解析：R = (color >> 16) & 0xFF
 *             G = (color >> 8)  & 0xFF
 *             B =  color        & 0xFF
 *
 * 使用方式：
 *   1. 调用 Bemfa_MQTT_Init() 初始化 MQTT 客户端
 *   2. 创建 FreeRTOS 任务运行 BemFaMQTT_Task()
 *   3. 任务会自动处理连接、订阅、消息接收和状态上报
 *
 * @note 需要先连接 Wi-Fi 才能连接巴法云
 * @see BemFa.h - 公共 API 接口声明
 * @see LED_Control.h - 灯光控制模块
 */
#include "BemFa.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "LED_Control.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_manager.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#define TAG "BemFaMQTT"

/* ======================== 巴法云配置 =========================================== */

#define BEMFA_MQTT_CLIENT_ID 					"463dcbeda62b00a215786d00a8df66b1"
#define BEMFA_MQTT_BROKER_URL 					"mqtt://bemfa.com:9501"


#define BEMFA_MQTT_TOPIC_BEDROOMLIGHT 			"bedroom002"
#define BEMFA_MQTT_TOPIC_LIVINGROOMLIGHT 		"livingroom002"

#define BEMFA_MQTT_REPORT_INTERVAL_MS 30000

static esp_mqtt_client_handle_t bemfa_mqtt_client = NULL;


static bool s_mqtt_connected = false;

/* ======================== 内部函数 =========================================== */

/**
 * @brief 根据巴法云主题获取 LED ID
 * 
 * @param topic 巴法云主题
 * @return LED_ID LED ID 结构体
 */
static LED_ID bemfaMQTT_Get_LEDID(const char *topic)
{
	LED_ID led = { .id = 0, .name = "unknown" };

    if (strcmp(topic, BEMFA_MQTT_TOPIC_BEDROOMLIGHT) == 0) {
        led.id   = LED_ID_BEDROOM;
        led.name = "Bedroom";
    } else if (strcmp(topic, BEMFA_MQTT_TOPIC_LIVINGROOMLIGHT) == 0) {
        led.id   = LED_ID_LIVINGROOM;
        led.name = "Livingroom";
    }
    return led;
}

/**
 * @brief 解析巴法云灯指令并控制对应灯
 *
 * 支持格式：
 *   on / off
 *   on#亮度(1~100)
 *   on#亮度#RGB十进制整数（如 16711680 = 红色）
 */
static void bemfaMQTT_Handle_Message(const char *topic, const char *data)
{
    LED_ID led = bemfaMQTT_Get_LEDID(topic);
    if (led.id != LED_ID_BEDROOM && led.id != LED_ID_LIVINGROOM) {
        ESP_LOGW(TAG, "未知主题: %s", topic);
        return;
    }

    LED_Control_State st;
    if (LED_Control_Get_Light(led, &st) != ESP_OK) {
        return;
    }

    if (strcmp(data, "on") == 0) {
        st.is_on = true;
    } else if (strcmp(data, "off") == 0) {
        st.is_on = false;
    } else {
        int  brightness = 0;
        unsigned int color_val = 0;

        if (sscanf(data, "on#%d#%u", &brightness, &color_val) == 2) {
            st.is_on      = true;
            st.brightness = (uint8_t)brightness;
            st.color_r    = (uint8_t)((color_val >> 16) & 0xFF);
            st.color_g    = (uint8_t)((color_val >> 8) & 0xFF);
            st.color_b    = (uint8_t)(color_val & 0xFF);
        } else if (sscanf(data, "on#%d", &brightness) == 1) {
            st.is_on      = true;
            st.brightness = (uint8_t)brightness;
        } else {
            ESP_LOGW(TAG, "无法解析的指令: %s", data);
            return;
        }
    }

    if (LED_Control_Set_Light(led, st) == ESP_OK) {
        ESP_LOGI(TAG, "收到米家指令: topic=%s data=%s", topic, data);
        /* 不在此处立即上报，避免消息回环 */
        /* 状态同步由定时任务(30秒)和连接时触发 */
    }
}

/**
 * @brief 订阅巴法云主题
 * 
 * @param topics 巴法云主题数组
 */
static void bemfaMQTT_Subscribe(const char *topics[])
{
    for (int i = 0; i < LED_ID_MAX; i++) {
        esp_mqtt_client_subscribe(bemfa_mqtt_client, topics[i], 1);
    }
}

/**
 * @brief 处理巴法云 MQTT 回调事件
 * 
 * @param handler_args 事件处理函数的参数
 * @param base 事件基底
 * @param event_id 事件 ID
 * @param event_data 事件数据
 */
static void bemfaMQTT_mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "已连接巴法云");
        s_mqtt_connected = true;
        static const char *topics[LED_ID_MAX] = { BEMFA_MQTT_TOPIC_BEDROOMLIGHT, BEMFA_MQTT_TOPIC_LIVINGROOMLIGHT };
        bemfaMQTT_Subscribe(topics);
        BemfaMQTT_Report_State();   /* 上线即上报一次真实状态 */
        break;

    case MQTT_EVENT_DATA: {
        char topic[64] = {0};
        char data[64]  = {0};
        int  tlen = (event->topic_len < (int)sizeof(topic) - 1) ? event->topic_len : (int)sizeof(topic) - 1;
        int  dlen = (event->data_len  < (int)sizeof(data)  - 1) ? event->data_len  : (int)sizeof(data)  - 1;
        memcpy(topic, event->topic, tlen);
        memcpy(data,  event->data,  dlen);
        bemfaMQTT_Handle_Message(topic, data);
        break;
    }

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "与巴法云断开，esp-mqtt 将自动重连");
        break;

    default:
        break;
    }
}

/* ======================== 供外部使用的api函数 =========================================== */

/**
 * @brief 把当前两路灯的状态上报到巴法云（米家 App 同步显示）
 * @return ESP_OK 成功
 */
esp_err_t BemfaMQTT_Report_State(void)
{
    if (bemfa_mqtt_client == NULL || !s_mqtt_connected) {
        return ESP_FAIL;
    }

    static const char *topics[LED_ID_MAX] = { BEMFA_MQTT_TOPIC_BEDROOMLIGHT, BEMFA_MQTT_TOPIC_LIVINGROOMLIGHT };

    for (int i = 0; i < LED_ID_MAX; i++) {
        LED_Control_State st;
        LED_ID led = { .id = i, .name = "led" };

        if (LED_Control_Get_Light(led, &st) != ESP_OK) {
            continue;
        }

        char payload[32];
        if (st.is_on) {
            snprintf(payload, sizeof(payload), "on#%d#%02x%02x%02x",
                     st.brightness, st.color_r, st.color_g, st.color_b);
        } else {
            snprintf(payload, sizeof(payload), "off");
        }

        esp_mqtt_client_publish(bemfa_mqtt_client, topics[i], payload, 0, 1, 0); /* QoS1, 不使用retain避免循环 */
    }
    return ESP_OK;
}

/**
 * @brief 初始化巴emFa MQTT
 * 
 * @return ESP_OK 成功
 * @return ESP_FAIL 失败
 */
esp_err_t Bemfa_MQTT_Init(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = BEMFA_MQTT_BROKER_URL,
        .credentials.client_id = BEMFA_MQTT_CLIENT_ID,
        .session.keepalive = 60,
    };

    bemfa_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (bemfa_mqtt_client == NULL) {
        ESP_LOGE(TAG, "MQTT 客户端创建失败");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(bemfa_mqtt_client, ESP_EVENT_ANY_ID, bemfaMQTT_mqtt_event_handler, NULL);
    return esp_mqtt_client_start(bemfa_mqtt_client);
}


/**
 * @brief 巴法云任务
 * 
 * @param pvParameters 任务参数
 */
void BemFaMQTT_Task(void *pvParameters)
{
	(void)pvParameters;
	
	// 等待wifi连接成功，不成功则阻塞，因为不连接wifi无法连接巴法云
	while(!WifiManager_IsConnected());
	
	Bemfa_MQTT_Init();

	while(1) {
		BemfaMQTT_Report_State();
		vTaskDelay(pdMS_TO_TICKS(BEMFA_MQTT_REPORT_INTERVAL_MS));	// 30秒上报一次状态，因为可能有状态变化
	}

}