/**
 * @file AI_Coud.c
 * @brief AI Cloud 云端语音对话模块实现
 *
 * 本模块基于 WebSocket 协议与豆包（ByteDance）云端大模型进行实时语音对话。
 * 核心流程：
 *   1. 初始化阶段：配置 WebSocket 连接参数 → 创建音频上传任务
 *   2. 连接阶段：建立 WebSocket 连接 → 发送 session.create 配置 AI 角色与工具
 *   3. 对话阶段：持续上传 PCM 音频 → 接收并处理云端事件（文本/音频/函数调用）
 *   4. 断连阶段：清理部分缓冲区 → 通知播放模块结束
 *
 * 豆包实时 API 事件类型：
 *   - session.created / session.updated：会话创建/更新确认
 *   - response.output_text.delta：AI 文本回复（流式）
 *   - response.output_audio.delta：AI 音频回复（OGG_OPUS，Base64 编码）
 *   - response.function_call_arguments.done：函数调用请求（控制灯光）
 *   - response.done：响应结束
 *
 * 支持的 Function Call：
 *   - set_light：控制灯光开关、亮度、颜色
 *   - get_light：查询当前灯光状态
 *
 * 依赖模块：
 *   - Audio_Stream：音频环形缓冲区（上传 PCM 和播放 OGG）
 *   - LED_Control：灯光设备控制
 *   - cJSON：JSON 解析与构造
 *   - mbedtls：Base64 编解码
 */

#include "AI_Coud.h"
#include "esp_websocket_client.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include "Audio_Stream.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "LED_Control.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include "freertos/semphr.h"

/* ======================== 模块常量定义 =========================================== */

#define TAG "AI_CLOUD"                   /**< 日志标签 */

#define AI_CLOUD_READ_BUF_LEN 512        /**< 每次从环形缓冲区读取的 PCM 采样数 */

/* ======================== 模块静态变量 =========================================== */

/** WebSocket 客户端句柄，用于建立和管理与云端服务器的连接 */
static esp_websocket_client_handle_t ws_client = NULL;

/** API 认证令牌副本，初始化时由 strdup 分配 */
static char *g_api_key = NULL;

/** WebSocket 部分帧缓冲区：当 JSON 帧被 TCP 分片时，用于累积拼接 */
static char *ws_partial_buf = NULL;

/** 部分帧缓冲区当前有效数据长度 */
static size_t ws_partial_len = 0;

/** WebSocket 操作互斥锁，保护 send 操作的线程安全 */
static SemaphoreHandle_t ws_op_lock = NULL;

/* ======================== 前向声明 =============================================== */

/** 发送 session.create 事件，配置 AI 角色和可用工具 */
static void send_session_create_event(void);

/** AI Cloud 音频上传任务（由 AiCloud_Init 创建） */
void AiCloud_task(void *pvParameters);

/* ======================== WebSocket 事件处理 ===================================== */

/**
 * @brief 处理云端下发的 JSON 事件
 *
 * 根据事件类型分发处理逻辑，支持的豆包事件类型：
 *   - session.created / session.updated：会话确认，打印会话信息
 *   - response.output_text.delta：AI 文本回复，打印到日志
 *   - response.output_audio.started：音频流开始，通知播放模块创建解码器
 *   - response.output_audio.delta：音频数据块，Base64 解码后送入播放队列
 *   - response.output_audio.done：音频流结束，通知播放模块销毁解码器
 *   - response.function_call_arguments.done：函数调用请求，执行本地灯光控制
 *
 * @param data JSON 消息数据指针
 * @param len  消息数据长度（字节）
 */
