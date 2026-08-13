/**
 * @file onenet_token.c
 * @brief OneNET 平台设备 Token 生成实现（Base64 + HMAC + Token 组装）
 *
 * 本模块实现 OneNET 物联网平台设备认证 Token 的生成。
 *
 * 核心流程（dev_token_generate）：
 *   1. 拼接 Token 前缀（version + 资源路径 + 过期时间 + 签名方法）
 *   2. 对 access_key 进行 Base64 解码得到 HMAC 密钥
 *   3. 构造待签名字符串（按 OneNET 协议格式）
 *   4. 使用 HMAC 计算签名（支持 MD5 / SHA1 / SHA256）
 *   5. 对签名结果进行 Base64 编码（无换行模式）
 *   6. 对 Base64 结果中的特殊字符进行 URL 转义
 *   7. 拼接到 Token 末尾
 *
 * Base64 实现说明：
 *   本模块包含完整的 Base64 编解码实现，支持三种行结束模式：
 *   - WC_STD_ENC：标准 PEM 格式（每 64 字符插入 \n）
 *   - WC_ESC_NL_ENC：URL 安全模式（用 %0A 转义换行符）
 *   - WC_NO_NL_ENC：无换行模式（Token 签名编码使用）
 *
 * 依赖：
 *   - mbedtls（MD5/SHA1/SHA256 HMAC 计算）
 */

#include "onenet_token.h"
#include "mbedtls/md5.h"
#include "mbedtls/md.h"
#include <string.h>
#include <stdio.h>

/* ======================== Base64 编码表和解码表 ================================== */

/** 无效编码标记（解码表中使用） */
enum {
    BAD         = 0xFF,  /**< 无效的 Base64 字符 */
    PAD         = '=',   /**< 填充字符 */
    PEM_LINE_SZ = 64     /**< PEM 格式每行最大字符数 */
};

/** Base64 编码表（RFC 4648 标准） */
static
const byte base64Encode[] = { 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J',
                              'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T',
                              'U', 'V', 'W', 'X', 'Y', 'Z',
                              'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j',
                              'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't',
                              'u', 'v', 'w', 'x', 'y', 'z',
                              '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
                              '+', '/'
                            };

/** Base64 解码表（索引偏移 0x2B，即从 '+' 字符开始） */
static
const byte base64Decode[] = { 62, BAD, BAD, BAD, 63,   /* + 从 0x2B 开始 */
                              52, 53, 54, 55, 56, 57, 58, 59, 60, 61,
                              BAD, BAD, BAD, BAD, BAD, BAD, BAD,
                              0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
                              10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
                              20, 21, 22, 23, 24, 25,
                              BAD, BAD, BAD, BAD, BAD, BAD,
                              26, 27, 28, 29, 30, 31, 32, 33, 34, 35,
                              36, 37, 38, 39, 40, 41, 42, 43, 44, 45,
                              46, 47, 48, 49, 50, 51
                            };

/* ======================== Base64 编码辅助函数 ==================================== */

/**
 * @brief 单个字符的编码写入（支持转义和大小计算）
 *
 * 根据编码模式决定是否对特殊字符（+、=、\n）进行 URL 转义。
 * 当 getSzOnly 为真时，仅计算所需大小而不实际写入。
 *
 * @param escaped   编码模式（WC_STD_ENC / WC_ESC_NL_ENC / WC_NO_NL_ENC）
 * @param e         要写入的字符（原始值或编码后的索引）
 * @param out       输出缓冲区
 * @param i         输出位置索引（会被更新）
 * @param max       输出缓冲区最大值
 * @param raw       是否直接写入原始字符（1 = 不编码，0 = 查表编码）
 * @param getSzOnly 是否仅计算大小（1 = 不写入，0 = 正常写入）
 * @return 0 成功
 *         -132 输出缓冲区不足
 */
