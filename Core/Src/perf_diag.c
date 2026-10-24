#include "perf_diag.h"
#if CANSIGHT_DETAILED_DIAGNOSTICS
#include "can_timestamp.h"
#include "can_time.h"
#include "main.h"
#include <stdio.h>
#include <string.h>

#define PERF_EVENT_LIMIT 12U
#define PERF_WINDOW_LIMIT 16U
#define PERF_LONG_US 5000U
#define PERF_WINDOW_MS 100U

typedef struct {
    uint64_t self_us;
    uint32_t calls, max_us, max_queue_before, max_queue_after;
} PhaseMetrics;

typedef struct {
    uint32_t ended_us, elapsed_us, queue_before, queue_after, lost;
    PerfPhase phase;
} LongEvent;

typedef struct {
    uint32_t ended_ms, span_ms, minimum, maximum, samples, arrived;
} QueueWindow;

static const char *const names[PERF_PHASE_COUNT] = {
    "other", "rx_batch", "logger", "stats", "log_process", "ui", "history",
    "write", "sync", "archive", "dma_wait", "card_ready", "idle"
};
static PhaseMetrics metrics[PERF_PHASE_COUNT];
static LongEvent events[PERF_EVENT_LIMIT], first_loss_event;
static QueueWindow windows[PERF_WINDOW_LIMIT];
static uint32_t event_count, event_next, window_count, window_next;
static uint32_t last_account_us;
static uint32_t (*read_depth)(void);
static uint32_t (*read_lost)(void);
static volatile uint32_t current_phase;
static volatile uint32_t first_loss_valid, first_loss_us, first_loss_phase;
static volatile uint32_t first_loss_depth, first_loss_fifo;
static volatile uint8_t enabled;
static uint8_t paused, first_loss_event_valid;
static uint32_t window_started_ms, window_first_fifo, window_min, window_max, window_samples;
static uint8_t window_active;

// self_us 在嵌套边界结算，子阶段不会再次计入父阶段；中断时间仍包含在经过时间内。
static void account_until(uint32_t now)
{
    metrics[current_phase].self_us += (uint32_t)(now - last_account_us);
    last_account_us = now;
}

void PerfDiag_Reset(uint32_t (*queue_depth)(void), uint32_t (*queue_lost)(void))
{
    enabled = 0U;
    memset(metrics, 0, sizeof(metrics));
    event_count = event_next = window_count = window_next = 0U;
    first_loss_valid = first_loss_event_valid = window_active = paused = 0U;
    read_depth = queue_depth;
    read_lost = queue_lost;
    current_phase = PERF_OTHER;
    last_account_us = (uint32_t)CanTimestamp_GetUs();
    enabled = 1U;
}

PerfScope PerfDiag_Begin(PerfPhase phase)
{
    PerfScope scope = {0};
    if(!enabled || paused || phase >= PERF_PHASE_COUNT) return scope;
    scope.queue_before = read_depth();
    scope.lost_before = read_lost();
    scope.started_us = (uint32_t)CanTimestamp_GetUs();
    scope.previous = (PerfPhase)current_phase;
    scope.active = 1U;
    account_until(scope.started_us);
    metrics[phase].calls++;
    if(scope.queue_before > metrics[phase].max_queue_before)
        metrics[phase].max_queue_before = scope.queue_before;
    current_phase = (uint32_t)phase;
    return scope;
}

void PerfDiag_End(PerfScope scope)
{
    if(!scope.active) return;
    uint32_t ended = (uint32_t)CanTimestamp_GetUs();
    PerfPhase phase = (PerfPhase)current_phase;
    account_until(ended);
    uint32_t elapsed = ended - scope.started_us;
    uint32_t depth = read_depth();
    uint32_t lost = read_lost() - scope.lost_before;
    if(elapsed > metrics[phase].max_us) metrics[phase].max_us = elapsed;
    if(depth > metrics[phase].max_queue_after) metrics[phase].max_queue_after = depth;
    LongEvent event = {ended, elapsed, scope.queue_before, depth, lost, phase};
    if(!first_loss_event_valid && scope.lost_before == 0U && lost != 0U) {
        first_loss_event = event;
        first_loss_event_valid = 1U;
    }
    if(elapsed >= PERF_LONG_US || lost != 0U) {
        events[event_next] = event;
        event_next = (event_next + 1U) % PERF_EVENT_LIMIT;
        if(event_count < PERF_EVENT_LIMIT) event_count++;
    }
    current_phase = (uint32_t)scope.previous;
}