static void handle_ws_incoming_data(const char *data, int len)
{
    ESP_LOGI(TAG, "WS data in: %d bytes", len);

    /* 解析 JSON 消息 */
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root)
    {
        ESP_LOGW(TAG, "JSON parse failed! raw=%.*s", len > 200 ? 200 : len, data);
        return;
    }

    /* 提取事件类型字段 */
    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!type)
    {
        char *raw = cJSON_PrintUnformatted(root);
        ESP_LOGW(TAG, "No type field in JSON: %s", raw ? raw : "null");
        if (raw) free(raw);
        cJSON_Delete(root);
        return;
    }

    if (type) {
        ESP_LOGI(TAG, "Server event: %s", type->valuestring);

        /*
         * 文本增量事件：AI 流式输出文本回复。
         * 从 delta 字段提取文本内容并打印到日志。
         */
        if (strcmp(type->valuestring, "response.output_text.delta") == 0) {
            cJSON *delta = cJSON_GetObjectItem(root, "delta");
            if (delta && delta->valuestring) {
                ESP_LOGI(TAG, "AI text: %s", delta->valuestring);
            }
        }

        /*
         * 会话确认事件：云端确认会话创建或更新成功。
         * 打印完整的会话配置信息，便于调试。
         */
        if (strcmp(type->valuestring, "session.created") == 0 ||
            strcmp(type->valuestring, "session.updated") == 0) {
            char *json_str = cJSON_PrintUnformatted(root);
            if (json_str) {
                ESP_LOGI(TAG, "Session info: %s", json_str);
                free(json_str);
            }
        }

        /*
         * 音频边界事件：打印音频流开始/结束及响应完成的详细信息。
         */
        if (strcmp(type->valuestring, "response.output_audio.started") == 0 ||
            strcmp(type->valuestring, "response.output_audio.done") == 0 ||
            strcmp(type->valuestring, "response.done") == 0) {
            char *json_str = cJSON_PrintUnformatted(root);
            if (json_str) {
                ESP_LOGI(TAG, "Audio/response detail: %s", json_str);
                free(json_str);
            }
        }

        /*
         * 音频流开始：通知播放模块创建 OGG_OPUS 解码器。
         * 后续的 audio.delta 数据块将通过解码器播放。
         */
        if (strcmp(type->valuestring, "response.output_audio.started") == 0) {
            AudioStream_Playback_Write(AUDIO_PLAYBACK_START, NULL, 0);
        }

        /*
         * 音频增量事件：接收云端 AI 语音回复的 OGG_OPUS 数据块。
         * 数据经 Base64 解码后送入播放队列，由播放任务异步解码播放。
         */
        bool is_audio_delta = (strcmp(type->valuestring, "response.output_audio.delta") == 0 ||
                               strcmp(type->valuestring, "response.audio.delta") == 0);
        if (is_audio_delta) {
            cJSON *delta = cJSON_GetObjectItem(root, "delta");
            if (delta && delta->valuestring) {
                size_t b64_len = strlen(delta->valuestring);
                unsigned char *ogg_data = malloc(b64_len);
                if (ogg_data) {
                    size_t out_len = 0;
                    int decode_ret = mbedtls_base64_decode(ogg_data, b64_len, &out_len,
                                                            (unsigned char *)delta->valuestring, b64_len);
                    if (out_len > 0 && decode_ret == 0) {
                        /* Base64 解码成功，将 OGG 数据送入播放队列 */
                        AudioStream_Playback_Write(AUDIO_PLAYBACK_DATA, ogg_data, out_len);
                    }
                    else
                    {
                        ESP_LOGW(TAG, "base64 decode failed: out_len=%u, ret=%d", (unsigned)out_len, decode_ret);
                    }
                    free(ogg_data);
                }
            }
        }

        /*
         * 音频流结束：通知播放模块 AI 语音回复已完整接收。
         * 播放任务会先播完队列中剩余数据，再销毁解码器。
         */
        bool is_audio_done = (strcmp(type->valuestring, "response.output_audio.done") == 0 ||
                             strcmp(type->valuestring, "response.audio.done") == 0);
        if (is_audio_done) {
            AudioStream_Playback_Write(AUDIO_PLAYBACK_END, NULL, 0);
        }

        /*
         * 函数调用完成事件：云端 AI 请求调用本地函数来控制设备。
         * 当前支持的函数：
         *   - set_light：设置灯光状态（开关、亮度、颜色）
         *   - get_light：查询灯光当前状态
         *
         * 处理流程：
         *   1. 解析函数名、参数和 call_id
         *   2. 执行对应的本地函数，获取结果
         *   3. 构造 conversation.item.create 响应消息回传云端
         */
        if (strcmp(type->valuestring, "response.function_call_arguments.done") == 0)
        {
            /* 提取函数调用信息 */
            cJSON *func_items = cJSON_GetObjectItem(root, "items");
            if (!func_items || !cJSON_IsArray(func_items)) return;

            cJSON *func_item = cJSON_GetArrayItem(func_items, 0);
            if (!func_item) return;

            cJSON *call_id = cJSON_GetObjectItem(func_item, "call_id");
            cJSON *fun_name = cJSON_GetObjectItem(func_item, "name");
            cJSON *args_raw = cJSON_GetObjectItem(func_item, "arguments");

            if (!call_id || !fun_name || !args_raw) return;

            ESP_LOGI(TAG, "Function call: %s, id=%s", fun_name->valuestring, call_id->valuestring);
            ESP_LOGI(TAG, "  raw args: %s", args_raw->valuestring);

            bool ok = false;
            char output_buf[256] = {0};

            /* ========== set_light：设置灯光状态 ========== */
            if (strcmp(fun_name->valuestring, "set_light") == 0)
            {
                cJSON *args = cJSON_Parse(args_raw->valuestring);
                if (args)
                {
                    /* 提取房间位置参数 */
                    const char *loc = cJSON_GetObjectItem(args, "location")->valuestring;

                    /*
                     * 提取开关状态。
                     * 豆包模型可能将布尔值序列化为 JSON true/false 或字符串 "True"/"true"，
                     * 需要兼容两种格式。
                     */
                    cJSON *on_item = cJSON_GetObjectItem(args, "on");
                    bool on = false;
                    if (on_item && cJSON_IsTrue(on_item))
                    {
                        on = true;
                    }
                    else if (on_item && cJSON_IsString(on_item) && on_item->valuestring &&
                             strcasecmp(on_item->valuestring, "true") == 0)
                    {
                        on = true;
                    }

                    /* 提取亮度参数，默认 100（最亮） */
                    cJSON *bri_item = cJSON_GetObjectItem(args, "brightness");
                    int bri = bri_item ? bri_item->valueint : 100;

                    /* 根据房间名称确定 LED ID */
                    LED_ID led_id = { .id = (strcmp(loc, "bedroom") == 0) ? LED_ID_BEDROOM : LED_ID_LIVINGROOM,
                                      .name = loc };

                    /* 获取当前灯光状态，用于未指定参数时的默认值填充 */
                    LED_Control_State cur;
                    LED_Control_Get_Light(led_id, &cur);

                    /* 提取 RGB 颜色分量，未指定时沿用当前值 */
                    cJSON *r_item = cJSON_GetObjectItem(args, "color_r");
                    cJSON *g_item = cJSON_GetObjectItem(args, "color_g");
                    cJSON *b_item = cJSON_GetObjectItem(args, "color_b");

                    uint8_t r = (r_item && r_item->valueint >= 0 && r_item->valueint <= 255) ? (uint8_t)r_item->valueint : cur.color_r;
                    uint8_t g = (g_item && g_item->valueint >= 0 && g_item->valueint <= 255) ? (uint8_t)g_item->valueint : cur.color_g;
                    uint8_t b = (b_item && b_item->valueint >= 0 && b_item->valueint <= 255) ? (uint8_t)b_item->valueint : cur.color_b;

                    /* 构造灯光状态并执行设置 */
                    LED_Control_State state = {
                        .is_on      = on,
                        .brightness = (uint8_t)bri,
                        .color_r    = r,
                        .color_g    = g,
                        .color_b    = b,
                    };
                    ok = (LED_Control_Set_Light(led_id, state) == ESP_OK);
                    cJSON_Delete(args);

                    /* 读取设置后的实际状态，构造返回结果 */
                    LED_Control_Get_Light(led_id, &cur);
                    snprintf(output_buf, sizeof(output_buf),
                        "{\"status\":\"%s\",\"location\":\"%s\",\"on\":%s,\"brightness\":%d,\"color_r\":%d,\"color_g\":%d,\"color_b\":%d}",
                        ok ? "ok" : "error", loc, cur.is_on ? "true" : "false",
                        cur.brightness, cur.color_r, cur.color_g, cur.color_b);
                }
            }
            /* ========== get_light：查询灯光状态 ========== */
            else if (strcmp(fun_name->valuestring, "get_light") == 0)
            {
                cJSON *args = cJSON_Parse(args_raw->valuestring);
                if (args)
                {
                    /* 提取房间位置参数 */
                    const char *loc = cJSON_GetObjectItem(args, "location")->valuestring;

                    LED_ID led_id = { .id = (strcmp(loc, "bedroom") == 0) ? LED_ID_BEDROOM : LED_ID_LIVINGROOM,
                                      .name = loc };
                    LED_Control_State cur;
                    LED_Control_Get_Light(led_id, &cur);
                    snprintf(output_buf, sizeof(output_buf),
                        "{\"status\":\"ok\",\"location\":\"%s\",\"on\":%s,\"brightness\":%d,\"color_r\":%d,\"color_g\":%d,\"color_b\":%d}",
                        loc, cur.is_on ? "true" : "false",
                        cur.brightness, cur.color_r, cur.color_g, cur.color_b);

                    cJSON_Delete(args);
                }
            }

            /* ========== 构造 conversation.item.create 响应消息 ========== */
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddStringToObject(resp, "type", "conversation.item.create");

            cJSON *items = cJSON_CreateArray();
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "call_id", call_id->valuestring);
            cJSON_AddStringToObject(item, "role", "tool");

            cJSON *content = cJSON_CreateArray();
            cJSON *content_item = cJSON_CreateObject();
            cJSON_AddStringToObject(content_item, "type", "input_text");
            cJSON_AddStringToObject(content_item, "text", output_buf[0] ? output_buf : "{\"status\":\"error\"}");
            cJSON_AddItemToArray(content, content_item);

            cJSON_AddItemToObject(item, "content", content);
            cJSON_AddItemToArray(items, item);
            cJSON_AddItemToObject(resp, "items", items);

            /* 发送函数调用结果回云端 */
            char *json_str = cJSON_PrintUnformatted(resp);
            esp_websocket_client_send_text(ws_client, json_str, strlen(json_str), pdMS_TO_TICKS(1000));
            free(json_str);
            cJSON_Delete(resp);
        }
    }
    cJSON_Delete(root);
}

