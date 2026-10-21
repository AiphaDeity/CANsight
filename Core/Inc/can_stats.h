#ifndef CAN_STATS_H
#define CAN_STATS_H

#include "can_logger.h"

//最多保存 128 种 ID + IDE + RTR 组合，固定分配内存，不使用动态堆。
#define CAN_STATS_CAPACITY 128U

// 一条汇总记录：同一组合的新帧覆盖 latest，并更新计数和周期
typedef struct {
    CanFrame latest;       //最新一帧，包含时间戳、ID、格式、DLC 和数据
    uint32_t count;        // 本次启动以来，该组合累计收到的帧数。
    uint64_t period_us;    // 最近两帧接收时间之差，单位微秒，不是平均周期。
    uint8_t has_period;    //收到第二帧后置 1；只有第一帧时周期尚不可计算。 
} CanStatsEntry;

// CAN 控制器当前状态；接收溢出及历史错误另行记录。
typedef enum {
    CAN_STATS_STOPPED = 0,
    CAN_STATS_RUNNING,
    CAN_STATS_ERROR
} CanStatsState;

// 硬件层向界面发布的运行状态快照。
typedef struct {
    CanStatsState can_state;  //CAN 当前未启动、运行中或错误
    CanLoggerState sd_state;  //SD 当前未挂载、就绪、记录中或错误
    uint32_t queue_lost;       // 软件队列已确认丢弃的帧数。
    uint32_t fifo_overruns;    // FIFO 溢出事件数，不等同于精确丢帧数。
    uint32_t error_flags;      // 本次启动累计发生的 HAL CAN 错误位。
    uint32_t sd_error;         // 最近一次 FatFs 错误码。
    uint32_t fifo_frames;      // 从硬件 FIFO 成功取出的帧数，包含软件队列拒收帧。
    uint32_t queue_depth;      // 发布快照时的软件队列占用。
    uint32_t queue_peak;       // 本次启动的软件队列最高占用。
    uint32_t queue_capacity;   // 软件队列实际可用容量。
    uint32_t max_service_gap_us; // 相邻接收处理批次开始时刻的最长间隔。
    uint32_t max_batch_us;     // 单次接收批次最长耗时，包含追加日志中的同步写卡。
    uint32_t max_ui_us;        // LVGL 处理函数最长耗时，包含触摸和显示刷新。
    uint32_t max_history_us;   // 单次历史文件处理最长耗时，包含查询及删除。
    CanLoggerDiagnostics logger; // 日志接纳、写入、同步及耗时的独立快照。
    CanArchiveState archive_state;
    char archive_name[CAN_LOG_ARCHIVE_NAME_SIZE];  // 最近成功归档的文件名。
} CanStatsRuntime;

/**
 * @brief 发布或读取当前运行状态。
 * @note 仅由主循环及其 LVGL 回调调用，不支持中断并发访问。
 */
void CanStats_SetRuntime(const CanStatsRuntime *runtime);
const CanStatsRuntime *CanStats_GetRuntime(void);

//启动时调用一次，清空条目和计数；不要在界面切换或筛选时调用
void CanStats_Init(void);

//主循环每取出一帧后调用；NULL 不计数，表满不影响已有条目更新
void CanStats_AddFrame(const CanFrame *frame);

// 返回已占用的条目数量，不是总帧数，也不是筛选匹配数量。
uint16_t CanStats_GetEntryCount(void);

//按首次出现顺序读取条目；越界返回 NULL。
const CanStatsEntry *CanStats_GetEntry(uint16_t index);

//总数为交给统计模块的帧数，不包含接收队列之前丢失的帧
uint32_t CanStats_GetTotalCount(void);

//表满后无法建立新条目的帧数；已有条目仍继续更新
uint32_t CanStats_GetUntrackedCount(void);

#endif
