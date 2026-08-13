# INMP441 数字 MEMS 麦克风使用教程

## 模块概述

INMP441 是一款高性能、低功耗的数字 MEMS 麦克风，采用 I²S 接口输出 24 位音频数据。本项目使用 INMP441 作为语音输入模块，通过 ESP32-S3 的 I²S 接口接收音频数据，用于语音识别、录音等场景。

## 技术规格

| 参数 | 规格 |
|------|------|
| 类型 | 数字 MEMS 麦克风（底部收音） |
| 输出接口 | I²S（从模式） |
| 数据格式 | 24 位补码，MSB 优先 |
| 采样率 | 由 SCK 决定，典型 16kHz ~ 48kHz |
| SCK 频率 | 0.5 MHz ~ 3.2 MHz |
| 灵敏度 | -26 dBFS @ 1kHz, 94dB SPL |
| 信噪比 (SNR) | 61 dBA |
| 功耗 | 正常模式 1.4mA，待机 < 0.8mA，关断 < 2μA |
| 工作电压 | 1.8V ~ 3.3V |
| 封装 | LGA 9 引脚（底部声孔） |

## 引脚定义

| 引脚 | 名称 | 功能 | ESP32-S3 引脚 | 说明 |
|------|------|------|--------------|------|
| 1 | SCK | I²S 串行时钟输入 | GPIO7 | 由 MCU 提供 |
| 2 | SD | I²S 串行数据输出 | GPIO6 | 三态输出，需接下拉电阻 |
| 3 | WS | I²S 字选择（帧时钟） | GPIO5 | 由 MCU 提供 |
| 4 | L/R | 左右声道选择 | GND | 接地=左声道，接 VDD=右声道 |
| 5,6,9 | GND | 地 | GND | 全部接地 |
| 7 | VDD | 电源 | 3.3V | 必须加 0.1μF 去耦电容 |
| 8 | CHIPEN | 芯片使能 | VDD | 高电平工作，本项目硬接 VDD |

## 硬件连接

### 接线图

```
INMP441                    ESP32-S3
┌──────────────┐            ┌──────────────┐
│ VDD (Pin 7)  │───────────▶│ 3.3V         │
│ GND (Pin 5,6,9)│──────────▶│ GND          │
│ SCK (Pin 1)  │───────────▶│ GPIO7 (BCLK)  │
│ WS  (Pin 3)  │───────────▶│ GPIO5 (LRCK)  │
│ SD  (Pin 2)  │───────────▶│ GPIO6 (DOUT)  │
│ L/R (Pin 4)  │───────────▶│ GND（左声道）  │
│ CHIPEN(Pin 8)│───────────▶│ 3.3V（硬接）   │
└──────────────┘            └──────────────┘
```

### 硬件注意事项

1. **电源去耦**：VDD 与 GND 之间必须放置 **0.1 μF 陶瓷电容**（X7R），尽量靠近芯片引脚
2. **SD 下拉电阻**：SD 数据线上建议接 **100 kΩ 下拉电阻**到 GND，防止麦克风三态时浮空。代码中已启用 ESP32 内部下拉作为软件补偿
3. **L/R 引脚**：必须接地或接 VDD，**绝对不能悬空**。本项目 L/R 接地（左声道），但 I²S 驱动配置为读取右声道（ESP32 声道映射特殊）
4. **CHIPEN 引脚**：本项目硬接 VDD（常开），不可控。如果 MCU 引脚控制 CHIPEN，需按指定时序操作
5. **声孔**：PCB 底部开孔必须对齐麦克风底部声孔，直径 0.5mm ~ 1mm

## 驱动架构

本项目的 INMP441 驱动分为两层：

```
┌─────────────────────────────────────────┐
│  microphone.c / microphone.h            │  ← 应用层：麦克风硬件控制、PCM 转换
│  - Microphone_Init() / Deinit()         │
│  - Microphone_Read_Pcm16()              │
│  - INMP441 使能、SD 下拉、时序控制       │
├─────────────────────────────────────────┤
│  i2s_driver.c / i2s_driver.h            │  ← 驱动层：I²S 总线配置、数据读写
│  - I2s_Rx_Init() / I2s_Rx_Read()        │
│  - I2S 时钟、槽位、DMA 配置             │
└─────────────────────────────────────────┘
```

