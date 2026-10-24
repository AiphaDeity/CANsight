#ifndef LV_PORT_INDEV_H
#define LV_PORT_INDEV_H

#include "lvgl.h"
#include <stdint.h>

// 返回本次启动累计的触摸 I2C 访问错误数。
uint32_t lv_port_indev_get_io_errors(void);
void lv_port_indev_init(void);

#endif
