#ifndef CAN_TIME_H
#define CAN_TIME_H

#include <stdint.h>
#include <stddef.h>

// 将 64 位无符号数转换为十进制，避免依赖精简 printf 的 long long 支持。
static inline void CanTime_FormatUnsigned(char *text, uint64_t value)
{
    char reversed[20];
    unsigned count = 0U;
    do {
        reversed[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while(value);
    for(unsigned i = 0; i < count; i++) text[i] = reversed[count - 1U - i];
    text[count] = '\0';
}

// 输出带固定小数位的时间；调用方提供至少 28 字节缓冲区。
static inline void CanTime_FormatFixed(char *text, uint64_t value, uint32_t scale, unsigned digits)
{
    CanTime_FormatUnsigned(text, value / scale);
    size_t used = 0U;
    while(text[used]) used++;
    text[used++] = '.';
    uint32_t fraction = (uint32_t)(value % scale);
    for(unsigned i = digits; i > 0U; i--) {
        text[used + i - 1U] = (char)('0' + fraction % 10U);
        fraction /= 10U;
    }
    text[used + digits] = '\0';
}

#endif