static int CEscape(int escaped, byte e, byte* out, word32* i, word32 max,
                  int raw, int getSzOnly)
{
    int    doEscape = 0;
    word32 needed = 1;
    word32 idx = *i;

    byte basic;
    byte plus    = 0;
    byte equals  = 0;
    byte newline = 0;

    /* 查表或直接使用原始字符 */
    if (raw)
        basic = e;
    else
        basic = base64Encode[e];

    /* 仅在 WC_ESC_NL_ENC 模式下检查是否需要 URL 转义 */
    if (escaped == WC_ESC_NL_ENC)
    {
        switch ((char)basic)
        {
            case '+' :
                plus     = 1;
                doEscape = 1;
                needed  += 2;   /* %2B 需要 3 个字节 */
                break;
            case '=' :
                equals   = 1;
                doEscape = 1;
                needed  += 2;   /* %3D 需要 3 个字节 */
                break;
            case '\n' :
                newline  = 1;
                doEscape = 1;
                needed  += 2;   /* %0A 需要 3 个字节 */
                break;
            default:
                break;
        }
    }

    /* 检查输出缓冲区是否足够 */
    if ( (idx+needed) > max && !getSzOnly)
    {
        return -132;
    }

    /* 写入字符 */
    if (doEscape == 0)
    {
        if(getSzOnly)
            idx++;
        else
            out[idx++] = basic;
    }
    else
    {
        if(getSzOnly)
            idx+=3;
        else
        {
            out[idx++] = '%';  /* URL 转义起始符 */

            if (plus)
            {
                out[idx++] = '2';
                out[idx++] = 'B';
            }
            else if (equals)
            {
                out[idx++] = '3';
                out[idx++] = 'D';
            }
            else if (newline)
            {
                out[idx++] = '0';
                out[idx++] = 'A';
            }
        }
    }
    *i = idx;

    return 0;
}

/* ======================== Base64 编码核心实现 ==================================== */

/**
 * @brief Base64 编码内部实现（支持三种行结束模式）
 *
 * 编码流程：
 *   - 每 3 个输入字节编码为 4 个 Base64 字符
 *   - 输入不足 3 字节时用 '=' 填充
 *   - 标准模式下每 64 字符插入 \n 换行
 *
 * 当 out 为 NULL 时，仅计算所需大小并写入 outLen（getSzOnly 模式）。
 *
 * @param in      输入的二进制数据
 * @param inLen   输入长度
 * @param out     输出缓冲区（NULL = 仅计算大小）
 * @param outLen  输入时为输出缓冲区大小，输出时为实际编码长度
 * @param escaped 编码模式
 * @return 0      成功
 *         -2     输出缓冲区不足
 *         -154   编码大小不一致
 *         -202   仅计算大小模式（非错误，调用方需检查）
 */
