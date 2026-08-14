# 豆包实时语音 — 添加 Function Calling 支持



注意注意注意！！！

这个1.2.6.1版本，也就是从火山[豆包语音](https://console.volcengine.com/speech/new/setting/activate?projectName=default) 这里找的文档是不支持Function Calling的，他说是支持，但是根本不支持，要去github上找到1.2.6.0的版本才行。具体直接看`realtime_duplex.md`这个文件，也在这个note文件夹下面。github链接为：https://github.com/GizClaw/doubao-speech-go/blob/main/docs/realtime_duplex.md



## 概述

豆包端到端实时语音 API 支持 Function Calling（又叫 Tool Use），让 AI 能调用 ESP32 上的本地功能（如控制灯、读取传感器）。整个链路：

```
用户语音 → 豆包云端 → response.function_call_arguments.done → ESP32 执行 → 回传结果 → 豆包回复确认
```

## 需要的三方交互

### 1. 客户端 → 服务端：注册可调用函数

在 `session.create` 的 JSON 里加 `tools` 数组：

```json
{
  "type": "session.create",
  "session": {
    "model": "1.2.6.1",
    "voice": "zh_female_01",
    "audio": { ... },
    "tools": [
      {
        "type": "function",
        "function": {
          "name": "set_light",
          "description": "Control room light: on/off, brightness, color",
          "parameters": {
            "type": "object",
            "properties": {
              "location": {
                "type": "string",
                "enum": ["bedroom", "living_room"],
                "description": "Room to control"
              },
              "on": {
                "type": "boolean",
                "description": "Turn light on or off"
              },
              "brightness": {
                "type": "integer",
                "minimum": 0,
                "maximum": 100,
                "description": "Brightness percentage 0-100"
              },
              "color_r": {
                "type": "integer",
                "minimum": 0,
                "maximum": 255,
                "description": "Red component 0-255. AI calculates from user's color description (e.g. 'warm yellow'->R:255,G:180,B:80)"
              },
              "color_g": {
                "type": "integer",
                "minimum": 0,
                "maximum": 255,
                "description": "Green component 0-255"
              },
              "color_b": {
                "type": "integer",
                "minimum": 0,
                "maximum": 255,
                "description": "Blue component 0-255"
              }
            },
            "required": ["location"]
          }
        }
      }
    ]
  }
}
```

**参数设计要点：**

- `type` 固定为 `"function"`
- `function.name` 是 ESP32 收到后用来路由的函数名
- `function.parameters` 用 JSON Schema 格式描述参数
- `enum` 限制可选值，避免 AI 传回无法处理的值
- `required` 标记必填参数
- `description` 帮助 AI 理解参数含义

**set_light + get_light 成对注册（完整示例）：**

```json
"tools": [
  {
    "type": "function",
    "function": {
      "name": "set_light",
      "description": "Control room light: on/off, brightness, color",
      "parameters": {
        "type": "object",
        "properties": {
          "location": { "type": "string", "enum": ["bedroom", "living_room"], "description": "Room to control" },
          "on": { "type": "boolean", "description": "Turn light on or off" },
          "brightness": { "type": "integer", "minimum": 0, "maximum": 100, "description": "Brightness percentage 0-100" },
          "color_r": { "type": "integer", "minimum": 0, "maximum": 255, "description": "Red 0-255, AI calculates from user's color description" },
          "color_g": { "type": "integer", "minimum": 0, "maximum": 255, "description": "Green 0-255" },
          "color_b": { "type": "integer", "minimum": 0, "maximum": 255, "description": "Blue 0-255" }
        },
        "required": ["location"]
      }
    }
  },
  {
    "type": "function",
    "function": {
      "name": "get_light",
      "description": "Query current light state: whether it's on, brightness, color",
      "parameters": {
        "type": "object",
        "properties": {
          "location": { "type": "string", "enum": ["bedroom", "living_room"], "description": "Which room to query" }
        },
        "required": ["location"]
      }
    }
  }
]
```

AI 会根据用户意图自动选函数：用户说"打开卧室灯" → 调 `set_light`，用户问"卧室灯什么颜色" → 调 `get_light`。

**扩展到更多函数：**

```json
"tools": [
  { "type": "function", "function": { "name": "set_light", ... } },
  { "type": "function", "function": { "name": "get_light", ... } },
  { "type": "function", "function": { "name": "get_temperature", ... } },
  { "type": "function", "function": { "name": "set_ac", ... } }
]
```

### 2. 服务端 → 客户端：下发函数调用

豆包识别用户意图后，发送这个事件：

```json
{
  "type": "response.function_call_arguments.done",
  "event_id": "event_57",
  "call_id": "call_abc123",
  "name": "set_light",
  "arguments": "{\"location\":\"bedroom\",\"on\":true,\"brightness\":80,\"color_r\":255,\"color_g\":180,\"color_b\":80}"
}
```

关键字段：
- `call_id` — 回传时必须原样带回，用于关联请求
- `name` — 函数名，对应 tools 里注册的 function.name
- `arguments` — 注意这是 **JSON 字符串**，不是 JSON 对象，需要二次 `cJSON_Parse`

**颜色与亮度的关系：**

RGB 三个值决定"什么颜色"，brightness 决定"多亮"。ESP32 端按公式 `duty = (color × brightness × max_duty) / (255 × 100)` 等比例缩放，所以 AI 不需要自己算缩放后的 RGB。

当用户只改亮度不改颜色时（如"调亮一点"），AI 必须从对话上下文中记住当前颜色，把同样的 `color_r/g/b` 带上：

```
用户: "打开卧室灯，暖黄色"
AI:  → {"color_r":255, "color_g":180, "color_b":80, "brightness":100}
      ← {"status":"ok", "color_r":255, "color_g":180, "color_b":80, "brightness":100}
      (AI 记住了当前颜色: R=255, G=180, B=80)

用户: "亮度调到 50%"
AI:  → {"color_r":255, "color_g":180, "color_b":80, "brightness":50}
      (带上记住的 RGB，只改 brightness)
```

> ESP32 端也有兜底：如果 AI 没传 color_r/g/b（全为 0），则保留当前颜色不变，只改亮度。

### 3. 客户端 → 服务端：回传执行结果

ESP32 执行完必须发回执，否则豆包不知道执行是否成功：

```json
{
  "type": "conversation.item.create",
  "item": {
    "type": "function_call_output",
    "call_id": "call_abc123",
    "output": "{\"status\":\"ok\",\"location\":\"bedroom\",\"on\":true,\"brightness\":80,\"color_r\":255,\"color_g\":180,\"color_b\":80}"
  }
}
```

`call_id` 必须和收到的完全一致。`output` 是 JSON 字符串。

**关键：`output` 里带上完整状态，不只是 `{"status":"ok"}`。** 这样 AI 在对话上下文里就知道灯当前是什么状态，用户问"灯现在什么颜色"时不需要再调 `get_light` 也能回答。

**`get_light` 的回执也一样格式：**

```json
{
  "type": "conversation.item.create",
  "item": {
    "type": "function_call_output",
    "call_id": "call_abc456",
    "output": "{\"status\":\"ok\",\"location\":\"bedroom\",\"on\":false,\"brightness\":50,\"color_r\":255,\"color_g\":0,\"color_b\":0}"
  }
}
```

## ESP32 端代码实现

### 在 handle_ws_incoming_data 中添加处理

```c
else if (strcmp(type->valuestring, "response.function_call_arguments.done") == 0)
{
    cJSON *call_id = cJSON_GetObjectItem(root, "call_id");
    cJSON *fun_name = cJSON_GetObjectItem(root, "name");
    cJSON *args_raw = cJSON_GetObjectItem(root, "arguments");

    if (!call_id || !fun_name || !args_raw) return;

    ESP_LOGI(TAG, "Function call: %s, id=%s", fun_name->valuestring, call_id->valuestring);

    /* === 路由到具体的处理函数 === */
    bool ok = false;
    char output_buf[256] = {0};

    if (strcmp(fun_name->valuestring, "set_light") == 0)
    {
        cJSON *args = cJSON_Parse(args_raw->valuestring);
        if (args)
        {
            const char *loc = cJSON_GetObjectItem(args, "location")->valuestring;
            bool on = cJSON_IsTrue(cJSON_GetObjectItem(args, "on"));
            int bri = cJSON_GetObjectItem(args, "brightness")->valueint;

            /* 构造 LED_ID */
            LED_ID led_id = { .id = (strcmp(loc, "bedroom") == 0) ? LED_ID_BEDROOM : LED_ID_LIVINGROOM,
                              .name = loc };

            /* 先读取当前状态，作为兜底 */
            LED_Control_State cur;
            LED_Control_Get_Light(led_id, &cur);

            /* 解析 AI 传来的 RGB，如果没传或全为 0 则保留当前颜色 */
            cJSON *r_item = cJSON_GetObjectItem(args, "color_r");
            cJSON *g_item = cJSON_GetObjectItem(args, "color_g");
            cJSON *b_item = cJSON_GetObjectItem(args, "color_b");

            uint8_t r = (r_item && r_item->valueint > 0) ? (uint8_t)r_item->valueint : cur.color_r;
            uint8_t g = (g_item && g_item->valueint > 0) ? (uint8_t)g_item->valueint : cur.color_g;
            uint8_t b = (b_item && b_item->valueint > 0) ? (uint8_t)b_item->valueint : cur.color_b;

            LED_Control_State state = {
                .is_on      = on,
                .brightness = (uint8_t)bri,
                .color_r    = r,
                .color_g    = g,
                .color_b    = b,
            };
            ok = (LED_Control_Set_Light(led_id, state) == ESP_OK);
            cJSON_Delete(args);

            /* 回执里带上完整状态 */
            LED_Control_Get_Light(led_id, &cur);
            snprintf(output_buf, sizeof(output_buf),
                "{\"status\":\"%s\",\"location\":\"%s\",\"on\":%s,\"brightness\":%d,\"color_r\":%d,\"color_g\":%d,\"color_b\":%d}",
                ok ? "ok" : "error", loc, cur.is_on ? "true" : "false",
                cur.brightness, cur.color_r, cur.color_g, cur.color_b);
        }
    }
    else if (strcmp(fun_name->valuestring, "get_light") == 0)
    {
        cJSON *args = cJSON_Parse(args_raw->valuestring);
        if (args)
        {
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

    /* === 回传执行结果 === */
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "conversation.item.create");

    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "function_call_output");
    cJSON_AddStringToObject(item, "call_id", call_id->valuestring);
    cJSON_AddStringToObject(item, "output", output_buf[0] ? output_buf : "{\"status\":\"error\"}");

    cJSON_AddItemToObject(resp, "item", item);

    char *json_str = cJSON_PrintUnformatted(resp);
    esp_websocket_client_send_text(ws_client, json_str, strlen(json_str), pdMS_TO_TICKS(1000));
    free(json_str);
    cJSON_Delete(resp);
}
```

### 路由模式

多个函数时用 if-else + strcmp 做路由，清晰直接：

```c
if (strcmp(fun_name->valuestring, "set_light") == 0)
{
    // 处理灯光设置（写操作）
}
else if (strcmp(fun_name->valuestring, "get_light") == 0)
{
    // 查询灯光状态（读操作）
}
else if (strcmp(fun_name->valuestring, "get_temperature") == 0)
{
    // 读取温度传感器
}
else if (strcmp(fun_name->valuestring, "set_ac") == 0)
{
    // 控制空调
}
else
{
    ESP_LOGW(TAG, "Unknown function: %s", fun_name->valuestring);
}
```

## 让 AI 知道灯当前状态

AI 本身不记设备状态，有两种方式互补：

### 写操作时附带完整状态

`set_light` 的回执里带上执行后的全量状态（location / on / brightness / color_r / color_g / color_b），AI 在对话上下文里就能记住。用户问"灯现在什么颜色"时，AI 直接回答，不需要再查 ESP32。

### 读操作时主动查询

用户问了一个 AI 不知道状态的问题（比如刚连接、或问的是之前没操作过的灯），AI 会调 `get_light` → ESP32 返回当前实际状态 → AI 用语音回复用户。

```
用户: "卧室灯开着没？"
  ↓
Server → Client:  response.function_call_arguments.done
  { "name": "get_light", "arguments": "{\"location\":\"bedroom\"}" }
  ↓
Client → Server:  conversation.item.create
  { "item": { "type": "function_call_output", "output": "{\"status\":\"ok\",\"location\":\"bedroom\",\"on\":false,\"color_r\":255,\"color_g\":0,\"color_b\":0,...}" } }
  ↓
Server → Client:  response.output_audio.delta  (AI: "卧室灯现在关着的。")
```

两种方式配合就够了，不需要周期性上报状态。

---

## 完整的事件流

```
Client → Server:  session.create (含 tools 数组)

... 用户语音 "打开卧室灯" ...

Server → Client:  conversation.item.input_audio_transcription.delta  (ASR识别)
Server → Client:  conversation.item.input_audio_transcription.completed
Server → Client:  response.function_call_arguments.done  ← 函数调用

Client → Server:  conversation.item.create              ← ESP32 回传结果
  { "item": { "type": "function_call_output", "call_id": "xxx", "output": "{...}" } }

Server → Client:  response.output_audio.delta           ← AI 确认语音："好的，卧室灯已经打开"
Server → Client:  response.output_audio.done
Server → Client:  response.done
```

## 支持的 JSON Schema 参数类型

| type | 示例 |
|------|------|
| `"string"` | `{"type":"string","enum":["red","green"]}` |
| `"boolean"` | `{"type":"boolean"}` |
| `"integer"` | `{"type":"integer","minimum":0,"maximum":100}` |
| `"number"` | `{"type":"number","minimum":0.0,"maximum":1.0}` |
| `"object"` | `{"type":"object","properties":{...}}` |

## 添加新函数的步骤

1. 在 `session.create` 的 `tools` 数组里加一个新的 function 对象
2. 如果是读操作 → 在 LED_Control 模块里加对应的 Getter 函数（如 `LED_Control_GetLight`）
3. 在 `handle_ws_incoming_data` 的路由里加一个 `else if (strcmp(name, "xxx") == 0)`
4. 回执的 `output` 里带上完整状态（写操作也一样），让 AI 对话上下文保持最新
5. 编译烧录测试

不需要改任何其他的现有代码。