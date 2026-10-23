#ifndef UI_H_
#define UI_H_ 

#include "lvgl.h"

// 对照实验开关：0 为正常周期更新，1 为冻结周期统计更新；LVGL 与触摸继续运行。
#ifndef UI_DIAG_FREEZE_STATS
#define UI_DIAG_FREEZE_STATS 0
#endif
#if UI_DIAG_FREEZE_STATS != 0 && UI_DIAG_FREEZE_STATS != 1
#error "UI_DIAG_FREEZE_STATS must be zero or one"
#endif

// 接收页对照开关：1 时计数列显示 --，其余控件及后台统计继续更新。
#ifndef UI_DIAG_FREEZE_RX_METRICS
#define UI_DIAG_FREEZE_RX_METRICS 0
#endif
#if UI_DIAG_FREEZE_RX_METRICS != 0 && UI_DIAG_FREEZE_RX_METRICS != 1
#error "UI_DIAG_FREEZE_RX_METRICS must be zero or one"
#endif

typedef struct {
    uint32_t stats_calls;
    uint32_t stats_us;
    uint32_t max_stats_us;
    uint32_t frozen;
    uint32_t frozen_rx_metrics;
} UiDiagnostics;

void ui_get_diagnostics(UiDiagnostics *diagnostics);
void ui_reset_diagnostics(void);

// 字体
LV_FONT_DECLARE(lv_font_cansight_14);

// 初始化UI界面
void ui_init(void);

// 注册归档请求接口；回调仅提交请求，文件操作由主循环执行。
typedef void (*ui_archive_callback_t)(void);
void ui_set_archive_callback(ui_archive_callback_t callback);

#endif