static int DoBase64_Encode(const byte* in, word32 inLen, byte* out,
                           word32* outLen, int escaped)
{
    int    ret = 0;
    word32 i = 0,
           j = 0,
           n = 0;   /* 换行计数器 */

    int    getSzOnly = (out == NULL);

    /* 计算编码后大小 */
    word32 outSz = (inLen + 3 - 1) / 3 * 4;                    /* 编码数据 */
    word32 addSz = (outSz + PEM_LINE_SZ - 1) / PEM_LINE_SZ;    /* 换行符 */

    if (escaped == WC_ESC_NL_ENC)
        addSz *= 3;   /* 转义模式下每个 \n 变成 %0A（3 字节） */
    else if (escaped == WC_NO_NL_ENC)
        addSz = 0;    /* 无换行模式不添加换行符 */

    outSz += addSz;

    /* 检查输出缓冲区 */
    if (!outLen || (outSz > *outLen && !getSzOnly)) return -2;

    /* 主循环：每次处理 3 个输入字节 */
    while (inLen > 2)
    {
        byte b1 = in[j++];
        byte b2 = in[j++];
        byte b3 = in[j++];

        /* 计算 4 个 Base64 编码索引 */
        byte e1 = b1 >> 2;
        byte e2 = (byte)(((b1 & 0x3) << 4) | (b2 >> 4));
        byte e3 = (byte)(((b2 & 0xF) << 2) | (b3 >> 6));
        byte e4 = b3 & 0x3F;

        /* 写入 4 个编码字符 */
        ret = CEscape(escaped, e1, out, &i, *outLen, 0, getSzOnly);
        if (ret != 0) break;
        ret = CEscape(escaped, e2, out, &i, *outLen, 0, getSzOnly);
        if (ret != 0) break;
        ret = CEscape(escaped, e3, out, &i, *outLen, 0, getSzOnly);
        if (ret != 0) break;
        ret = CEscape(escaped, e4, out, &i, *outLen, 0, getSzOnly);
        if (ret != 0) break;

        inLen -= 3;

        /* 每 64 字符插入换行（无换行模式除外） */
        if (escaped != WC_NO_NL_ENC && (++n % (PEM_LINE_SZ/4)) == 0 && inLen)
        {
            ret = CEscape(escaped, '\n', out, &i, *outLen, 1, getSzOnly);
            if (ret != 0) break;
        }
    }

    /* 处理末尾不足 3 字节的情况 */
    if (inLen && ret == 0)
    {
        int twoBytes = (inLen == 2);

        byte b1 = in[j++];
        byte b2 = (twoBytes) ? in[j++] : 0;

        byte e1 = b1 >> 2;
        byte e2 = (byte)(((b1 & 0x3) << 4) | (b2 >> 4));
        byte e3 = (byte)((b2 & 0xF) << 2);

        ret = CEscape(escaped, e1, out, &i, *outLen, 0, getSzOnly);
        if (ret == 0)
            ret = CEscape(escaped, e2, out, &i, *outLen, 0, getSzOnly);
        if (ret == 0)
        {
            /* 第 3 个字符：2 字节时编码，1 字节时填充 '=' */
            if (twoBytes)
                ret = CEscape(escaped, e3, out, &i, *outLen, 0, getSzOnly);
            else
                ret = CEscape(escaped, '=', out, &i, *outLen, 1, getSzOnly);
        }
        /* 第 4 个字符始终填充 '=' */
        if (ret == 0)
            ret = CEscape(escaped, '=', out, &i, *outLen, 1, getSzOnly);
    }

    /* 末尾换行（无换行模式除外） */
    if (ret == 0 && escaped != WC_NO_NL_ENC)
        ret = CEscape(escaped, '\n', out, &i, *outLen, 1, getSzOnly);

    if (i != outSz && escaped != 1 && ret == 0)
        return -154;

    *outLen = i;
    if(ret == 0)
        return getSzOnly ? -202 : 0;
    return ret;
}

/* ======================== Base64 对外接口 ======================================== */

/**
 * @brief Base64 编码（标准 PEM 格式，每 64 字符插入 \n）
 *
 * @param in     输入数据
 * @param inLen  输入长度
 * @param out    输出缓冲区
 * @param outLen 输入/输出长度
 * @return 0 成功
 */
int Base64_Encode(const byte* in, word32 inLen, byte* out, word32* outLen)
{
    return DoBase64_Encode(in, inLen, out, outLen, WC_STD_ENC);
}

/**
 * @brief Base64 编码（转义换行模式，用 %0A 替代 \n）
 *
 * @param in     输入数据
 * @param inLen  输入长度
 * @param out    输出缓冲区
 * @param outLen 输入/输出长度
 * @return 0 成功
 */
int Base64_EncodeEsc(const byte* in, word32 inLen, byte* out, word32* outLen)
{
    return DoBase64_Encode(in, inLen, out, outLen, WC_ESC_NL_ENC);
}

/**
 * @brief Base64 编码（无换行模式，Token 签名编码使用）
 *
 * @param in     输入数据
 * @param inLen  输入长度
 * @param out    输出缓冲区
 * @param outLen 输入/输出长度
 * @return 0 成功
 */
