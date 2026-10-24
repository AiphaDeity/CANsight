#ifndef LV_PORT_INDEV_H
#define LV_PORT_INDEV_H

#include "lvgl.h"
#include <stdint.h>

// io_errors 始终记录；回调次数和耗时仅在详细诊断开启时记录。
typedef struct {
    uint32_t callbacks;
    uint32_t read_us;
    uint32_t max_read_us;
    uint32_t io_errors;
} LvPortIndevMetrics;

void lv_port_indev_get_metrics(LvPortIndevMetrics *metrics);

// 注册GT911电容触摸输入设备
void lv_port_indev_init(void);

#endif
