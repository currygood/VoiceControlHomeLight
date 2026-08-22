#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "wifi_manager.h"
#include "nvs_flash.h"
#include "LED_Control.h"
#include "Audio_Stream.h"
#include "AI_Coud.h"
#include "ASR.h"
#include "OLED.h"
#include "i2c_driver.h"
#include "amplifier.h"
#include "OTA_Update.h"
#include "BemFa.h"
#include "HardKeyControlLight.h"

#define TAG "Main"

#define AI_CLOUD_URL "wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue"
#define AI_CLOUD_TOKEN "you-token"

#define AUDIO_STREAM_BUF_SIZE 32000

static i2c_master_bus_handle_t i2c_bus_handle = NULL;

/**
 * @brief 颜色名称枚举
 * 
 * 用于表示不同的颜色名称
 */
typedef enum
{
    COLOR_RED,
    COLOR_GREEN,
    COLOR_BLUE,
    COLOR_YELLOW,
    COLOR_CYAN,
    COLOR_MAGENTA,
    COLOR_WHITE,
    COLOR_BLACK,
    COLOR_ORANGE,
    COLOR_PURPLE,
    COLOR_PINK,
    COLOR_LIME,
    COLOR_TEAL,
    COLOR_NAVY,
    COLOR_MAROON,
    COLOR_OLIVE,
    COLOR_GRAY,
    COLOR_SILVER,
    COLOR_BROWN,
    COLOR_GOLD,
    COLOR_COUNT
} Color_Name;

/**
 * @brief 颜色表条目结构体
 * 
 * 用于存储颜色名称、字符串表示和 RGB 值
 */
typedef struct
{
    Color_Name name;
    const char *str;
    uint8_t    r;
    uint8_t    g;
    uint8_t    b;
} Color_Entry;

/**
 * @brief 颜色表
 * 
 * 用于存储所有颜色名称、字符串表示和 RGB 值
 */
static const Color_Entry Color_Table[COLOR_COUNT] =
{
    {COLOR_RED,     "red",     255,   0,   0},
    {COLOR_GREEN,   "green",     0, 255,   0},
    {COLOR_BLUE,    "blue",      0,   0, 255},
    {COLOR_YELLOW,  "yellow",  255, 255,   0},
    {COLOR_CYAN,    "cyan",      0, 255, 255},
    {COLOR_MAGENTA, "magenta", 255,   0, 255},
    {COLOR_WHITE,   "white",   255, 255, 255},
    {COLOR_BLACK,   "black",     0,   0,   0},
    {COLOR_ORANGE,  "orange",  255, 165,   0},
    {COLOR_PURPLE,  "purple",  128,   0, 128},
    {COLOR_PINK,    "pink",    255, 192, 203},
    {COLOR_LIME,    "lime",     50, 205,  50},
    {COLOR_TEAL,    "teal",      0, 128, 128},
    {COLOR_NAVY,    "navy",      0,   0, 128},
    {COLOR_MAROON,  "maroon",  128,   0,   0},
    {COLOR_OLIVE,   "olive",   128, 128,   0},
    {COLOR_GRAY,    "gray",    128, 128, 128},
    {COLOR_SILVER,  "silver",  192, 192, 192},
    {COLOR_BROWN,   "brown",   139,  69,  19},
    {COLOR_GOLD,    "gold",    255, 215,   0},
};

/**
 * @brief 从 RGB 值获取颜色名称
 * 
 * @param r 红色分量
 * @param g 绿色分量
 * @param b 蓝色分量
 * @return const char* 颜色名称字符串
 */
static const char *Color_Name_From_RGB(uint8_t r, uint8_t g, uint8_t b)
{
    int32_t minDist = 0x7FFFFFFF;
    int bestIdx = 0;

    for(int idx = 0; idx < COLOR_COUNT; idx++)
    {
        int32_t dr = (int32_t)r - (int32_t)Color_Table[idx].r;
        int32_t dg = (int32_t)g - (int32_t)Color_Table[idx].g;
        int32_t db = (int32_t)b - (int32_t)Color_Table[idx].b;
        int32_t dist = dr * dr + dg * dg + db * db;

        if(dist < minDist)
        {
            minDist = dist;
            bestIdx = idx;
        }
    }

    return Color_Table[bestIdx].str;
}