int Base64_Encode_NoNl(const byte* in, word32 inLen, byte* out, word32* outLen)
{
    return DoBase64_Encode(in, inLen, out, outLen, WC_NO_NL_ENC);
}

/* ======================== Base64 解码 ============================================ */

/**
 * @brief Base64 解码
 *
 * 解码流程：
 *   - 每 4 个 Base64 字符解码为 3 个字节
 *   - 处理填充字符 '='（1 个或 2 个）
 *   - 跳过换行符（\n、\r\n）和尾部空白
 *
 * @param in     输入的 Base64 编码字符串
 * @param inLen  输入长度
 * @param out    输出缓冲区
 * @param outLen 输入时为输出缓冲区大小，输出时为实际解码长度
 * @return 0      成功
 *         -173  输出缓冲区不足
 *         -154  输入数据格式错误
 */
int Base64_Decode(const byte* in, word32 inLen, byte* out, word32* outLen)
{
    word32 i = 0;
    word32 j = 0;
    word32 plainSz = inLen - ((inLen + (PEM_LINE_SZ - 1)) / PEM_LINE_SZ );
    const byte maxIdx = (byte)sizeof(base64Decode) + 0x2B - 1;

    /* 估算解码后大小 */
    plainSz = (plainSz * 3 + 3) / 4;
    if (plainSz > *outLen) return -173;

    while (inLen > 3)
    {
        byte b1, b2, b3;
        byte e1 = in[j++];
        byte e2 = in[j++];
        byte e3 = in[j++];
        byte e4 = in[j++];

        int pad3 = 0;
        int pad4 = 0;

        if (e1 == 0)            /* 文件结束标记 */
            break;
        if (e3 == PAD)
            pad3 = 1;
        if (e4 == PAD)
            pad4 = 1;

        /* 验证字符范围 */
        if (e1 < 0x2B || e2 < 0x2B || e3 < 0x2B || e4 < 0x2B)
        {
            return -154;
        }

        if (e1 > maxIdx || e2 > maxIdx || e3 > maxIdx || e4 > maxIdx)
        {
            return -154;
        }

        /* 查表解码 */
        e1 = base64Decode[e1 - 0x2B];
        e2 = base64Decode[e2 - 0x2B];
        e3 = (e3 == PAD) ? 0 : base64Decode[e3 - 0x2B];
        e4 = (e4 == PAD) ? 0 : base64Decode[e4 - 0x2B];

        /* 重组 3 个原始字节 */
        b1 = (byte)((e1 << 2) | (e2 >> 4));
        b2 = (byte)(((e2 & 0xF) << 4) | (e3 >> 2));
        b3 = (byte)(((e3 & 0x3) << 6) | e4);

        out[i++] = b1;
        if (!pad3)
            out[i++] = b2;
        if (!pad4)
            out[i++] = b3;
        else
            break;

        inLen -= 4;

        /* 跳过换行符（支持 \n、\r\n 和尾部空白） */
        if (inLen && (in[j] == ' ' || in[j] == '\r' || in[j] == '\n'))
        {
            byte endLine = in[j++];
            inLen--;
            while (inLen && endLine == ' ')   /* 允许尾部空白 */
            {
                endLine = in[j++];
                inLen--;
            }
            if (endLine == '\r')
            {
                if (inLen)
                {
                    endLine = in[j++];
                    inLen--;
                }
            }
            if (endLine != '\n')
            {
                return -154;
            }
        }
    }
    *outLen = i;

    return 0;
}

/* ======================== HMAC 签名计算 ========================================== */

/**
 * @brief 计算 HMAC 签名
 *
 * 使用 mbedtls 库计算 HMAC（Hash-based Message Authentication Code）。
 * 支持三种哈希算法：MD5、SHA1、SHA256。
 *
 * 计算流程：
 *   1. 根据签名方法选择对应的 mbedtls 哈希信息
 *   2. 初始化 HMAC 上下文
 *   3. 设置密钥（hmac_starts）
 *   4. 输入待签名内容（hmac_update）
 *   5. 完成计算并输出签名（hmac_finish）
 *
 * @param method      签名方法（MD5/SHA1/SHA256）
 * @param key         HMAC 密钥
 * @param key_len     密钥长度
 * @param content     待签名内容
 * @param content_len 内容长度
 * @param output      输出缓冲区（存放签名结果）
 */
