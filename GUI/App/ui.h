#ifndef UI_H_
#define UI_H_ 

#include "lvgl.h"

// 字体
LV_FONT_DECLARE(lv_font_cansight_14);

// 初始化UI界面
void ui_init(void);

// 注册归档请求接口；回调仅提交请求，文件操作由主循环执行。
typedef void (*ui_archive_callback_t)(void);
void ui_set_archive_callback(ui_archive_callback_t callback);

#endif
