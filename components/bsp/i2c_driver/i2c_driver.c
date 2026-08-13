/**
 * @file i2c_driver.c
 * @brief I2C 总线驱动实现
 *
 * 本模块封装 ESP32 的 I2C 主机驱动，提供统一的 I2C 总线初始化和设备操作接口。
 * 主要用于 OLED 显示屏（SSD1306）等 I2C 外设的通信。
 *
 * 核心流程：
 *   1. 初始化总线：配置 GPIO、频率、内部上拉，创建 I2C 主机实例
 *   2. 添加设备：在总线上注册从设备（7-bit 地址），获取设备句柄
 *   3. 读写操作：通过设备句柄进行寄存器读写（单字节/多字节）
 *   4. 清理：移除设备 → 删除总线，释放所有资源
 *
 * I2C 通信协议说明：
 *   - 写寄存器：发送 [设备地址+W] [寄存器地址] [数据...]
 *   - 读寄存器：发送 [设备地址+W] [寄存器地址] → 重复起始 → [设备地址+R] [数据...]
 *   - 使用重复起始条件（repeated start）实现读操作，避免总线释放冲突
 *
 * 硬件连接：
 *   - SDA: GPIO11（数据线，需外部上拉或使能内部上拉）
 *   - SCL: GPIO12（时钟线，需外部上拉或使能内部上拉）
 *   - 频率: 400kHz（Fast Mode，兼容大多数 I2C 设备）
 *
 * 依赖：
 *   - ESP-IDF I2C Master Driver
 */

#include "i2c_driver.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ======================== 模块静态变量 =========================================== */

/** 全局 I2C 总线句柄，供其他模块通过 I2c_Get_Global_Bus_Handle() 获取 */
static i2c_master_bus_handle_t global_i2c_bus = NULL;

/* ======================== I2C 总线初始化 ========================================= */

/**
 * @brief 初始化 I2C 总线
 *
 * 配置并创建 I2C 主机总线实例，使能内部上拉电阻。
 * 初始化成功后，总线句柄同时保存到全局变量，供其他模块获取。
 *
 * @param port       I2C 端口号（如 I2C_NUM_0）
 * @param sda_pin    SDA 数据线 GPIO 编号
 * @param scl_pin    SCL 时钟线 GPIO 编号
 * @param freq_hz    I2C 时钟频率（Hz），当前未使用（频率在添加设备时指定）
 * @param bus_handle [输出] I2C 总线句柄指针
 *
 * @return ESP_OK        初始化成功
 *         ESP_ERR_NO_MEM 内存不足
 *         ESP_FAIL       初始化失败
 */
esp_err_t I2c_Init_Bus(i2c_port_t port, gpio_num_t sda_pin, gpio_num_t scl_pin, uint32_t freq_hz, i2c_master_bus_handle_t *bus_handle) {
    /* 配置 I2C 主机总线参数 */
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,   /* 使用默认时钟源 */
        .i2c_port = port,                     /* I2C 端口号 */
        .sda_io_num = sda_pin,                /* SDA 数据线 GPIO */
        .scl_io_num = scl_pin,                /* SCL 时钟线 GPIO */
        .flags = {
            .enable_internal_pullup = true,   /* 使能内部上拉电阻 */
        }
    };

    /* 创建 I2C 主机总线实例 */
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, bus_handle);
    if (ret == ESP_OK && bus_handle != NULL) {
        /* 保存全局句柄，供其他模块获取 */
        global_i2c_bus = *bus_handle;
    }
    return ret;
}

/* ======================== I2C 设备管理 =========================================== */

/**
 * @brief 向 I2C 总线添加设备
 *
 * 在已初始化的 I2C 总线上注册一个从设备。
 * 配置 7-bit 地址和通信频率，获取设备句柄用于后续读写。
 *
 * @param bus_handle I2C 总线句柄
 * @param dev_addr   设备 7-bit 地址
 * @param freq_hz    设备通信频率（Hz）
 * @param dev_handle [输出] 设备句柄指针
 *
 * @return ESP_OK 添加成功
 */
esp_err_t I2c_Add_Device(i2c_master_bus_handle_t bus_handle, uint16_t dev_addr, uint32_t freq_hz, i2c_master_dev_handle_t *dev_handle) {
    /* 配置设备参数 */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,  /* 7-bit 地址模式 */
        .device_address = dev_addr,              /* 设备地址 */
        .scl_speed_hz = freq_hz,                 /* 通信频率 */
    };
    return i2c_master_bus_add_device(bus_handle, &dev_cfg, dev_handle);
}

/**
 * @brief 从 I2C 总线移除设备
 *
 * 释放设备句柄，设备不再可用。
 *
 * @param dev_handle 设备句柄
 * @return ESP_OK 移除成功
 */
esp_err_t I2c_Remove_Device(i2c_master_dev_handle_t dev_handle) {
    return i2c_master_bus_rm_device(dev_handle);
}

/**
 * @brief 删除 I2C 总线
 *
 * 释放总线句柄和所有相关资源（GPIO、内存等）。
 * 删除前需确保已移除总线上的所有设备。
 *
 * @param bus_handle I2C 总线句柄
 * @return ESP_OK 删除成功
 */