static void calc_hmd(enum sig_method_e method, unsigned char* key, size_t key_len,
                     unsigned char *content, size_t content_len, unsigned char *output)
{
    mbedtls_md_context_t md_ctx;
    const mbedtls_md_info_t *md_info = NULL;

    /* 选择哈希算法 */
    if (SIG_METHOD_MD5 == method)
    {
        md_info = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
    }
    else if (SIG_METHOD_SHA1 == method)
    {
        md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    }
    else if (SIG_METHOD_SHA256 == method)
    {
        md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    }

    /* HMAC 计算 */
    mbedtls_md_init(&md_ctx);
    mbedtls_md_setup(&md_ctx, md_info, 1);   /* 参数 1 = 使用 HMAC 模式 */
    mbedtls_md_hmac_starts(&md_ctx, key, key_len);
    mbedtls_md_hmac_update(&md_ctx, content, content_len);
    mbedtls_md_hmac_finish(&md_ctx, output);
    mbedtls_md_free(&md_ctx);
}

/* ======================== Token 生成 ============================================= */

/** Token 输出缓冲区大小 */
#define DEV_TOKEN_LEN 256

/** OneNET 安全鉴权协议版本号 */
#define DEV_TOKEN_VERISON_STR "2018-10-31"

/** 签名方法字符串（拼接到 Token 中） */
#define DEV_TOKEN_SIG_METHOD_MD5 "md5"
#define DEV_TOKEN_SIG_METHOD_SHA1 "sha1"
#define DEV_TOKEN_SIG_METHOD_SHA256 "sha256"

/**
 * @brief 生成 OneNET 设备认证 Token
 *
 * 按 OneNET 安全鉴权协议生成设备 Token。
 *
 * Token 格式：
 *   version=2018-10-31&res=products/{pid}/devices/{device}&et={exp}&method={sig}&sign={sign_url_encoded}
 *
 * 生成流程（共 6 步）：
 *   1. 拼接 Token 前缀（version + res + et + method）
 *   2. 对 access_key 进行 Base64 解码得到 HMAC 密钥
 *   3. 构造待签名字符串（按 OneNET 协议格式）
 *   4. 使用 HMAC 计算签名
 *   5. 对签名结果进行 Base64 编码（无换行模式）
 *   6. 对 Base64 结果中的特殊字符进行 URL 转义后拼接到 Token
 *
 * 待签名字符串格式（设备级）：
 *   "{exp}\n{method}\nproducts/{pid}/devices/{device}\n2018-10-31"
 *
 * 待签名字符串格式（产品级，dev_name 为 NULL）：
 *   "{exp}\n{method}\nproducts/{pid}\n2018-10-31"
 *
 * @param token       输出缓冲区（建议 >= 512 字节）
 * @param method      签名方法
 * @param exp_time    Token 过期时间（Unix 时间戳）
 * @param product_id  OneNET 产品 ID
 * @param dev_name    设备名称（NULL 表示产品级 Token）
 * @param access_key  OneNET Access Key（Base64 编码）
 * @return 0 成功
 */
