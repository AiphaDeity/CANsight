#ifndef PERF_DIAG_H
#define PERF_DIAG_H

#include <stdint.h>
#include "cansight_config.h"

typedef enum {
    PERF_OTHER, PERF_RX_BATCH, PERF_LOGGER, PERF_STATS, PERF_LOG_PROCESS,
    PERF_UI, PERF_HISTORY, PERF_WRITE, PERF_SYNC, PERF_ARCHIVE,
    PERF_DMA_WAIT, PERF_CARD_READY, PERF_IDLE, PERF_PHASE_COUNT
} PerfPhase;

#if CANSIGHT_DETAILED_DIAGNOSTICS
typedef struct {
    uint32_t started_us;
    uint32_t queue_before;
    uint32_t lost_before;
    PerfPhase previous;
    uint8_t active;
} PerfScope;

// 计时入口和出口仅由主循环调用，必须按嵌套顺序配对。
void PerfDiag_Reset(uint32_t (*queue_depth)(void), uint32_t (*queue_lost)(void));
PerfScope PerfDiag_Begin(PerfPhase phase);
void PerfDiag_End(PerfScope scope);
// ISR 只保存第一次软件队列溢出，不打印、不调用文件系统。
void PerfDiag_RecordLossFromISR(uint32_t timestamp_us, uint32_t depth, uint32_t fifo_frames);
void PerfDiag_SampleQueue(uint32_t depth, uint32_t fifo_frames, uint32_t tick_ms);
void PerfDiag_Pause(void);
void PerfDiag_Print(void);
#else
// 关闭时不计算实参，不读取时间戳或队列，不分配阶段记录数组。
typedef uint8_t PerfScope;
#define PerfDiag_Reset(depth, lost) ((void)0)
#define PerfDiag_Begin(phase) ((PerfScope)0)
#define PerfDiag_End(scope) ((void)sizeof(scope))
#define PerfDiag_RecordLossFromISR(timestamp, depth, fifo) ((void)0)
#define PerfDiag_SampleQueue(depth, fifo, tick) ((void)0)
#define PerfDiag_Pause() ((void)0)
#define PerfDiag_Print() ((void)0)
#endif

#endif