/* ======================== WebSocket 连接事件处理 ================================ */

/**
 * @brief WebSocket 事件回调处理函数
 *
 * 处理 WebSocket 连接生命周期中的各类事件：
 *   - WEBSOCKET_EVENT_CONNECTED：连接建立成功，发送 session.create 配置 AI
 *   - WEBSOCKET_EVENT_DATA：接收数据帧，处理 JSON 分片重组和完整帧分发
 *   - WEBSOCKET_EVENT_DISCONNECTED：连接断开，清理部分缓冲区并通知播放结束
 *
 * JSON 分片重组机制：
 *   豆包下发的 JSON 帧可能因 TCP 分片被拆分为多个 WebSocket 帧。
 *   本函数通过 ws_partial_buf 累积不完整的 JSON 数据，
 *   每次追加后尝试解析，直到 cJSON 解析成功才调用 handle_ws_incoming_data()。
 *
 * @param handler_args 事件处理参数（未使用）
 * @param base         事件基类
 * @param event_id     事件 ID
 * @param event_data   事件数据（esp_websocket_event_data_t 类型）
 */
static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WebSocket Connected");
            /* 连接成功后立即发送 session.create 配置 AI 角色和工具 */
            send_session_create_event();
            break;
        case WEBSOCKET_EVENT_DATA:
            ESP_LOGI(TAG, "WS frame op=0x%02x len=%d", data->op_code, data->data_len);
            if (data->op_code == 0x01)  /* 文本帧 */
            {
                /*
                 * 判断是否为续帧：如果已有部分缓冲且新帧不以 '{' 开头，
                 * 则认为是前一个 JSON 帧的后续部分。
                 */
                bool is_continuation = (data->data_len > 0 && data->data_ptr[0] != '{' && ws_partial_buf != NULL);
                if (is_continuation)
                {
                    /* 续帧：追加到部分缓冲区 */
                    size_t new_len = ws_partial_len + (size_t)data->data_len;
                    char *new_buf = realloc(ws_partial_buf, new_len + 1);
                    if (new_buf)
                    {
                        memcpy(new_buf + ws_partial_len, data->data_ptr, data->data_len);
                        new_buf[new_len] = '\0';
                        ws_partial_buf = new_buf;
                        ws_partial_len = new_len;

                        /* 尝试解析拼接后的完整 JSON */
                        cJSON *test = cJSON_ParseWithLength(ws_partial_buf, (int)ws_partial_len);
                        if (test)
                        {
                            cJSON_Delete(test);
                            ESP_LOGI(TAG, "WS reassembled: +%d -> %u bytes, parse OK",
                                     data->data_len, (unsigned)ws_partial_len);
                            handle_ws_incoming_data(ws_partial_buf, (int)ws_partial_len);
                            free(ws_partial_buf);
                            ws_partial_buf = NULL;
                            ws_partial_len = 0;
                        }
                        else
                        {
                            ESP_LOGI(TAG, "WS continuation +%d -> %u bytes, still incomplete",
                                     data->data_len, (unsigned)ws_partial_len);
                        }
                    }
                    else
                    {
                        ESP_LOGE(TAG, "WS realloc failed, dropping partial");
                        free(ws_partial_buf);
                        ws_partial_buf = NULL;
                        ws_partial_len = 0;
                    }
                }
                else
                {
                    /* 新帧：尝试直接解析，失败则缓存为部分帧 */
                    cJSON *test = cJSON_ParseWithLength(data->data_ptr, data->data_len);
                    if (test)
                    {
                        cJSON_Delete(test);
                        /* 丢弃之前残留的部分缓冲区（如果有的话） */
                        if (ws_partial_buf)
                        {
                            ESP_LOGW(TAG, "Orphaned partial buffer (%u bytes) discarded",
                                     (unsigned)ws_partial_len);
                            free(ws_partial_buf);
                            ws_partial_buf = NULL;
                            ws_partial_len = 0;
                        }
                        handle_ws_incoming_data((const char *)data->data_ptr, data->data_len);
                    }
                    else
                    {
                        /* JSON 不完整，缓存到部分缓冲区 */
                        if (ws_partial_buf)
                        {
                            ESP_LOGW(TAG, "Partial buffer replaced: %u -> %d bytes",
                                     (unsigned)ws_partial_len, data->data_len);
                            free(ws_partial_buf);
                        }
                        ws_partial_buf = malloc(data->data_len + 1);
                        if (ws_partial_buf)
                        {
                            memcpy(ws_partial_buf, data->data_ptr, data->data_len);
                            ws_partial_buf[data->data_len] = '\0';
                            ws_partial_len = (size_t)data->data_len;
                        }
                        ESP_LOGI(TAG, "WS partial buffered: %d bytes", data->data_len);
                    }
                }
            }
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "WebSocket DISCONNECTED");
            /* 断开时清理残留的部分缓冲区 */
            if (ws_partial_buf)
            {
                free(ws_partial_buf);
                ws_partial_buf = NULL;
                ws_partial_len = 0;
            }
            /* 断开时补发 END：播放任务会先播完队列里剩余 DATA，再销毁解码器 */
            AudioStream_Playback_Write(AUDIO_PLAYBACK_END, NULL, 0);
            break;
    }
}

