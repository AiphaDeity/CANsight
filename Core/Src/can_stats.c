#include "can_stats.h"

#include <stddef.h>
#include <string.h>

static CanStatsEntry entries[CAN_STATS_CAPACITY];  // 固定容量统计表。
static uint16_t entry_count;       // 有效统计条目数。
static uint32_t total_count;       // 累计处理帧数，包含未入表帧。
static uint32_t untracked_count;   // 容量耗尽后的未入表帧数。
static CanStatsRuntime runtime_state;

// 重置所有统计条目及模块计数。
void CanStats_Init(void)
{
    memset(entries, 0, sizeof(entries));  // 同时清除条目计数和周期有效标志。
    entry_count = 0;
    total_count = 0;
    untracked_count = 0;
    memset(&runtime_state, 0, sizeof(runtime_state));
}

// 按 ID、IDE 和 RTR 查找或建立条目，并更新接收统计。
void CanStats_AddFrame(const CanFrame *frame)
{
    if(frame == NULL) return;
    total_count++;  // 总帧数包含因容量限制而未入表的报文。

    // DLC 和数据内容不参与条目匹配，同一组合的数据变化仅更新原条目。
    uint16_t index;
    for(index = 0; index < entry_count; index++) {
        const CanFrame *previous = &entries[index].latest;
        if(previous->id == frame->id && previous->ide == frame->ide &&
           previous->rtr == frame->rtr) break;
    }

    if(index == entry_count) {  // 未匹配已有条目。
        if(entry_count == CAN_STATS_CAPACITY) {
            untracked_count++;  // 容量耗尽时保留已有条目，累计未入表帧数。
            return;
        }
        entry_count++;  // 将当前空闲位置纳入有效条目范围。
    }

    CanStatsEntry *entry = &entries[index];
    if(entry->count != 0U) {
        /* 在覆盖旧报文前计算接收间隔。无符号减法支持跨一次 TIM2 回绕，
         * 前提是相邻两帧间隔小于 2^32 微秒，约 71.6 分钟。
         */
        entry->period_us = frame->timestamp_us - entry->latest.timestamp_us;
        entry->has_period = 1U;
    }
    // 首帧保留无效周期标志，界面据此显示 --。
    entry->latest = *frame;
    if(entry->latest.dlc > 8U) entry->latest.dlc = 8U;  // 限制为经典 CAN 最大数据长度。
    if(frame->rtr != 0U) memset(entry->latest.data, 0, sizeof(entry->latest.data));  // 清除远程帧的无效数据域。
    entry->count++;
}

// 以下查询接口不修改统计状态。
uint16_t CanStats_GetEntryCount(void) { return entry_count; }
// 返回内部条目的只读视图，下标越界时返回 NULL。
const CanStatsEntry *CanStats_GetEntry(uint16_t index)
{
    return index < entry_count ? &entries[index] : NULL;
}
// 返回累计处理帧数。
uint32_t CanStats_GetTotalCount(void) { return total_count; }
// 返回容量限制导致的未入表帧数。
uint32_t CanStats_GetUntrackedCount(void) { return untracked_count; }

// 按值保存状态快照，避免依赖调用方局部变量的生命周期。
void CanStats_SetRuntime(const CanStatsRuntime *runtime)
{
    if(runtime != NULL) runtime_state = *runtime;
}

const CanStatsRuntime *CanStats_GetRuntime(void) { return &runtime_state; }
