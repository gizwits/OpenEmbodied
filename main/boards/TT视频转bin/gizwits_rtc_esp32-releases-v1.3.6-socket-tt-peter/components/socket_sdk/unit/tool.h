#ifndef _TOOL_H_
#define _TOOL_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// 添加 MIN 宏定义
#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif

/**
 * @brief 生成指定长度的随机字符串(A-Z)
 * @param len 字符串长度
 * @param out_buf 输出缓冲区
 * @param buf_size 缓冲区大小(需要比len大1，用于存放结束符)
 * @return true成功，false失败
 */
bool generate_random_string(uint8_t len, char *out_buf, size_t buf_size);

/**
 * @brief 十六进制数据转字符串
 * @param dest 目标缓冲区(需要源数据2倍+1的大小)
 * @param src 源数据
 * @param src_len 源数据长度
 * @param uppercase 是否大写
 * @return 转换后的字符串长度
 */
int hex_to_str(char *dest, const uint8_t *src, size_t src_len, bool uppercase);

/**
 * @brief 字符串转十六进制数据
 * @param dest 目标缓冲区
 * @param src 源字符串
 * @param dest_len 目标缓冲区大小
 * @return 转换的字节数，失败返回-1
 */
int str_to_hex(uint8_t *dest, const char *src, size_t dest_len);

/**
 * @brief 获取设备MAC地址字符串
 * @param mac_str 输出MAC字符串缓冲区(至少13字节)
 * @param uppercase 是否大写
 * @return true成功，false失败
 */
bool get_mac_str(char *mac_str, bool uppercase);

/**
 * @brief 打印当前堆内存使用情况
 * @param tag 日志标签
 * @param note 附加说明，可以为NULL
 */
void print_heap_info(const char *tag, const char *note);

#endif /* _TOOL_H_ */