int32_t dev_token_generate(char* token, enum sig_method_e method,
                        uint32_t exp_time, const char* product_id,
                        const char* dev_name, const char* access_key)
{
    uint8_t  base64_data[64] = { 0 };
    uint8_t  str_for_sig[64] = { 0 };
    uint8_t  sign_buf[128]   = { 0 };
    unsigned int base64_data_len = sizeof(base64_data);
    uint8_t* sig_method_str  = NULL;
    unsigned int sign_len        = 0;
    uint32_t i               = 0;
    char* tmp             = NULL;

    /* 步骤 1：拼接 Token 前缀 */
    sprintf(token, "version=%s", DEV_TOKEN_VERISON_STR);

    if (dev_name)
    {
        /* 设备级 Token：包含设备名称 */
        sprintf(token + strlen(token), "&res=products%%2F%s%%2Fdevices%%2F%s", product_id, dev_name);
    }
    else
    {
        /* 产品级 Token：仅包含产品 ID */
        sprintf(token + strlen(token), "&res=products%%2F%s", product_id);
    }

    sprintf(token + strlen(token), "&et=%lu", exp_time);

    /* 步骤 2：Base64 解码 access_key 得到 HMAC 密钥 */
    Base64_Decode((const byte*)access_key, strlen(access_key), base64_data, &base64_data_len);

    /* 步骤 3：根据签名方法确定方法字符串和签名长度 */
    if (SIG_METHOD_MD5 == method)
    {
        sig_method_str = (uint8_t*)DEV_TOKEN_SIG_METHOD_MD5;
        sign_len       = 16;    /* MD5 输出 16 字节 */
    }
    else if (SIG_METHOD_SHA1 == method)
    {
        sig_method_str = (uint8_t*)DEV_TOKEN_SIG_METHOD_SHA1;
        sign_len       = 20;    /* SHA1 输出 20 字节 */
    }
    else if (SIG_METHOD_SHA256 == method)
    {
        sig_method_str = (uint8_t*)DEV_TOKEN_SIG_METHOD_SHA256;
        sign_len       = 32;    /* SHA256 输出 32 字节 */
    }

    /* 拼接 method 字段到 Token */
    sprintf(token + strlen(token), "&method=%s", sig_method_str);

    /* 步骤 4：构造待签名字符串 */
    if (dev_name)
    {
        /* 设备级：{exp}\n{method}\nproducts/{pid}/devices/{device}\n2018-10-31 */
        sprintf((char*)str_for_sig, "%lu\n%s\nproducts/%s/devices/%s\n%s",
                exp_time, sig_method_str, product_id, dev_name, DEV_TOKEN_VERISON_STR);
    }
    else
    {
        /* 产品级：{exp}\n{method}\nproducts/{pid}\n2018-10-31 */
        sprintf((char*)str_for_sig, "%lu\n%s\nproducts/%s\n%s",
                exp_time, sig_method_str, product_id, DEV_TOKEN_VERISON_STR);
    }

    /* 步骤 5：计算 HMAC 签名 */
    calc_hmd(method, base64_data, base64_data_len, str_for_sig, strlen((char*)str_for_sig), sign_buf);

    /* 步骤 6：Base64 编码签名结果（无换行模式） */
    memset(base64_data, 0, sizeof(base64_data));
    base64_data_len = sizeof(base64_data);
    Base64_Encode_NoNl(sign_buf, sign_len, base64_data, &base64_data_len);

    /* 步骤 7：URL 转义并拼接到 Token */
    strcat(token, "&sign=");
    tmp = token + strlen(token);

    for (i = 0; i < base64_data_len; i++)
    {
        switch (base64_data[i])
        {
            case '+':
                strcat(tmp, "%2B");
                tmp += 3;
                break;
            case ' ':
                strcat(tmp, "%20");
                tmp += 3;
                break;
            case '/':
                strcat(tmp, "%2F");
                tmp += 3;
                break;
            case '?':
                strcat(tmp, "%3F");
                tmp += 3;
                break;
            case '%':
                strcat(tmp, "%25");
                tmp += 3;
                break;
            case '#':
                strcat(tmp, "%23");
                tmp += 3;
                break;
            case '&':
                strcat(tmp, "%26");
                tmp += 3;
                break;
            case '=':
                strcat(tmp, "%3D");
                tmp += 3;
                break;
            default:
                /* 普通字符直接写入 */
                *tmp = base64_data[i];
                tmp += 1;
                break;
        }
    }

    return 0;
}