/**
 * @brief 检查 LED 状态是否有差异
 * 
 * @param last 上一次的 LED 状态
 * @param current 当前的 LED 状态
 * @return true 如果有差异，否则 false
 * @note 用于判断是否需要更新 OLED 显示
 * @param current 当前的 LED 状态结构
 * @return true 如果状态有差异，否则 false
 */
static bool IsDiffertFun(LED_Control_State *last,LED_Control_State *current)
{
	bool isDiff = false;
	if(last->is_on != current->is_on)
	{
		isDiff = true;
	}
	if(last->brightness != current->brightness)
	{
		isDiff = true;
	}
	if(last->color_r != current->color_r)
	{
		isDiff = true;
	}
	if(last->color_g != current->color_g)
	{
		isDiff = true;
	}
	if(last->color_b != current->color_b)
	{
		isDiff = true;
	}
	return isDiff;
}

/**
 * @brief 复制 LED 状态
 * 
 * @param last 上一次的 LED 状态
 * @param current 当前的 LED 状态
 * @note 用于更新 OLED 显示 的判断
 */
static void CopyLEDState(LED_Control_State *last,LED_Control_State *current)
{
	last->is_on = current->is_on;
	last->brightness = current->brightness;
	last->color_r = current->color_r;
	last->color_g = current->color_g;
	last->color_b = current->color_b;
}

// OLED显示任务
void Task_OLED_Show(void *pvParameters)
{
	static bool isToUpdate = false;	// 是否需要更新 OLED 显示
	static bool isFirstGetLEDState = true;	// 是否是第一次获取 LED 状态
	static LED_Control_State ledS1_Last,ledS2_Last;	// 上一次的 LED 状态
	static LED_Control_State ledS1_Current,ledS2_Current;	// 当前的 LED 状态
	static LED_ID led_1,led_2;	// LED 灯标识
	static char tempStr[128];	// 临时字符串缓冲区
	static esp_err_t err;		// 错误码

	led_1.id = LED_ID_BEDROOM;
	led_2.id = LED_ID_LIVINGROOM;
	led_1.name = "Bedroom";
	led_2.name = "Livingroom";

	// 两个不变的直接写到这里
	OLED_ShowString(0,0,led_1.name,OLED_8X16);
	OLED_ShowString(0,32,led_2.name,OLED_8X16);

	while(1)
	{
		// 获取当前的LED灯状态
		err = LED_Control_Get_Light(led_1,&ledS1_Current);
		if(err != ESP_OK)
		{
			ESP_LOGE(TAG, "LED Control get light failed: %s", esp_err_to_name(err));
		}

		err = LED_Control_Get_Light(led_2,&ledS2_Current);
		if(err != ESP_OK)
		{
			ESP_LOGE(TAG, "LED Control get light failed: %s", esp_err_to_name(err));
		}

		// 第一次获取LED灯状态 直接更新OLED
		if(isFirstGetLEDState)
		{
			isFirstGetLEDState = false;
			isToUpdate=true;
		}
		else	// 非第一次获取LED灯状态 检查是否有差异
		{
			if(IsDiffertFun(&ledS1_Last,&ledS1_Current))
			{
				isToUpdate=true;
			}
			if(IsDiffertFun(&ledS2_Last,&ledS2_Current))
			{
				isToUpdate=true;
			}
		}

		if(isToUpdate)	// 如果有差异 则更新OLED 并且把当前状态复制给上一次状态，继续下一轮循环
		{
			isToUpdate = false;

			OLED_ClearArea(0,16,128,8);
			OLED_ClearArea(0,48,128,8);
			// 更新LED1的状态
			snprintf(tempStr,sizeof(tempStr),"%d%%,%s,%s",ledS1_Current.brightness,ledS1_Current.is_on?"ON":"OFF",Color_Name_From_RGB(ledS1_Current.color_r,ledS1_Current.color_g,ledS1_Current.color_b));
			OLED_ShowString(0,16,tempStr,OLED_6X8);

			// 更新LED2的状态
			
			snprintf(tempStr,sizeof(tempStr),"%d%%,%s,%s",ledS2_Current.brightness,ledS2_Current.is_on?"ON":"OFF",Color_Name_From_RGB(ledS2_Current.color_r,ledS2_Current.color_g,ledS2_Current.color_b));
			OLED_ShowString(0,48,tempStr,OLED_6X8);

			// 更新OLED显示
			OLED_Update();
			CopyLEDState(&ledS1_Last,&ledS1_Current);
			CopyLEDState(&ledS2_Last,&ledS2_Current);
		}

		vTaskDelay(pdMS_TO_TICKS(1000));		// 延时1s
	}
}

