#ifndef __BEMFA_H__
#define __BEMFA_H__

#include "esp_err.h"

/**
 * @brief 初始化BemFa MQTT
 * 
 * @return esp_err_t 
 */
esp_err_t BemfaMQTT_client_init(void);

/**
 * @brief 把当前两路灯的状态上报到巴法云（米家 App 同步显示）
 * @return ESP_OK 成功
 */
esp_err_t BemfaMQTT_Report_State(void);

/**
 * @brief 巴法云任务
 * 
 * @param pvParameters 任务参数
 */
void BemFaMQTT_Task(void *pvParameters);

#endif