esp_err_t I2c_Delete_Bus(i2c_master_bus_handle_t bus_handle) {
    return i2c_del_master_bus(bus_handle);
}

/* ======================== I2C 寄存器读写 ========================================= */

/**
 * @brief 向 I2C 设备寄存器写入单个字节
 *
 * I2C 通信序列：
 *   START → 设备地址(W) → ACK → 寄存器地址 → ACK → 数据 → ACK → STOP
 *
 * @param dev_handle 设备句柄
 * @param reg        寄存器地址
 * @param data       要写入的数据
 *
 * @return ESP_OK 写入成功
 *         ESP_FAIL 写入失败（设备无应答或总线错误）
 */
esp_err_t I2c_Write_Reg(i2c_master_dev_handle_t dev_handle, uint8_t reg, uint8_t data) {
    uint8_t buf[2] = {reg, data};   /* 发送格式：[寄存器地址, 数据] */
    return i2c_master_transmit(dev_handle, buf, sizeof(buf), -1);
}

/**
 * @brief 从 I2C 设备寄存器读取单个字节
 *
 * I2C 通信序列：
 *   START → 设备地址(W) → ACK → 寄存器地址 → ACK →
 *   重复起始 → 设备地址(R) → ACK → 数据(1字节) → NACK → STOP
 *
 * @param dev_handle 设备句柄
 * @param reg        寄存器地址
 * @param data       [输出] 读取到的数据指针
 *
 * @return ESP_OK 读取成功
 *         ESP_FAIL 读取失败
 */
esp_err_t I2c_Read_Reg(i2c_master_dev_handle_t dev_handle, uint8_t reg, uint8_t *data) {
    return i2c_master_transmit_receive(dev_handle, &reg, 1, data, 1, -1);
}

/**
 * @brief 从 I2C 设备寄存器读取多个字节
 *
 * 先发送寄存器地址，再连续接收 count 字节数据。
 * 适用于读取 FIFO 或连续寄存器区域。
 *
 * I2C 通信序列：
 *   START → 设备地址(W) → ACK → 寄存器地址 → ACK →
 *   重复起始 → 设备地址(R) → ACK → 数据1...数据N → NACK → STOP
 *
 * @param dev_handle 设备句柄
 * @param reg        寄存器起始地址
 * @param buffer     [输出] 接收缓冲区
 * @param count      要读取的字节数
 *
 * @return ESP_OK 读取成功
 *         ESP_FAIL 读取失败
 */
esp_err_t I2c_Read_Bytes(i2c_master_dev_handle_t dev_handle, uint8_t reg, uint8_t *buffer, size_t count) {
    return i2c_master_transmit_receive(dev_handle, &reg, 1, buffer, count, -1);
}

/**
 * @brief 向 I2C 设备寄存器写入多个字节
 *
 * 发送格式：[寄存器地址, 数据1, 数据2, ...]，共 count+1 字节。
 * 内部使用临时缓冲区拼接寄存器地址和数据，避免修改原始数据。
 *
 * 适用于 OLED 等需要连续写入命令/数据的设备。
 *
 * I2C 通信序列：
 *   START → 设备地址(W) → ACK → 寄存器地址 → ACK → 数据1...数据N → ACK → STOP
 *
 * @param dev_handle 设备句柄
 * @param reg        寄存器地址
 * @param buffer     要写入的数据缓冲区
 * @param count      要写入的字节数
 *
 * @return ESP_OK        写入成功
 *         ESP_ERR_NO_MEM 内存不足
 *         ESP_FAIL       写入失败
 */
esp_err_t I2c_Write_Bytes(i2c_master_dev_handle_t dev_handle, uint8_t reg, uint8_t *buffer, size_t count) {
    /*
     * 分配临时缓冲区：寄存器地址(1) + 数据(count) 字节。
     * 使用 malloc 分配是因为 count 可能较大（如 OLED 整帧数据），
     * 栈上分配可能溢出。
     */
    uint8_t *temp_buf = (uint8_t *)malloc(count + 1);
    if (temp_buf == NULL) return ESP_ERR_NO_MEM;

    /* 拼接：第一个字节为寄存器地址，后续为数据 */
    temp_buf[0] = reg;
    memcpy(&temp_buf[1], buffer, count);

    /* 发送连续数据块 */
    esp_err_t ret = i2c_master_transmit(dev_handle, temp_buf, count + 1, -1);
    free(temp_buf);
    return ret;
}

/* ======================== 全局总线访问 =========================================== */

/**
 * @brief 获取全局 I2C 总线句柄
 *
 * 返回 I2c_Init_Bus 保存的全局总线句柄。
 * 其他模块（如 OLED 驱动）可直接通过此函数获取总线引用，
 * 无需通过参数传递。
 *
 * @return 全局 I2C 总线句柄（未初始化时返回 NULL）
 */
i2c_master_bus_handle_t I2c_Get_Global_Bus_Handle(void) {
    return global_i2c_bus;
}