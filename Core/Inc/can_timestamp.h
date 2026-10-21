#ifndef CAN_TIMESTAMP_H
#define CAN_TIMESTAMP_H

#include <stdint.h>

// TIM2 以 1 MHz 运行，溢出中断维护高 32 位，时间基准为本次设备启动。
uint8_t CanTimestamp_Start(void);
uint64_t CanTimestamp_GetUs(void);
void CanTimestamp_IRQHandler(void);

#endif
