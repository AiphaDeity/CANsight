#ifndef CAN_TIME_H
#define CAN_TIME_H

#include <stdint.h>

// 将 64 位无符号数转换为十进制，避免依赖精简 printf 的 long long 支持。
// text 指向调用方提供的可写缓冲区，至少 21 字节，包含字符串结束符。
void CanTime_FormatUnsigned(char *text, uint64_t value);

// 输出带固定小数位的无符号数值；scale 必须非零且等于 10 的 digits 次幂。
// text 至少预留 22 + digits 字节；当前微秒转秒使用 scale=1000000、digits=6。
void CanTime_FormatFixed(char *text, uint64_t value, uint32_t scale, unsigned digits);

#endif