### 关键配置文件

| 文件 | 内容 |
|------|------|
| `microphone.h` | 麦克风采样率、位深、CHIPEN GPIO 配置 |
| `microphone.c` | 麦克风初始化流程、PCM 数据读取 |
| `i2s_driver.h` | I²S GPIO 引脚、槽位掩码、DMA 参数 |
| `i2s_driver.c` | I²S 驱动初始化、数据读写 |

## I²S 配置参数

```c
// i2s_driver.h 中的关键配置
#define I2S_RX_BCLK_GPIO      7     // 位时钟引脚
#define I2S_RX_LRCK_GPIO      5     // 帧时钟引脚
#define I2S_RX_DOUT_GPIO      6     // 数据输入引脚
#define I2S_RX_SAMPLE_RATE    16000 // 采样率 16kHz
#define I2S_RX_BIT_DEPTH      32    // 每个槽位 32 位
#define I2S_RX_DMA_BUF_COUNT  8     // DMA 缓冲区数量
#define I2S_RX_DMA_BUF_LEN    256   // 每个 DMA 缓冲区帧数
#define I2S_RX_SLOT_MASK      I2S_STD_SLOT_RIGHT  // 读取右声道
```

### ⚠️ 关键配置说明

| 配置项 | 值 | 必须这样设置的原因 |
|--------|-----|-------------------|
| `bit_shift` | `false` | `true` 会导致位偏移，数据全零 |
| `ws_pol` | `true` | WS 极性需反转才能匹配 INMP441 |
| `slot_mask` | `I2S_STD_SLOT_RIGHT` | 虽然 L/R 接地（左声道），但 ESP32 声道映射特殊，需读右声道 |
| `slot_mode` | `I2S_SLOT_MODE_STEREO` | 必须用立体声模式，INMP441 需要 64 个 SCK/帧 |

完整 I²S 初始化代码：

```c
// i2s_driver.c - I2s_Rx_Init()
esp_err_t I2s_Rx_Init(void)
{
    i2s_chan_config_t chanConfig = {
        .id = I2S_NUM_0,           
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = I2S_RX_DMA_BUF_COUNT,
        .dma_frame_num = I2S_RX_DMA_BUF_LEN,
        .auto_clear = false,
    };

    i2s_new_channel(&chanConfig, NULL, &Rx_Handle);

    i2s_std_config_t stdConfig = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT,
            I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .bclk = I2S_RX_BCLK_GPIO,
            .ws   = I2S_RX_LRCK_GPIO,
            .din  = I2S_RX_DOUT_GPIO,
            // ...
        },
    };
    // 关键！必须手动覆盖这两个参数
    stdConfig.slot_cfg.slot_mask = I2S_STD_SLOT_RIGHT;
    stdConfig.slot_cfg.bit_shift = false;  // 关键！true 会导致数据全零
    stdConfig.slot_cfg.ws_pol = true;      // 关键！WS 极性反转

    i2s_channel_init_std_mode(Rx_Handle, &stdConfig);
    i2s_channel_enable(Rx_Handle);
}
```

## 初始化流程

INMP441 的初始化**必须严格按以下顺序**执行，否则麦克风不会输出数据：

```
1. 配置 SD 引脚下拉（防止浮空）
       │
2. 启动 I²S 时钟（SCK + WS）
       │
3. 重新配置 SD 引脚下拉（I²S 初始化可能覆盖）
       │
4. 使能 CHIPEN / 等待恢复
       │
5. 等待稳定（≥ 100ms）
       │
6. 开始读取数据
```

### 时序要求

| 场景 | 等待时间 | 原因 |
|------|---------|------|
| VDD 首次上电 → 使能 | 2¹⁸ 个 SCK 周期 | 内部电路稳定 |
| 关断唤醒（CHIPEN 低→高） | 2¹⁷ 个 SCK 周期 | 数据路径初始化 |
| 待机恢复（SCK 停止→启动） | 2¹⁴ 个 SCK 周期 | 时钟恢复同步 |
| 本项目（CHIPEN 硬接 VDD） | 100ms | SCK=1.024MHz，2¹⁴ 周期 ≈ 16ms，留足余量 |

