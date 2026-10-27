// 时间文字格式化工具：供日志和界面在主循环上下文调用，不读取硬件计时器。
// 使用整数运算，不依赖精简 printf 对 64 位整数或浮点输出的支持。
#include "can_time.h"

#include <stddef.h>

// 将 uint64_t 转成十进制字符串；text 由调用方提供，至少 21 字节，包含结束符。
// 函数直接写入缓冲，不分配内存，也不检查调用方容量。
void CanTime_FormatUnsigned(char *text, uint64_t value)
{
    // uint64_t 十进制最多 20 位；先逐次取末位，再倒序复制得到正常顺序。
    char reversed[20];
    unsigned count = 0U;
    // 使用 do-while，让 value 为 0 时也生成一个字符 0。
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

// 将无符号数按 scale 缩放成固定小数位文本，不进行浮点运算或四舍五入。
// scale 须非零且与 digits 匹配；当前微秒转秒使用 1000000 和 6，缓冲至少 28 字节。
void CanTime_FormatFixed(char *text, uint64_t value, uint32_t scale, unsigned digits)
{
    CanTime_FormatUnsigned(text, value / scale);
    size_t used = 0U;
    while(text[used])
    {
        used++;
    }
    text[used++] = '.';
    // 余数是小数部分；从右向左填满 digits 位，不足的高位自然补 0。
    uint32_t fraction = (uint32_t)(value % scale);
    for(unsigned i = digits; i > 0U; i--)
    {
        text[used + i - 1U] = (char)('0' + fraction % 10U);
        fraction /= 10U;
    }
    text[used + digits] = '\0';
}
