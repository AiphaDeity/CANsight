#ifndef LV_PORT_DISP_H
#define LV_PORT_DISP_H

#include "lvgl.h"
#include <stdint.h>

// 累计值按 32 位自然回绕，主循环以相邻快照之差计算单次 LVGL 调用的传输开销。
typedef struct {
    uint32_t flush_calls;
    uint32_t flush_us;
    uint32_t pixels;
} LvPortDispMetrics;

// 仅供主循环读取，刷新回调与查询不并发执行。
void lv_port_disp_get_metrics(LvPortDispMetrics *metrics);

// 初始化 LCD 并向 LVGL 注册显示设备。
void lv_port_disp_init(void);

#endif 