完整初始化代码：

```c
// microphone.c - Microphone_Init()
esp_err_t Microphone_Init(void)
{
    // 步骤 1：先配置 SD 引脚下拉（防止 I2S 接管前浮空）
    gpio_set_pull_mode(I2S_RX_DOUT_GPIO, GPIO_PULLDOWN_ONLY);

    // 步骤 2：启动 I2S 时钟（SCK/WS 必须在 CHIPEN 前运行）
    I2s_Rx_Init();

    // 步骤 3：重新配置下拉（I2S 初始化可能覆盖 GPIO 设置）
    gpio_set_pull_mode(I2S_RX_DOUT_GPIO, GPIO_PULLDOWN_ONLY);

    // 步骤 4：使能麦克风（CHIPEN 硬接 VDD 则只需等待恢复）
    // 等待 100ms 让麦克风从待机恢复
    vTaskDelay(pdMS_TO_TICKS(100));

    return ESP_OK;
}
```

## 数据格式

### I²S 帧结构

INMP441 输出数据遵循 I²S 标准格式，每个 WS 帧固定包含 64 个 SCK 周期：

```
WS    ─┐           ┌───────────────────────────┐
       └───────────┘                           └──────
           ← 左声道 32 SCK →← 右声道 32 SCK →
SCK    ─┐┌─┐┌─┐ ... ┌─┐┌─┐┌─┐┌─┐ ... ┌─┐┌─┐┌─
SD     ──┘└─┘└─ ... └─┘└─┘└─┘└─┘ ... └─┘└─┘└─
           ← 24位数据 →← 8位空 →← 24位数据 →← 8位空 →
```

- 每个声道占 32 个 SCK 周期
- 前 24 个 SCK：有效音频数据（MSB 优先，补码格式）
- 后 8 个 SCK：填充位（SD 三态，下拉到 GND = 0）
- INMP441 只在配置的声道（L/R 选择）输出数据，另一声道 SD 三态

### 32 位数据 → 16 位 PCM 转换

ESP32 读取到的是 32 位值，其中高 24 位是音频数据，低 8 位为 0。提取 16 位 PCM 的方法是**右移 16 位取高 16 位**：

```c
// 32 位原始数据
// ┌────────────── 24位音频数据 ──────────────┐┌─ 8位0 ─┐
//   b31  b30  ...  b16  b15  ...  b8  b7  ...  b0
//  ┌──────────────────┐
//  ← 取高 16 位作为 PCM →

int32_t rawData;    // 从 I²S 读取的 32 位原始值
int16_t pcmData = (int16_t)(rawData >> 16);  // 右移 16 位得到 16 位 PCM
```

### 示例数据

```
原始 32 位值          十进制        右移 16 位后 PCM
──────────────────────────────────────────────────
0x01DCE000         31,252,480    →  0x01DC (476)
0x01D8A000         30,973,952    →  0x01D8 (472)
0x00000000         0             →  0x0000 (0, 静音)
```

## 使用示例

### 基本录音流程

```c
#include "microphone.h"

void Record_Example(void)
{
    // 1. 初始化麦克风
    Microphone_Init();

    // 2. 分配 PCM 缓冲区
    int16_t *pcmBuffer = malloc(16000 * sizeof(int16_t));  // 1 秒 @ 16kHz

    // 3. 读取 PCM 数据
    size_t samplesRead = 0;
    Microphone_Read_Pcm16(pcmBuffer, 16000, &samplesRead, portMAX_DELAY);

    // 4. 处理 PCM 数据...
    // pcmBuffer 中存放 16 位有符号 PCM，范围 -32768 ~ 32767

    // 5. 释放资源
    free(pcmBuffer);
    Microphone_Deinit();
}
```

### 完整录音+播放测试

