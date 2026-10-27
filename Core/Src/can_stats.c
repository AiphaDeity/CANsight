// 接收统计模块：主循环每取出一帧后更新，界面读取最新汇总，不在 CAN 中断里修改。
// 统计只保留各 ID / IDE / RTR 组合的最新帧；逐帧历史由日志模块保存。
#include "can_stats.h"

#include <stddef.h>
#include <string.h>

static CanStatsEntry entries[CAN_STATS_CAPACITY];  // 固定容量统计表。
static uint16_t entry_count;       // 有效统计条目数。
static uint32_t total_count;       // 累计处理帧数，包含未入表帧。
static uint32_t untracked_count;   // 容量耗尽后的未入表帧数。
static CanStatsRuntime runtime_state;  // main 发布的 CAN、SD 和队列状态副本，供界面查询。

// main 启动时调用：清空固定统计表、总数和运行状态；切页或筛选时不调用。
void CanStats_Init(void)
{
    memset(entries, 0, sizeof(entries));
    entry_count = 0;
    total_count = 0;
    untracked_count = 0;
    memset(&runtime_state, 0, sizeof(runtime_state));
}

// 主循环调用：按 ID、标准/扩展和数据/远程类型匹配条目，更新最新帧、计数和最近周期。
// NULL 不计数；表满后不新增条目，但总数和未入表计数继续累加，已有条目仍可更新。
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
        // 采集端已展开 TIM2 回绕，较长的报文间隔不会截断为 32 位。
        entry->period_us = frame->timestamp_us - entry->latest.timestamp_us;
        entry->has_period = 1U;
    }
    // 首帧无法计算间隔，保留无效周期标志；调用方应先判断 has_period 再使用周期。
    // 覆盖该组合的最新状态，不在统计表中追加所有历史帧；32 位累计计数按无符号规则回绕。
    entry->latest = *frame;
    if(entry->latest.dlc > 8U) entry->latest.dlc = 8U;  // 限制为经典 CAN 最大数据长度。
    if(frame->rtr != 0U) memset(entry->latest.data, 0, sizeof(entry->latest.data));  // 清除远程帧的无效数据域。
    entry->count++;
}

// 界面查询：返回已占用的统计条目数，不是总帧数；最多 CAN_STATS_CAPACITY 项。
uint16_t CanStats_GetEntryCount(void) { return entry_count; }
// 界面按首次出现顺序查询条目：index 有效时返回内部只读地址，否则返回 NULL。
// 不能释放或修改该地址；后续新帧会更新同一条目的内容。
const CanStatsEntry *CanStats_GetEntry(uint16_t index)
{
    return index < entry_count ? &entries[index] : NULL;
}
// 返回交给本模块的累计帧数，包含未入表帧，不包含入队前或队列满时丢失的帧。
uint32_t CanStats_GetTotalCount(void) { return total_count; }
// 返回统计表满后，无法为新组合建立条目的累计帧数；不代表这些帧的 SD 记录也失败。
uint32_t CanStats_GetUntrackedCount(void) { return untracked_count; }

// main 调用：按值复制运行状态，不保存调用方局部变量地址；NULL 时不更新。
void CanStats_SetRuntime(const CanStatsRuntime *runtime)
{
    if(runtime != NULL) runtime_state = *runtime;
}

// 界面查询：返回内部运行状态副本的只读地址，不提供跨中断的整体原子快照。
const CanStatsRuntime *CanStats_GetRuntime(void) { return &runtime_state; }