void app_main(void)
{
	// 初始化 NVS wifi需要使用
	esp_err_t err = nvs_flash_init();
	if (err != ESP_OK)
	{
		while(1)
		{
			ESP_LOGE(TAG, "NVS flash init failed: %s", esp_err_to_name(err));
			vTaskDelay(pdMS_TO_TICKS(1000));
		}
	}

	// 初始化 OTA_Update 模块（仅读取版本号与配置，不访问网络）
	ota_update_config_t ota_config = {0};
	bool ota_ready = false;
	err = OTA_Update_Init(&ota_config);
	if(err != ESP_OK)
	{
		ESP_LOGE(TAG, "OTA_Update init failed: %s", esp_err_to_name(err));
	}
	else
	{
		ota_ready = true;
		ESP_LOGI("+++++Version+++++", "Local firmware version: %s", OTA_Update_GetLocalVersion());
	}

	// 初始化并且连接wifi（无凭据时会进入 AP 配网模式）
	WifiManager_Wifi_Init();

	// AP 配网完成前 OTA 不能开始，阻塞等待 WiFi 真正连接成功
	while(!WifiManager_IsConnected())
	{
		ESP_LOGI(TAG, "Waiting for WiFi connection before OTA...");
		vTaskDelay(pdMS_TO_TICKS(2000));
	}

	// 新固件首次启动且 WiFi 正常后，确认当前固件有效，取消自动回滚
	err = OTA_Update_MarkAppValid();
	if(err != ESP_OK)
	{
		ESP_LOGW(TAG, "OTA mark app valid failed: %s", esp_err_to_name(err));
	}

	if(ota_ready)
	{
		// 同步检查升级：有新版本会下载、校验并重启；无新版本返回 ESP_OK 后继续初始化
		err = OTA_Update_CheckAndUpgrade();
		if(err != ESP_OK)
		{
			ESP_LOGW(TAG, "OTA check failed, continue boot: %s", esp_err_to_name(err));
		}
	}


	// 初始化 LED 控制模块
	err = LED_Control_Init();
	if(err != ESP_OK)
	{
		ESP_LOGE(TAG, "LED Control init failed: %s", esp_err_to_name(err));
	}

	// 初始化按键控制灯模块
	HardKeyControlLight_Init();

	// 初始化I2C总线 然后初始化OLED
	err = I2c_Init_Bus(I2C_NUM_0,I2C_SDA_GPIO,I2C_SCL_GPIO,I2C_FREQ,&i2c_bus_handle);
	if(err != ESP_OK)
	{
		ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(err));
	}
	err = OLED_Init(i2c_bus_handle);
	if(err != ESP_OK)
	{
		ESP_LOGE(TAG, "OLED init failed: %s", esp_err_to_name(err));
	}

	// 开始按键控制灯模块
	HardKeyControlLight_Start();

	// 初始化音频流模块 里面有初始化麦克风
	AudioStream_Init(AUDIO_STREAM_BUF_SIZE);

	// 初始化功放
	Amplifier_Init();

	// 初始化AI_Cloud AiCloud 任务启动后会注册为 AudioStream 消费者并等待数据。
	AiCloud_Init(AI_CLOUD_URL, AI_CLOUD_TOKEN);

	// 初始化唤醒模块
	ASR_Init();

	// 启动音频流的生产者任务
	AudioStream_Start();

	// 启动ASR唤醒任务
	ASR_Start();

	// 等待上面的初始化完成
	vTaskDelay(pdMS_TO_TICKS(2000));
	// 启动OLED显示任务
	xTaskCreate(Task_OLED_Show,"OLED_Show",4096,NULL,4,NULL);

	// 启动巴法云任务
	xTaskCreate(BemFaMQTT_Task,"BemFaMQTT_Task",8192,NULL,6,NULL);

	while(1)
	{
		vTaskDelay(pdMS_TO_TICKS(1000));
	}

	vTaskDelete(NULL);
}