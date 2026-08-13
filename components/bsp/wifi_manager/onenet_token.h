/**
 * @file onenet_token.h
 * @brief OneNET 平台设备 Token 生成接口（Base64 + HMAC + Token 组装）
 *
 * 本模块提供 OneNET 物联网平台设备认证 Token 的生成功能。
 * Token 是设备接入 OneNET 平台的凭证，包含资源路径、过期时间、签名等信息。
 *
 * 核心功能：
 *   - Base64 编解码（支持标准、转义、无换行三种模式）
 *   - HMAC 签名计算（MD5 / SHA1 / SHA256）
 *   - OneNET 设备 Token 生成（遵循 OneNET 安全鉴权协议）
 *
 * Token 格式（根据 OneNET 协议）：
 *   version=2018-10-31&res=products/{pid}/devices/{device}&et={exp}&method={sig}&sign={sign_base64}
 *
 * 签名算法：
 *   1. 对 access_key 进行 Base64 解码得到密钥
 *   2. 构造待签名字符串："{exp}\n{method}\nproducts/{pid}/devices/{device}\n2018-10-31"
 *   3. 使用 HMAC（密钥 + 待签名字符串）计算签名
 *   4. 对签名结果进行 Base64 编码，URL 转义后拼接到 Token
 *
 * 依赖：
 *   - mbedtls（MD5/SHA1/SHA256 HMAC 计算）
 */

#ifndef _ONENET_TOKEN_H_
#define _ONENET_TOKEN_H_
#include <stdint.h>

/* ======================== 类型定义 =============================================== */

/** Base64 编码行结束模式 */
enum Escaped {
    WC_STD_ENC = 0,       /**< 标准模式：使用 \n 换行（PEM 格式） */
    WC_ESC_NL_ENC,        /**< 转义模式：使用 %0A 转义换行（URL 安全） */
    WC_NO_NL_ENC          /**< 无换行模式：不添加任何换行符 */
};

#ifndef byte
typedef unsigned char  byte;
#endif

typedef unsigned short word16;
typedef unsigned int   word32;
typedef byte           word24[3];

/* ======================== Base64 编解码 API ====================================== */

/**
 * @brief Base64 解码
 *
 * @param in     输入的 Base64 编码字符串
 * @param inLen  输入长度
 * @param out    输出缓冲区（存放解码后的二进制数据）
 * @param outLen 输入时为输出缓冲区大小，输出时为实际解码长度
 * @return 0      成功
 *         -173  输出缓冲区不足
 *         -154  输入数据格式错误
 */
int Base64_Decode(const byte* in, word32 inLen, byte* out, word32* outLen);

/**
 * @brief Base64 编码（PEM 标准格式，每 64 字符插入 \n）
 *
 * @param in     输入的二进制数据
 * @param inLen  输入长度
 * @param out    输出缓冲区
 * @param outLen 输入时为输出缓冲区大小，输出时为实际编码长度
 * @return 0 成功
 *         其他值 失败
 */
int Base64_Encode(const byte* in, word32 inLen, byte* out, word32* outLen);

/**
 * @brief Base64 编码（转义换行模式，用 %0A 替代 \n）
 *
 * 用于 URL 参数场景，避免 \n 导致 URL 解析错误。
 *
 * @param in     输入的二进制数据
 * @param inLen  输入长度
 * @param out    输出缓冲区
 * @param outLen 输入时为输出缓冲区大小，输出时为实际编码长度
 * @return 0 成功
 */
int Base64_EncodeEsc(const byte* in, word32 inLen, byte* out, word32* outLen);

/**
 * @brief Base64 编码（无换行模式，所有字符连续输出）
 *
 * 用于 Token 签名编码，不需要换行符。
 *
 * @param in     输入的二进制数据
 * @param inLen  输入长度
 * @param out    输出缓冲区
 * @param outLen 输入时为输出缓冲区大小，输出时为实际编码长度
 * @return 0 成功
 */
int Base64_Encode_NoNl(const byte* in, word32 inLen, byte* out, word32* outLen);

/* ======================== 签名方法枚举 =========================================== */

/**
 * @brief HMAC 签名方法
 *
 * OneNET 平台支持三种签名算法，设备端根据平台配置选择对应方法。
 */
enum sig_method_e
{
    SIG_METHOD_MD5,     /**< MD5 签名（输出 16 字节） */
    SIG_METHOD_SHA1,    /**< SHA1 签名（输出 20 字节） */
    SIG_METHOD_SHA256   /**< SHA256 签名（输出 32 字节） */
};

/* ======================== Token 生成 API ========================================= */

/**
 * @brief 生成 OneNET 设备认证 Token
 *
 * 按 OneNET 安全鉴权协议生成设备 Token，格式如下：
 *   version=2018-10-31&res=products/{pid}/devices/{device}&et={exp}&method={sig}&sign={sign}
 *
 * 生成流程：
 *   1. 拼接 Token 前缀（version + res + et + method）
 *   2. 对 access_key 进行 Base64 解码得到密钥
 *   3. 构造待签名字符串
 *   4. 使用 HMAC 计算签名
 *   5. 对签名结果进行 Base64 编码
 *   6. URL 转义后拼接到 Token
 *
 * 注意：调用方需确保 token 缓冲区足够大（建议 >= 512 字节）。
 *
 * @param token       输出缓冲区（存放生成的 Token 字符串）
 * @param method      签名方法（MD5 / SHA1 / SHA256）
 * @param exp_time    Token 过期时间（Unix 时间戳）
 * @param product_id  OneNET 产品 ID
 * @param dev_name    设备名称（可为 NULL，表示产品级 Token）
 * @param access_key  OneNET 设备 Access Key（Base64 编码的密钥）
 * @return 0 成功
 */
int32_t dev_token_generate(char* token, enum sig_method_e method,
                        uint32_t exp_time, const char* product_id,
                        const char* dev_name, const char* access_key);

#endif