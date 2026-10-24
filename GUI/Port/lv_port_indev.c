#include "lv_port_indev.h"
#include "gt911.h"
#include "can_timestamp.h"
#include "cansight_config.h"
#include <stdint.h>

#define TOUCH_LCD_WIDTH  800U
#define TOUCH_LCD_HEIGHT 480U

// 触摸输入设备
static lv_indev_t *touch_indev;
static LvPortIndevMetrics touch_metrics;

void lv_port_indev_get_metrics(LvPortIndevMetrics *metrics)
{
    if(metrics != NULL) *metrics = touch_metrics;
}

// 记录最后一次有效坐标，松开时LVGL仍需要最近坐标
static lv_point_t last_point = {0, 0};
static uint8_t last_pressed;
static uint32_t last_touch_tick;

static void gt911_sample(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint8_t status = 0U;
    uint8_t point_data[4] = {0U};
    uint8_t clear_status = 0U;
    uint8_t touch_count;
    uint16_t raw_x;
    uint16_t raw_y;
    uint16_t screen_x;
    uint16_t screen_y;

    // 消除编译器未使用参数警告
    (void)indev;

    // 读取触摸状态
    if(gt911_read_data(GT_GSTID_REG, &status, 1) != HAL_OK)
    {
        touch_metrics.io_errors++;
        last_pressed = 0U;
        data->state = LV_INDEV_STATE_RELEASED;  // 触摸状态读取失败
        data->point= last_point;  // 返回上一次触摸点
        return;
    }

    // ready 位未置位只表示暂无新采样，不代表手指已经松开。
    if((status & 0x80U) == 0U) 
    { 
        if(HAL_GetTick() - last_touch_tick >= 100U) last_pressed = 0U;
        data->state = last_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        data->point = last_point; // 返回上一次触摸点
        return;
    }

    // 触摸点按下
    touch_count = status & 0x0FU;

    if(touch_count == 0U)
    {
        last_pressed = 0U;
        // 数据已读到，但没有触点：清状态并报告松开
        if(gt911_send_data(GT_GSTID_REG, &clear_status, 1) != HAL_OK) touch_metrics.io_errors++;
        data->state = LV_INDEV_STATE_RELEASED;  // 触摸状态为松开
        data->point = last_point;  // 返回上一次触摸点
        return;
    }

    //LVGL指针设备先使用第一个触点
    if (gt911_read_data(GT_TP1_REG, point_data, 4) != HAL_OK)
    {
        touch_metrics.io_errors++;
        last_pressed = 0U;
        data->state = LV_INDEV_STATE_RELEASED;
        data->point = last_point;
        return;
    }

    // 告诉GT911本次状态已处理，允许它报告下一次触摸变化
    if(gt911_send_data(GT_GSTID_REG, &clear_status, 1) != HAL_OK) touch_metrics.io_errors++;

    // GT911 每个坐标按低字节在前、高字节在后的格式返回
    raw_x = (uint16_t)point_data[0] | ((uint16_t)point_data[1] << 8);
    raw_y = (uint16_t)point_data[2] |((uint16_t)point_data[3] << 8);

    //采用横屏坐标变换：屏幕X=800-原始Y；屏幕Y=原始X
    screen_x = (raw_y < TOUCH_LCD_WIDTH)? (uint16_t)(TOUCH_LCD_WIDTH - 1U - raw_y): 0U;
    screen_y = (raw_x < TOUCH_LCD_HEIGHT)? raw_x : (TOUCH_LCD_HEIGHT - 1U);

    last_point.x = (lv_coord_t)screen_x;  
    last_point.y = (lv_coord_t)screen_y;  
    last_pressed = 1U;
    last_touch_tick = HAL_GetTick();

    data->point = last_point;  // 触摸点
    data->state = LV_INDEV_STATE_PRESSED;  //按下
}

// 包装原有输入流程，保留全部失败出口及坐标变换，不在回调中打印。
static void gt911_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
#if CANSIGHT_DETAILED_DIAGNOSTICS
    uint32_t started = (uint32_t)CanTimestamp_GetUs();
#endif
    gt911_sample(indev, data);
#if CANSIGHT_DETAILED_DIAGNOSTICS
    uint32_t elapsed = (uint32_t)CanTimestamp_GetUs() - started;
    touch_metrics.callbacks++;
    touch_metrics.read_us += elapsed;
    if(elapsed > touch_metrics.max_read_us) touch_metrics.max_read_us = elapsed;
#endif
}

void lv_port_indev_init(void)
{
    touch_indev = lv_indev_create();  //创建一个输入设备对象
    lv_indev_set_type(touch_indev, LV_INDEV_TYPE_POINTER);  //设置输入设备的类型为指针
    lv_indev_set_read_cb(touch_indev, gt911_read_cb);  //设置输入设备的读取回调函数为gt911_read_cb
}
