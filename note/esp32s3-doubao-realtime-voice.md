# ESP32-S3 与豆包端到端实时语音连接技术文档

## 概述

本文档描述 ESP32-S3 通过 WebSocket 连接豆包（字节跳动）端到端实时语音 API 的完整技术方案，包括音频采集、上传、云端处理、OGG Opus 解码、重采样到功放输出的全链路。

### 硬件平台

- **芯片**：ESP32-S3（需 PSRAM）
- **麦克风**：I2S MEMS 麦克风（16kHz / 16-bit / 单声道）
- **功放**：NS4168 D 类功放，I2S 输入（16kHz / 16-bit / 单声道）
- **WiFi**：2.4GHz，关闭省电模式

### 软件栈

| 层级 | 组件 |
|------|------|
| RTOS | FreeRTOS (ESP-IDF v5.2.6) |
| WiFi | esp_wifi + lwIP |
| WebSocket | esp_websocket_client ^1.2.4 |
| JSON | cJSON |
| 音频解码 | esphome/micro-opus ^0.4.1 (OGG Opus → PCM) |
| 音频重采样 | 自研 3-tap 平均（48kHz → 16kHz） |
| 语音唤醒 | espressif/esp-sr ^2.0.0 (MultiNet WakeNet) |

---

## 架构图

```
┌─────────────────────────────────────────────────────────────────┐
│                          ESP32-S3                               │
│                                                                 │
│  ┌──────────┐   ┌──────────────┐   ┌─────────────────────────┐ │
│  │ I2S MEMS │──▶│ AudioStream  │──▶│ AiCloud_task            │ │
│  │ 麦克风   │   │ (环形缓冲区) │   │ base64编码 → JSON封装   │ │
│  │ 16kHz    │   │              │   │ → WebSocket 上传        │ │
│  └──────────┘   └──────────────┘   └───────────┬─────────────┘ │
│                                                  │               │
│                                          wss://openspeech.      │
│                                          bytedance.com/api/v3/  │
│                                          duplex/realtime/       │
│                                          dialogue               │
│                                                  │               │
│  ┌──────────┐   ┌──────────────┐   ┌───────────▼─────────────┐ │
│  │ NS4168   │◀──│ Amplifier    │◀──│ websocket_event_handler │ │
│  │ 功放     │   │ I2S TX 16kHz │   │ 消息拼接 → JSON解析    │ │
│  │          │   │              │   │ → base64解码 → OGG解码  │ │
│  └──────────┘   └──────────────┘   │ → 48k→16k重采样 → 播放 │ │
│                                     └─────────────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
```

---

## 核心模块说明

### 1. 音频采集（AudioStream 环形缓冲区）

位于 `components/audio_stream/`，生产者-消费者模式：

- **生产者**：I2S 麦克风中断回调，将 16kHz PCM 数据写入环形缓冲区
- **消费者**：`AiCloud_task` 和 ASR 任务各自注册独立读句柄，互不干扰

```c
// 注册消费者
AudioStream_ReaderHandle_t reader = AudioStream_Reader_Register();

// 读取音频（非阻塞，100ms 超时）
size_t samples_read = 0;
AudioStream_Read(reader, pcm_buffer, 512, &samples_read, 100);
```

### 2. WebSocket 连接与会话管理

**连接地址**：`wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue`

**配置**：
```c
const esp_websocket_client_config_t ws_cfg = {
    .uri = url,
    .host = "openspeech.bytedance.com",
    .port = 443,
    .reconnect_timeout_ms = 5000,
    .network_timeout_ms = 10000,
    .buffer_size = 1024 * 16,       // 16KB 接收缓冲
    .crt_bundle_attach = esp_crt_bundle_attach,  // 使用内置证书
};
```

**认证**：通过 HTTP Header `X-Api-Key` 传递 Access Token。

**会话创建**（旧版协议，模型 `1.2.6.1`）：
```json
{
  "type": "session.create",
  "session": {
    "model": "1.2.6.1",
    "voice": "zh_female_01",
    "audio": {
      "input":  { "encoding": "pcm", "rate": 16000 },
      "output": { "encoding": "pcm", "rate": 24000 }
    }
  }
}
```

> **注意**：模型 `1.2.6.1` 不支持新 Duplex API 的 `session.update` 事件名，也不支持 `session.audio.output.voice` 字段位置。

### 3. 音频上传

`AiCloud_task` 持续从环形缓冲区读取音频，base64 编码后封装为 JSON 发送：

```json
{
  "type": "input_audio_buffer.append",
  "audio": "<base64 encoded PCM>"
}
```

语音结束后发送 commit：
```json
{
  "type": "input_audio_buffer.commit"
}
```

