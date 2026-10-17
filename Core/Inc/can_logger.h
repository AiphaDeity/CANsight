#ifndef CAN_LOGGER_H
#define CAN_LOGGER_H

#include <stdint.h>

/* CAN 接收中断和主循环共用的一帧经典 CAN 报文。 */
typedef struct
{
  uint32_t timestamp_us; /* TIM2 微秒计数；32 位计数约每 71.6 分钟回绕一次。 */
  uint32_t id;           /* 标准帧或扩展帧标识符。 */
  uint8_t ide;           /* CAN_ID_STD 或 CAN_ID_EXT。 */
  uint8_t rtr;           /* CAN_RTR_DATA 或 CAN_RTR_REMOTE。 */
  uint8_t dlc;           /* 经典 CAN 数据长度，范围 0 到 8。 */
  uint8_t data[8];       /* 数据帧内容；远程帧没有有效数据。 */
} CanFrame;

/* 挂载 SD 卡并打开 CANLOG.CSV；失败时 CAN 接收仍可继续。 */
uint8_t CanLogger_Init(void);

/* 将一帧报文追加到 RAM 缓冲区，不执行文件系统操作。 */
void CanLogger_AddFrame(const CanFrame *frame);

/* 主循环调用：缓冲区满时写卡，每秒同步一次文件。 */
void CanLogger_Process(void);

/* 返回 1 表示本次运行中的 SD 日志仍可写。 */
uint8_t CanLogger_IsReady(void);

#endif /* CAN_LOGGER_H */