void PerfDiag_RecordLossFromISR(uint32_t timestamp_us, uint32_t depth, uint32_t fifo_frames)
{
    if(!enabled || first_loss_valid) return;
    first_loss_us = timestamp_us;
    first_loss_phase = current_phase;
    first_loss_depth = depth;
    first_loss_fifo = fifo_frames;
    __DMB();
    first_loss_valid = 1U;
}

// 主循环边界采样；窗口最小值不是每帧精确轨迹，不能单独作为持续过载的证明。
void PerfDiag_SampleQueue(uint32_t depth, uint32_t fifo_frames, uint32_t tick_ms)
{
    if(!enabled) return;
    if(!window_active) {
        window_started_ms = tick_ms;
        window_first_fifo = fifo_frames;
        window_min = depth;
        window_max = window_samples = 0U;
        window_active = 1U;
    }
    if(depth < window_min) window_min = depth;
    if(depth > window_max) window_max = depth;
    window_samples++;
    uint32_t span = tick_ms - window_started_ms;
    if(span < PERF_WINDOW_MS) return;
    uint32_t arrived = fifo_frames - window_first_fifo;
    if(arrived != 0U) {
        windows[window_next] = (QueueWindow){tick_ms, span, window_min, window_max, window_samples, arrived};
        window_next = (window_next + 1U) % PERF_WINDOW_LIMIT;
        if(window_count < PERF_WINDOW_LIMIT) window_count++;
    }
    window_active = 0U;
}

static void print_event(const char *kind, const LongEvent *event)
{
    printf("CAN %s: end_us32=%lu phase=%s elapsed_us=%lu q_before=%lu q_after=%lu lost_delta=%lu\r\n",
           kind, (unsigned long)event->ended_us, names[event->phase],
           (unsigned long)event->elapsed_us, (unsigned long)event->queue_before,
           (unsigned long)event->queue_after, (unsigned long)event->lost);
}

void PerfDiag_Pause(void)
{
    if(!enabled) return;
    if(!paused) account_until((uint32_t)CanTimestamp_GetUs());
    paused = 1U;
    current_phase = PERF_OTHER;
}

void PerfDiag_Print(void)
{
    if(!enabled) return;
    PerfDiag_Pause();
    // 串口输出时间不计入分账；若此时恢复发送，首丢帧仍可被 ISR 捕获为 other。
    printf("CAN ACCOUNT: self_us excludes child scopes; max_us includes children and IRQ time\r\n");
    for(uint32_t i = 0U; i < PERF_PHASE_COUNT; i++) {
        const PhaseMetrics *m = &metrics[i];
        char total[21];
        CanTime_FormatUnsigned(total, m->self_us);
        printf("CAN STAGE: phase=%s calls=%lu self_us=%s max_us=%lu q_before_peak=%lu q_after_peak=%lu\r\n",
               names[i], (unsigned long)m->calls, total, (unsigned long)m->max_us,
               (unsigned long)m->max_queue_before, (unsigned long)m->max_queue_after);
    }
    if(first_loss_valid) {
        printf("CAN FIRST LOSS: at_us32=%lu phase=%s depth=%lu fifo=%lu\r\n",
               (unsigned long)first_loss_us, names[first_loss_phase],
               (unsigned long)first_loss_depth, (unsigned long)first_loss_fifo);
    }
    else printf("CAN FIRST LOSS: none\r\n");
    if(first_loss_event_valid) print_event("LOSS EVENT", &first_loss_event);
    for(uint32_t i = 0U; i < event_count; i++) {
        uint32_t index = (event_next + PERF_EVENT_LIMIT - event_count + i) % PERF_EVENT_LIMIT;
        print_event("LONG", &events[index]);
    }
    for(uint32_t i = 0U; i < window_count; i++) {
        uint32_t index = (window_next + PERF_WINDOW_LIMIT - window_count + i) % PERF_WINDOW_LIMIT;
        const QueueWindow *w = &windows[index];
        printf("CAN QUEUE WINDOW: end_ms=%lu span_ms=%lu sampled_min=%lu sampled_max=%lu samples=%lu arrived=%lu\r\n",
               (unsigned long)w->ended_ms, (unsigned long)w->span_ms,
               (unsigned long)w->minimum, (unsigned long)w->maximum,
               (unsigned long)w->samples, (unsigned long)w->arrived);
    }
    last_account_us = (uint32_t)CanTimestamp_GetUs();
    paused = 0U;
}
#endif