/* ======================== 会话配置 ============================================== */

/**
 * @brief 发送 session.create 事件，配置 AI 角色和可用工具
 *
 * 在 WebSocket 连接建立后调用，向云端发送会话配置，包括：
 *   - AI 模型版本（1.2.6.0）
 *   - AI 角色设定（智能家居语音助手"小鱼同学"）
 *   - 音频输入格式：PCM 16kHz 单声道
 *   - 音频输出格式：OGG_OPUS 24kHz，使用 zh_female_vv_jupiter_bigtts 音色
 *   - Function Call 工具定义：
 *     - set_light：设置灯光（开关、亮度、RGB 颜色）
 *     - get_light：查询灯光状态
 *
 * 函数调用规则（通过 instructions 字段告知 AI）：
 *   1. 灯光控制必须调用 set_light 函数，不能用文字回复说做不到
 *   2. 灯光查询必须调用 get_light 函数
 *   3. 函数调用后根据返回结果用自然语言告知用户
 *   4. 回复简洁，一般不超过两句话
 */
static void send_session_create_event(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "session.create");

    cJSON *session = cJSON_CreateObject();
    cJSON_AddStringToObject(session, "model", "1.2.6.0");

    /* AI 角色设定：智能家居语音助手的行为规则 */
    cJSON_AddStringToObject(session, "instructions",
        "你是一个智能家居语音助手，名字叫'小鱼同学'。你可以直接控制用户的灯光设备。"
        "重要规则："
        "1. 当用户要求开灯、关灯、调节亮度、改变灯光颜色时，你必须调用 set_light 函数，不要用文字回复说做不到。进行set_light之前先get_light确定状态，如果已经是打开状态，但用户说打开灯，不需要set_light了，直接回复用户说:`灯是开着的`。"
        "2. 当用户询问当前灯光状态时，你必须调用 get_light 函数。"
        "3. 调用函数后，根据返回结果用自然语言告诉用户执行结果。"
        "4. 回复要简洁，一般不超过两句话。");

    cJSON *audio = cJSON_CreateObject();

    /* 音频输入配置：PCM 16kHz */
    cJSON *input = cJSON_CreateObject();
    cJSON *input_format = cJSON_CreateObject();
    cJSON_AddStringToObject(input_format, "type", "pcm");
    cJSON_AddNumberToObject(input_format, "rate", 16000);
    cJSON_AddItemToObject(input, "format", input_format);
    cJSON_AddItemToObject(audio, "input", input);

    /* 音频输出配置：OGG_OPUS 24kHz，指定音色 */
    cJSON *output = cJSON_CreateObject();
    cJSON *output_format = cJSON_CreateObject();
    cJSON_AddStringToObject(output_format, "type", "ogg_opus");
    cJSON_AddNumberToObject(output_format, "rate", 24000);
    cJSON_AddItemToObject(output, "format", output_format);
    cJSON_AddStringToObject(output, "voice", "zh_female_vv_jupiter_bigtts");
    cJSON_AddItemToObject(audio, "output", output);

    cJSON_AddItemToObject(session, "audio", audio);

    /* ========== 工具定义：set_light ========== */
    cJSON *tools = cJSON_CreateArray();

    cJSON *tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "type", "function");
    cJSON_AddStringToObject(tool, "name", "set_light");
    cJSON_AddStringToObject(tool, "description", "当用户要求开启、关闭、调节亮度或改变灯光颜色时，必须调用此函数。例如：'开灯'、'关掉卧室灯'、'把客厅灯调亮些'、'换成暖黄色'");

    cJSON *parameters = cJSON_CreateObject();
    cJSON_AddStringToObject(parameters, "type", "object");

    cJSON *properties = cJSON_CreateObject();

    cJSON *location = cJSON_CreateObject();
    cJSON_AddStringToObject(location, "type", "string");
    cJSON *locationEnum = cJSON_CreateStringArray((const char *[]){"bedroom", "living_room"}, 2);
    cJSON_AddItemToObject(location, "enum", locationEnum);
    cJSON_AddStringToObject(location, "description", "要控制的房间：bedroom=卧室，living_room=客厅。若用户未指定房间，默认使用bedroom");
    cJSON_AddItemToObject(properties, "location", location);

    cJSON *on = cJSON_CreateObject();
    cJSON_AddStringToObject(on, "type", "boolean");
    cJSON_AddStringToObject(on, "description", "是否开灯：true=开灯，false=关灯。用户说'开灯'/'打开'时设为true，'关灯'/'关闭'时设为false");
    cJSON_AddItemToObject(properties, "on", on);

    cJSON *brightness = cJSON_CreateObject();
    cJSON_AddStringToObject(brightness, "type", "integer");
    cJSON_AddNumberToObject(brightness, "minimum", 0);
    cJSON_AddNumberToObject(brightness, "maximum", 100);
    cJSON_AddStringToObject(brightness, "description", "亮度百分比0-100。用户说'最亮'时为100，'最暗'时为1，'调亮些'时在当前基础上+20，'调暗些'时-20，'一半亮度'时为50。若用户未提及亮度，默认50");
    cJSON_AddItemToObject(properties, "brightness", brightness);

    cJSON *colorR = cJSON_CreateObject();
    cJSON_AddStringToObject(colorR, "type", "integer");
    cJSON_AddNumberToObject(colorR, "minimum", 0);
    cJSON_AddNumberToObject(colorR, "maximum", 255);
    cJSON_AddStringToObject(colorR, "description", "红色分量0-255。AI根据用户描述的颜色自动计算，如'暖黄'→R:255,G:180,B:80，'冷白'→R:200,G:220,B:255，'红色'→R:255,G:0,B:0");
    cJSON_AddItemToObject(properties, "color_r", colorR);

    cJSON *colorG = cJSON_CreateObject();
    cJSON_AddStringToObject(colorG, "type", "integer");
    cJSON_AddNumberToObject(colorG, "minimum", 0);
    cJSON_AddNumberToObject(colorG, "maximum", 255);
    cJSON_AddStringToObject(colorG, "description", "绿色分量0-255，由AI根据用户颜色描述自动计算");
    cJSON_AddItemToObject(properties, "color_g", colorG);

    cJSON *colorB = cJSON_CreateObject();
    cJSON_AddStringToObject(colorB, "type", "integer");
    cJSON_AddNumberToObject(colorB, "minimum", 0);
    cJSON_AddNumberToObject(colorB, "maximum", 255);
    cJSON_AddStringToObject(colorB, "description", "蓝色分量0-255，由AI根据用户颜色描述自动计算");
    cJSON_AddItemToObject(properties, "color_b", colorB);

    cJSON_AddItemToObject(parameters, "properties", properties);

    cJSON *required = cJSON_CreateStringArray((const char *[]){"location"}, 1);
    cJSON_AddItemToObject(parameters, "required", required);

    cJSON_AddItemToObject(tool, "parameters", parameters);
    cJSON_AddItemToArray(tools, tool);

    /* ========== 工具定义：get_light ========== */
    cJSON *tool2 = cJSON_CreateObject();
    cJSON_AddStringToObject(tool2, "type", "function");
    cJSON_AddStringToObject(tool2, "name", "get_light");
    cJSON_AddStringToObject(tool2, "description", "当用户询问灯光当前状态时，必须调用此函数。例如：'卧室灯开着吗'、'客厅灯现在什么颜色'、'灯亮不亮'、'当前亮度多少'");

    cJSON *parameters2 = cJSON_CreateObject();
    cJSON_AddStringToObject(parameters2, "type", "object");

    cJSON *properties2 = cJSON_CreateObject();

    cJSON *location2 = cJSON_CreateObject();
    cJSON_AddStringToObject(location2, "type", "string");
    cJSON *locationEnum2 = cJSON_CreateStringArray((const char *[]){"bedroom", "living_room"}, 2);
    cJSON_AddItemToObject(location2, "enum", locationEnum2);
    cJSON_AddStringToObject(location2, "description", "要查询的房间：bedroom=卧室，living_room=客厅。若用户未指定房间，默认使用bedroom");
    cJSON_AddItemToObject(properties2, "location", location2);

    cJSON_AddItemToObject(parameters2, "properties", properties2);

    cJSON *required2 = cJSON_CreateStringArray((const char *[]){"location"}, 1);
    cJSON_AddItemToObject(parameters2, "required", required2);

    cJSON_AddItemToObject(tool2, "parameters", parameters2);
    cJSON_AddItemToArray(tools, tool2);

    cJSON_AddItemToObject(session, "tools", tools);

    cJSON_AddItemToObject(root, "session", session);

    char *json_str = cJSON_PrintUnformatted(root);
    ESP_LOGI(TAG, "Sending session.create: %s", json_str);
    if (ws_client) {
        esp_websocket_client_send_text(ws_client, json_str, strlen(json_str), pdMS_TO_TICKS(1000));
    }
    free(json_str);
    cJSON_Delete(root);
}

