#include "can_time.h"

#include <stddef.h>

// 格式化无符号整数
void CanTime_FormatUnsigned(char *text, uint64_t value)
{
    char reversed[20];
    unsigned count = 0U;
    do
    {
        reversed[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while(value);
    for(unsigned i = 0; i < count; i++)
    {
        text[i] = reversed[count - 1U - i];
    }
    text[count] = '\0';
}

// 格式化带固定小数位的无符号数值
void CanTime_FormatFixed(char *text, uint64_t value, uint32_t scale, unsigned digits)
{
    CanTime_FormatUnsigned(text, value / scale);
    size_t used = 0U;
    while(text[used])
    {
        used++;
    }
    text[used++] = '.';
    uint32_t fraction = (uint32_t)(value % scale);
    for(unsigned i = digits; i > 0U; i--)
    {
        text[used + i - 1U] = (char)('0' + fraction % 10U);
        fraction /= 10U;
    }
    text[used + digits] = '\0';
}
