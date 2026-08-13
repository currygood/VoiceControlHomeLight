/**
 * @file i2c_driver.h
 * @brief I2C 总线驱动接口
 *
 * 本模块封装 ESP32 的 I2C 主机驱动，提供统一的 I2C 总线初始化和设备操作接口。
 * 主要用于 OLED 显示屏等 I2C 外设的通信。
 *
 * 核心功能：
 *   - I2C 总线初始化与销毁
 *   - I2C 设备添加与移除
 *   - 寄存器读写（单字节/多字节）
 *
 * 硬件配置：
 *   - GPIO: SDA=11, SCL=12
 *   - 频率: 400kHz（标准 Fast Mode）
 *   - 内部上拉电阻已使能
 *
 * 依赖：
 *   - ESP-IDF I2C Master Driver
 */

#ifndef I2C_DRIVER_H
#define I2C_DRIVER_H

#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

/* ======================== I2C 硬件配置宏 ========================================= */

/** I2C 端口号：使用 I2C_NUM_0 */
#define I2C_PORT I2C_NUM_0

/** I2C SDA 数据线 GPIO（连接到 I2C 设备的 SDA 引脚） */
#define I2C_SDA_GPIO 11

/** I2C SCL 时钟线 GPIO（连接到 I2C 设备的 SCL 引脚） */
#define I2C_SCL_GPIO 12

/** I2C 时钟频率（Hz）：400kHz Fast Mode */
#define I2C_FREQ 400000

/* ======================== 类型定义 =============================================== */

/**
 * @brief I2C 设备句柄结构体
 *
 * 封装 I2C 总线句柄和设备句柄，用于后续的读写操作。
 * 总线句柄对应物理 I2C 控制器，设备句柄对应总线上的具体从设备。
 */
typedef struct {
    i2c_master_bus_handle_t bus;  /**< I2C 总线句柄 */
    i2c_master_dev_handle_t dev;  /**< I2C 设备句柄 */
} i2c_handle_t;

/* ======================== API 函数声明 =========================================== */

/**
 * @brief 初始化 I2C 总线
 *
 * 配置并创建 I2C 主机总线实例，使能内部上拉电阻。
 * 初始化成功后可通过返回的 bus_handle 添加设备。
 *
 * @param port       I2C 端口号（如 I2C_NUM_0）
 * @param sda_pin    SDA 数据线 GPIO 编号
 * @param scl_pin    SCL 时钟线 GPIO 编号
 * @param freq_hz    I2C 时钟频率（Hz）
 * @param bus_handle [输出] I2C 总线句柄指针
 *
 * @return ESP_OK        初始化成功
 *         ESP_ERR_NO_MEM 内存不足
 *         ESP_FAIL       初始化失败
 */
esp_err_t I2c_Init_Bus(i2c_port_t port, gpio_num_t sda_pin, gpio_num_t scl_pin, uint32_t freq_hz, i2c_master_bus_handle_t *bus_handle);

/**
 * @brief 向 I2C 总线添加设备
 *
 * 在已初始化的 I2C 总线上注册一个从设备，获取设备句柄用于后续读写。
 *
 * @param bus_handle I2C 总线句柄
 * @param dev_addr   设备 7-bit 地址
 * @param freq_hz    设备通信频率（Hz）
 * @param dev_handle [输出] 设备句柄指针
 *
 * @return ESP_OK        添加成功
 *         ESP_ERR_NO_MEM 内存不足
 *         ESP_FAIL       添加失败
 */
esp_err_t I2c_Add_Device(i2c_master_bus_handle_t bus_handle, uint16_t dev_addr, uint32_t freq_hz, i2c_master_dev_handle_t *dev_handle);

/**
 * @brief 向 I2C 设备寄存器写入单个字节
 *
 * 发送格式：[寄存器地址, 数据]，共 2 字节。
 *
 * @param dev_handle 设备句柄
 * @param reg        寄存器地址
 * @param data       要写入的数据
 *
 * @return ESP_OK 写入成功
 *         ESP_FAIL 写入失败
 */
esp_err_t I2c_Write_Reg(i2c_master_dev_handle_t dev_handle, uint8_t reg, uint8_t data);

/**
 * @brief 从 I2C 设备寄存器读取单个字节
 *
 * 先发送寄存器地址，再接收 1 字节数据，使用重复起始条件（repeated start）。
 *
 * @param dev_handle 设备句柄
 * @param reg        寄存器地址
 * @param data       [输出] 读取到的数据指针
 *
 * @return ESP_OK 读取成功
 *         ESP_FAIL 读取失败
 */
esp_err_t I2c_Read_Reg(i2c_master_dev_handle_t dev_handle, uint8_t reg, uint8_t *data);

/**
 * @brief 从 I2C 设备寄存器读取多个字节
 *
 * 先发送寄存器地址，再连续接收 count 字节数据，使用重复起始条件。
 * 适用于读取 FIFO 或连续寄存器区域。
 *
 * @param dev_handle 设备句柄
 * @param reg        寄存器起始地址
 * @param buffer     [输出] 接收缓冲区
 * @param count      要读取的字节数
 *
 * @return ESP_OK 读取成功
 *         ESP_FAIL 读取失败
 */
esp_err_t I2c_Read_Bytes(i2c_master_dev_handle_t dev_handle, uint8_t reg, uint8_t *buffer, size_t count);

/**
 * @brief 向 I2C 设备寄存器写入多个字节
 *
 * 发送格式：[寄存器地址, 数据1, 数据2, ...]，共 count+1 字节。
 * 适用于 OLED 等需要连续写入数据的设备。
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
esp_err_t I2c_Write_Bytes(i2c_master_dev_handle_t dev_handle, uint8_t reg, uint8_t *buffer, size_t count);

/**
 * @brief 从 I2C 总线移除设备
 *
 * 释放设备句柄，设备不再可用。
 *
 * @param dev_handle 设备句柄
 * @return ESP_OK 移除成功
 */
esp_err_t I2c_Remove_Device(i2c_master_dev_handle_t dev_handle);

/**
 * @brief 删除 I2C 总线
 *
 * 释放总线句柄和所有相关资源（GPIO、内存等）。
 * 删除前需先移除总线上的所有设备。
 *
 * @param bus_handle I2C 总线句柄
 * @return ESP_OK 删除成功
 */
esp_err_t I2c_Delete_Bus(i2c_master_bus_handle_t bus_handle);

/**
 * @brief 获取全局 I2C 总线句柄
 *
 * 返回 I2c_Init_Bus 保存的全局总线句柄，供其他模块获取 I2C 总线引用。
 *
 * @return 全局 I2C 总线句柄（未初始化时返回 NULL）
 */
i2c_master_bus_handle_t I2c_Get_Global_Bus_Handle(void);

#endif // I2C_DRIVER_H