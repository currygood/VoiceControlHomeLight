# ESP32-S3 豆包实时语音 — 踩坑记录

## 1. 云端返回的是 OGG Opus，不是 PCM

**现象**：音频全噪音。

**原因**：豆包模型 `1.2.6.1` 的端到端语音只输出 24kHz OGG Opus 格式，`session.create` 里无论怎么配 `encoding: "pcm"` 都没用。

**解决**：使用 `esphome/micro-opus` 组件解码 OGG Opus。解码后是 48kHz PCM，再降采样到 16kHz 给功放。

**判断方法**：base64 解码后看前几个字节是不是 `0x4F 0x67 0x67 0x53`（即 ASCII `OggS`）。

---

## 2. 不要在音频数据上直接跑重采样

**现象**：用 Lanczos 重采样后输出全噪音。

**原因**：云端返回 OGG 容器格式数据，对 OGG 字节流直接重采样没有意义。

**解决**：先解码 OGG → 得到 48kHz PCM → 再用简单平均降采样到 16kHz。

```c
// 48k -> 16k: 每3个48k样本取平均得到1个16k样本
static size_t resample_48k_to_16k(const int16_t *input, size_t input_samples, int16_t *output)
{
    size_t n = input_samples / 3;
    for (size_t i = 0; i < n; i++)
    {
        int32_t sum = (int32_t)input[i * 3] + (int32_t)input[i * 3 + 1] + (int32_t)input[i * 3 + 2];
        output[i] = (int16_t)(sum / 3);
    }
    return n;
}
```

---

## 3. WiFi 省电模式导致断连

**现象**：对话空闲几分钟后 WebSocket 断开，服务器无响应。

**原因**：`esp_wifi_set_ps(WIFI_PS_MIN_MODEM)` 在空闲时进入省电模式，WebSocket 长连接的心跳包唤醒不及时。

**解决**：关闭 WiFi 省电。

```c
esp_wifi_set_ps(WIFI_PS_NONE);  // 连接成功后立即设置
```

---

## 4. I2S DMA 写入超时导致帧丢失

**现象**：音频播放到一半就停了，后面没声音。

**原因**：`Amplifier_Play_Buffer` 内部 `i2s_channel_write` 的 timeout 太小（100ms），DMA buffer 来不及排空就超时返回，后续帧被丢弃。

**解决**：timeout 改为 `portMAX_DELAY`，让写入操作等待 DMA buffer 排空。

---

## 5. WebSocket 消息被服务端拆成多段（核心坑）

**现象**：第一个 audio delta 能解码，后续 delta 全部 `OGG decode error: ret=-1`，第一轮对话没声音或播不完。

**原因**：豆包服务端把大的 audio delta（约 5-6KB JSON）拆成 **2~3 个独立的 WebSocket text message**，不是标准的 WebSocket 帧分片。ESP-IDF WebSocket 客户端总是 `fin=1`，无法通过协议标志检测分片。

数据示例：
```
Message 1 (4106 bytes): {"type":"response.output_audio.delta","delta":"T2dnUwAC..."  ← 截断
Message 2 (1341 bytes): m7siJysJasI4MPSbseQM8KGkfhcxiVIKVY0EqL4S6wW86YcF8...     ← base64续接
Message 3 (~800 bytes): ...AAAAAAAA=="}                                           ← base64尾部+JSON闭合
```

单独的 Message 1 无法用 cJSON 解析 → OGG 头部（OpusHead/OpusTags）丢失 → 后续 delta 送到解码器全部报错。

**解决**：在 `websocket_event_handler` 中实现手动拼接：

1. 收到的 text message 先用 cJSON 试着解析
2. 解析失败 → 缓存数据到 `ws_partial_buf`
3. 下一帧首字符不是 `{` 且已有缓存 → 追加到缓存，尝试解析拼接后的内容
4. 拼接后解析仍然失败 → **保留缓存**（可能有第 3、第 4 段）
5. 拼接后解析成功 → 处理完整 JSON，释放缓存
6. 收到新的 `{` 开头消息且解析成功 → 孤儿缓存丢弃

```c
case WEBSOCKET_EVENT_DATA:
    if (data->op_code == 0x01)
    {
        bool is_continuation = (data->data_len > 0 && data->data_ptr[0] != '{' && ws_partial_buf != NULL);
        if (is_continuation)
        {
            // 追加到缓存，尝试解析，不成功就保留缓存等下一段
        }
        else
        {
            // 新消息：先试解析，失败则缓存
        }
    }
```

---

## 6. `session.update` 不兼容旧模型

**现象**：发送 `session.update` 后服务端返回 `error` 事件，WebSocket 断开。

**原因**：模型 `1.2.6.1` 使用旧版 API 协议，只认 `session.create`，不认识新 Duplex API 的 `session.update`。`voice` 字段也只能放在 `session.voice`（顶层），不能放在 `session.audio.output.voice`。

**解决**：使用旧版格式：

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

---

## 7. OGG 解码器状态管理

**现象**：对话多轮后解码器状态混乱，decode 报错。

**原因**：OGG 解码器跨 delta 保持状态（一个 response 的所有 deltas 属于同一个 OGG 流）。但如果 `response.output_audio.done` 没收到（消息拆分丢失），解码器不会销毁，下一轮对话会复用旧解码器。

**解决**：
- 在第一个 `response.output_audio.delta` 创建解码器
- 在 `response.output_audio.done` 销毁解码器
- WebSocket 断开时也销毁解码器
- 兼容 `response.audio.delta` / `response.audio.done`（不带 `output_` 前缀的新版事件名）