```c
#define RECORD_SAMPLE_RATE    16000
#define RECORD_DURATION_SEC   10
#define RECORD_TOTAL_SAMPLES  (RECORD_SAMPLE_RATE * RECORD_DURATION_SEC)

static int16_t *Record_Buffer = NULL;

void Record_Playback_Test(void)
{
    // 1. 初始化麦克风
    Microphone_Init();

    // 2. 初始化放大器（用于播放）
    Amplifier_Init();

    // 3. 分配录音缓冲区
    Record_Buffer = heap_caps_malloc(
        RECORD_TOTAL_SAMPLES * sizeof(int16_t),
        MALLOC_CAP_SPIRAM);

    // 4. 录音
    size_t samplesRead = 0;
    Microphone_Read_Pcm16(Record_Buffer, RECORD_TOTAL_SAMPLES,
                          &samplesRead, portMAX_DELAY);

    // 5. 播放录音
    size_t bytesWritten = 0;
    I2s_Tx_Write((uint8_t *)Record_Buffer,
                 samplesRead * sizeof(int16_t),
                 &bytesWritten, portMAX_DELAY);

    // 6. 清理
    heap_caps_free(Record_Buffer);
    Amplifier_Deinit();
    Microphone_Deinit();
}
```

## API 参考

### Microphone_Init()

初始化 INMP441 麦克风，包括 SD 下拉、I²S 时钟启动、使能等待。

```c
esp_err_t Microphone_Init(void);
```

- **返回值**：`ESP_OK` 成功，否则失败
- **注意**：初始化需要约 100ms，内部会自动等待麦克风稳定

### Microphone_Deinit()

关闭麦克风，释放 I²S 资源。

```c
esp_err_t Microphone_Deinit(void);
```

### Microphone_Read_Pcm16()

读取指定数量的 16 位 PCM 样本。

```c
esp_err_t Microphone_Read_Pcm16(
    int16_t *pcmBuffer,    // [out] PCM 数据缓冲区
    size_t sampleCount,    // [in]  要读取的样本数
    size_t *samplesRead,   // [out] 实际读取的样本数
    uint32_t timeout       // [in]  超时时间（RTOS tick）
);
```

- **返回值**：`ESP_OK` 成功，`ESP_FAIL` 表示未读到任何数据
- **pcmBuffer** 需预先分配至少 `sampleCount * sizeof(int16_t)` 字节
- 数据为有符号 16 位补码，范围 -32768 ~ 32767

### Microphone_Read_Raw()

读取原始 32 位 I²S 数据（不转换）。

```c
esp_err_t Microphone_Read_Raw(
    uint8_t *buffer,       // [out] 原始数据缓冲区
    size_t size,           // [in]  要读取的字节数
    size_t *bytes_read,    // [out] 实际读取的字节数
    uint32_t timeout       // [in]  超时时间
);
```

## I²S 双端口共存说明

本项目同时使用两个 I²S 端口：

| 端口 | 方向 | 用途 | 设备 |
|------|------|------|------|
| I2S_NUM_0 | RX | 录音 | INMP441 麦克风 |
| I2S_NUM_1 | TX | 播放 | NS4168 放大器 |

### ⚠️ 重要限制

1. **禁止混用新旧驱动**：ESP-IDF v5.x 不允许 `driver/i2s.h`（legacy）和 `driver/i2s_std.h`（新版）同时使用，必须统一使用新版驱动
3. **TX 和 RX 可以同时运行**：两个端口独立，互不干扰

## 常见问题排查

| 现象 | 可能原因 | 解决办法 |
|------|---------|---------|
| 所有采样值为 0x00000000 | SD 引脚浮空或下拉未生效 | 检查外部 100kΩ 下拉电阻；确认 `gpio_set_pull_mode()` 在 I²S 初始化后再次调用 |
| 所有采样值为 0x00000001 | `bit_shift=true` 导致位偏移 | 设置 `bit_shift = false` |
| 数据全是非零但 PCM 值极小 | `ws_pol` 极性错误 | 设置 `ws_pol = true` |
| 系统启动崩溃 | 新旧 I²S 驱动混用 | 统一使用新版 `driver/i2s_std.h` |
| 初始化顺序错误 | SCK 未启动就使能 CHIPEN | 先调 `I2s_Rx_Init()` 再使能麦克风 |
| L/R 引脚悬空 | 声道选择不确定 | 接地或接 VDD，不能悬空 |

## 参考文档

- INMP441 数据手册：`datasheet/INMP441.pdf`
- 厂家测试代码：`datasheet/INMP441/esp32-i2s-mems-master/`
- 数据手册概念指南：本文档（覆盖了旧版概念性内容，新增实战代码和填坑记录）