/* ======================== AI Cloud 公共 API ====================================== */

/**
 * @brief 初始化 AI Cloud 模块
 *
 * 初始化流程：
 *   1. 创建 WebSocket 操作互斥锁（保护 send 操作的线程安全）
 *   2. 配置 WebSocket 客户端参数（URI、主机、端口、证书、缓冲区等）
 *   3. 设置 X-Api-Key 认证请求头
 *   4. 保存 API Key 副本
 *   5. 注册 WebSocket 事件回调
 *   6. 创建音频上传任务
 *
 * 注意：初始化不会建立 WebSocket 连接，需调用 AiCloud_Start() 启动连接。
 *
 * @param url    WebSocket 服务器地址
 * @param token  API 认证令牌
 * @return ESP_OK 初始化成功
 */
esp_err_t AiCloud_Init(const char *url, const char *token)
{
    ws_op_lock = xSemaphoreCreateMutex();

    /* 配置 WebSocket 客户端参数 */
    const esp_websocket_client_config_t ws_cfg = {
        .uri = url,
        .host = "openspeech.bytedance.com",
        .port = 443,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        .buffer_size = 1024 * 32,
        .task_stack = 1024 * 10,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    ws_client = esp_websocket_client_init(&ws_cfg);

    if (ws_client) {
        /* 设置认证请求头 */
        esp_websocket_client_append_header(ws_client, "X-Api-Key", token);
    }

    g_api_key = strdup(token);

    /* 注册 WebSocket 事件回调 */
    esp_websocket_register_events(ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);

    /* 创建音频上传任务（任务持续运行，在连接时上传音频，未连接时丢弃数据） */
    xTaskCreate(AiCloud_task, "AiCloud_task", 8192, NULL, 10, NULL);
    return ESP_OK;
}

/**
 * @brief 启动 WebSocket 连接
 *
 * 建立与云端服务器的 WebSocket 连接。
 * 连接成功后，websocket_event_handler 会自动发送 session.create 事件。
 *
 * @return ESP_OK   启动成功
 *         ESP_FAIL 客户端未初始化
 */
esp_err_t AiCloud_Start(void)
{
    if (ws_client) {
        return esp_websocket_client_start(ws_client);
    }
    return ESP_FAIL;
}

/**
 * @brief 停止 WebSocket 连接
 *
 * 断开与云端服务器的 WebSocket 连接。
 * 使用互斥锁确保停止操作与正在进行的 send 操作不冲突。
 *
 * @return ESP_OK 停止成功
 */
esp_err_t AiCloud_Stop(void)
{
    if (ws_client) {
        if (ws_op_lock && xSemaphoreTake(ws_op_lock, portMAX_DELAY) == pdTRUE) {
            esp_websocket_client_stop(ws_client);
            xSemaphoreGive(ws_op_lock);
        }
    }
    return ESP_OK;
}

/* ======================== 音频上传任务 =========================================== */

/**
 * @brief AI Cloud 音频上传任务的主循环
 *
 * 该任务作为 FreeRTOS 线程持续运行，执行以下流程：
 *   1. 注册为 AudioStream 的消费者，获取独立的读句柄
 *   2. 循环从环形缓冲区读取 PCM 16-bit 音频数据
 *   3. 检查 WebSocket 连接状态：
 *      - 未连接：丢弃音频数据，持续推进读指针（避免积压旧数据）
 *      - 已连接：Base64 编码后构造 JSON 消息发送至云端
 *
 * 音频数据流向：
 *   麦克风 → AudioStream 环形缓冲区 → 本任务读取 → Base64 编码 → WebSocket 上传
 *
 * 与 ASR 任务的关系：
 *   本任务与 ASR 唤醒词检测任务共享同一个环形缓冲区，
 *   但各自拥有独立的读指针，互不干扰。
 *
 * @param pvParameters FreeRTOS 任务参数（本任务未使用，传入 NULL）
 */
void AiCloud_task(void *pvParameters)
{
    /*
     * 注册为 AudioStream 的消费者，获取独立的读句柄。
     * 与 ASR 任务共享同一个环形缓冲区，但各自拥有独立的读指针，
     * 互不干扰。
     */
    AudioStream_ReaderHandle_t audioReader = AudioStream_Reader_Register();
    if (audioReader == NULL)
    {
        ESP_LOGE(TAG, "Failed to register as audio stream reader");
        vTaskDelete(NULL);
        return;
    }

    size_t sample_count = AI_CLOUD_READ_BUF_LEN;
    int16_t *pcm_buffer = malloc(sample_count * sizeof(int16_t));

    while (1) {
        size_t samples_read = 0;
        esp_err_t ret = AudioStream_Read(audioReader, pcm_buffer, sample_count, &samples_read, 100);

        if (ret != ESP_OK || samples_read == 0) {
            continue;
        }

        /*
         * WebSocket 未连接时：丢弃音频数据，但持续推进读指针，
         * 避免积压旧数据导致唤醒后上传乱序音频。
         */
        if (!(ws_client && esp_websocket_client_is_connected(ws_client))) {
            continue;
        }

        /* Base64 编码：将 PCM 16-bit 原始音频编码为文本格式 */
        size_t bytes_to_encode = samples_read * sizeof(int16_t);
        size_t b64_buf_len = (bytes_to_encode * 4 / 3) + 4;
        char *b64_str = malloc(b64_buf_len);
        size_t b64_out_len = 0;
        mbedtls_base64_encode((unsigned char *)b64_str, b64_buf_len, &b64_out_len, (unsigned char *)pcm_buffer, bytes_to_encode);

        /* 构造 input_audio_buffer.append JSON 消息 */
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "type", "input_audio_buffer.append");
        cJSON_AddStringToObject(root, "audio", b64_str);
        char *json_send = cJSON_PrintUnformatted(root);

        /*
         * 互斥锁保护 send 操作：
         * AiCloud_Stop() 可能在另一个线程中调用，需要确保 send 和 stop 不冲突。
         */
        if (ws_op_lock && xSemaphoreTake(ws_op_lock, portMAX_DELAY) == pdTRUE) {
            if (esp_websocket_client_is_connected(ws_client)) {
                esp_websocket_client_send_text(ws_client, json_send, strlen(json_send), pdMS_TO_TICKS(500));
            }
            xSemaphoreGive(ws_op_lock);
        }

        free(json_send);
        free(b64_str);
        cJSON_Delete(root);
    }

    /* 任务退出：注销消费者，释放缓冲区 */
    AudioStream_Reader_Unregister(audioReader);
    free(pcm_buffer);
    vTaskDelete(NULL);
}