10 秒超时保护：如果服务端 10 秒内没有返回音频，自动发送 commit。

### 4. 消息接收与拼接（关键逻辑）

豆包服务端可能将一个大的 audio delta JSON 拆成 2~3 个独立的 WebSocket text message。**这不是标准的 WebSocket 帧分片**（fin 始终为 1），需要通过内容特征识别并手动拼接。

详见 [踩坑记录第5条](pitfalls.md#5-websocket-消息被服务端拆成多段核心坑)。

### 5. OGG Opus 解码与重采样

**解码链路**：
```
base64 字符串 → OGG 字节流 → OggOpusDecoder → 48kHz PCM → 3-tap平均降采样 → 16kHz PCM → I2S TX
```

**OGG 解码器封装**：`components/ogg_decoder/`（C 接口封装 C++ micro_opus 解码器）

```c
OggDecoder *ogg_dec = ogg_decoder_create();

// 流式喂入数据，decoder 内部处理 OGG page 拼装
int decoded = ogg_decoder_feed(ogg_dec, ogg_data, out_len,
                                pcm_48k_buf, 5760, &consumed);
// decoded > 0: 解码出 PCM 样本数
// decoded = 0: 数据不足，继续喂入下一批
// decoded < 0: 解码错误
```

**重采样**：Opus 内部解码为 48kHz，用 3-tap 移动平均降到 16kHz（每 3 个 48k 样本取平均得 1 个 16k 样本）。质量足够，计算量极小。

**缓冲区大小**：
- `pcm_48k_buf`: 5760 samples（120ms @ 48kHz）
- `pcm_16k_buf`: 1920 samples（120ms @ 16kHz）

### 6. 功放输出

位于 `components/amplifier/`，调用 `Amplifier_Play_Buffer()`：
- 格式：16kHz / 16-bit / 单声道
- 通过 I2S TX 发送给 NS4168
- 写入超时：`portMAX_DELAY`（必须，短超时会丢帧）

---

## 事件流

### 会话建立
```
Client → Server:  session.create
Server → Client:  session.created   (返回 session.id)
```

### 一轮对话的完整事件顺序
```
Client → Server:  input_audio_buffer.append   (多次，发送音频)
Client → Server:  input_audio_buffer.commit   (语音结束)

Server → Client:  conversation.item.input_audio_transcription.started
Server → Client:  conversation.item.input_audio_transcription.delta   (多次，ASR结果)
Server → Client:  conversation.item.input_audio_transcription.completed

Server → Client:  response.output_text.delta       (多次，文字回复)
Server → Client:  response.output_audio.started    (音频开始)
Server → Client:  response.output_text.done        (文字回复结束)
Server → Client:  response.output_audio.delta      (多次，音频数据)
Server → Client:  response.output_audio.done       (音频结束)
Server → Client:  response.done                    (本轮回复结束)
```

---

## 关键配置总结

| 配置项 | 值 | 说明 |
|--------|-----|------|
| 音频输入 | 16kHz / 16-bit / mono / PCM | 麦克风采样参数 |
| 音频输出 | 16kHz / 16-bit / mono / PCM | 重采样后给功放 |
| 云端音频 | 24kHz OGG Opus | 模型 `1.2.6.1` 固定格式 |
| Opus 内部 | 48kHz | micro_opus 解码输出 |
| WiFi 省电 | WIFI_PS_NONE | 必须关闭，否则断连 |
| WebSocket buffer | 16384 bytes | 接收缓冲 |
| I2S 写入超时 | portMAX_DELAY | 避免 DMA buffer 溢出丢帧 |
| Commit 超时 | 10 秒 | 防止云端不返回音频 |
| 播放队列 | 无（同步播放） | 异步队列引入时序问题，已废弃 |

---

## 文件索引

| 文件 | 功能 |
|------|------|
| `main/idf_component.yml` | 组件依赖声明 |
| `components/ai_cloud/ai_cloud.c` | WebSocket 连接、消息处理、OGG 解码、音频播放 |
| `components/ai_cloud/ai_cloud.h` | AiCloud 模块接口 |
| `components/ogg_decoder/ogg_decoder.h` | OGG 解码器 C 接口 |
| `components/ogg_decoder/ogg_decoder.cpp` | OGG 解码器 C++ 实现 |
| `components/audio_stream/` | 麦克风环形缓冲区 |
| `components/amplifier/amplifier.c` | 功放 I2S 输出 |
| `components/my_i2s/i2s_driver.c` | I2S 底层驱动 |
| `components/wifi/wifi.c` | WiFi 连接管理 |
| `components/CMakeLists.txt` | 组件构建配置 |
