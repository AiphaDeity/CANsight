#ifndef LV_PORT_DISP_H
#define LV_PORT_DISP_H

#include "lvgl.h"
#include <stdint.h>

// 详细诊断开启时记录，关闭时查询返回 0；累计值按 32 位自然回绕。
typedef struct {
    uint32_t flush_calls;
    uint32_t flush_us;
    uint32_t pixels;
    uint32_t refresh_us;       // REFR_START 到 REFR_READY，包含布局、渲染和同步传输。
    uint32_t render_us;        // RENDER_START 到 RENDER_READY，包含同步 LCD 传输。
    uint32_t render_cpu_us;    // 渲染事件区间减去其中的 LCD 传输，仍包含中断占用。
    uint32_t invalid_requests; // 失效请求事件数，未去重，不是最终刷新区域数。
    uint32_t invalid_pixels;   // 请求面积累计值，可重复、重叠，不能当作实际刷新面积。
} LvPortDispMetrics;

#define LV_PORT_INVALID_AREA_LIMIT 8U
// 保留面积最大的八种不同请求矩形；hits 仅统计该条目保留期间的命中数。
typedef struct {
    lv_area_t area;
    uint32_t pixels;
    uint32_t hits;
} LvPortInvalidArea;

uint32_t lv_port_disp_get_invalid_areas(LvPortInvalidArea *areas, uint32_t capacity);
// 仅在 CAN 启动前、首次同步绘制完成后清零，运行期间不重置快照差分基准。
void lv_port_disp_reset_metrics(void);

// 仅供主循环读取，刷新回调与查询不并发执行。
void lv_port_disp_get_metrics(LvPortDispMetrics *metrics);

// 初始化 LCD 并向 LVGL 注册显示设备。
void lv_port_disp_init(void);

#endif 
