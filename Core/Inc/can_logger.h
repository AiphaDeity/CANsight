#ifndef CAN_LOGGER_H
#define CAN_LOGGER_H

#include <stdint.h>

// CAN 接收中断和主循环共用的一帧经典 CAN 报文。
typedef struct
{
  uint64_t timestamp_us; // 本次启动的 64 位微秒时间，由 TIM2 及溢出计数共同提供。
  uint32_t id;           // 标准帧或扩展帧标识符。
  uint8_t ide;           // CAN_ID_STD 或 CAN_ID_EXT。
  uint8_t rtr;           // CAN_RTR_DATA 或 CAN_RTR_REMOTE。
  uint8_t dlc;           // 经典 CAN 数据长度，范围 0 到 8。
  uint8_t data[8];       // 数据帧内容；远程帧没有有效数据。
} CanFrame;

// SD 日志模块的当前状态，不等同于物理插卡检测结果。
typedef enum {
  CAN_LOG_UNMOUNTED = 0,  // 文件系统尚未成功挂载。
  CAN_LOG_READY,          // 文件系统已挂载，活动日志尚未就绪。
  CAN_LOG_RECORDING,      // 活动日志已打开，可接受接收记录。
  CAN_LOG_ERROR           // 日志打开、格式化、写入或同步失败。
} CanLoggerState;

#define CAN_LOG_ARCHIVE_NAME_SIZE 24U
#define CAN_LOG_MAX_FILE_SIZE (10UL * 1024UL * 1024UL)  // 单个活动日志的大小上限。

// 归档请求的处理状态，与日志写入状态分别维护。
typedef enum {
  CAN_ARCHIVE_IDLE = 0,
  CAN_ARCHIVE_PENDING,
  CAN_ARCHIVE_DONE,
  CAN_ARCHIVE_EMPTY,
  CAN_ARCHIVE_FAILED
} CanArchiveState;

/**
 * @brief 请求归档当前日志，并创建新的活动日志。
 * @return 请求受理时返回 1；日志不可用或已有请求待处理时返回 0。
 * @note 仅提交请求，文件操作由主循环中的 CanLogger_Process() 执行。
 */
uint8_t CanLogger_RequestArchive(void);
CanArchiveState CanLogger_GetArchiveState(void);
// 返回最近成功归档的文件名，只读，无归档记录时为空字符串。
const char *CanLogger_GetLastArchiveName(void);
// 每次成功生成历史文件后递增，供文件列表判断是否需要刷新。
uint32_t CanLogger_GetArchiveRevision(void);

// 获取日志状态及最近一次 FatFs 错误码，仅读取模块状态。
CanLoggerState CanLogger_GetState(void);
uint32_t CanLogger_GetLastError(void);

// 挂载 SD 卡，归档已有的非空日志，并打开新的活动日志。
uint8_t CanLogger_Init(void);

// 主循环调用：追加到 RAM 缓冲区，空间不足时先写卡。
void CanLogger_AddFrame(const CanFrame *frame);

// 主循环调用：处理归档请求，并约每秒写出缓冲及同步文件。
void CanLogger_Process(void);

// 返回 1 表示本次运行中的 SD 日志仍可写。
uint8_t CanLogger_IsReady(void);

#endif /* CAN_LOGGER_H */
