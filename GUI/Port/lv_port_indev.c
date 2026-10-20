#include "lv_port_indev.h"
#include "gt911.h"
#include <stdint.h>

#define TOUCH_LCD_WIDTH  800U
#define TOUCH_LCD_HEIGHT 480U

// 触摸输入设备
static lv_indev_t *touch_indev;

// 记录最后一次有效坐标，松开时LVGL仍需要最近坐标
static lv_point_t last_point = {0, 0};

static void gt911_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
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
        data->state = LV_INDEV_STATE_RELEASED;  // 触摸状态读取失败
        data->point= last_point;  // 返回上一次触摸点
        return;
    }

    // 触摸点未按下
    if((status & 0x80U) == 0U) 
    { 
        data->state = LV_INDEV_STATE_RELEASED; // 触摸状态为松开
        data->point = last_point; // 返回上一次触摸点
        return;
    }

    // 触摸点按下
    touch_count = status & 0x0FU;

    if(touch_count == 0U)
    {
        // 数据已读到，但没有触点：清状态并报告松开
        (void)gt911_send_data(GT_GSTID_REG, &clear_status, 1);  // 清除触摸状态
        data->state = LV_INDEV_STATE_RELEASED;  // 触摸状态为松开
        data->point = last_point;  // 返回上一次触摸点
        return;
    }

    //LVGL指针设备先使用第一个触点
    if (gt911_read_data(GT_TP1_REG, point_data, 4) != HAL_OK)
    {
        data->state = LV_INDEV_STATE_RELEASED;
        data->point = last_point;
        return;
    }

    // 告诉GT911本次状态已处理，允许它报告下一次触摸变化
    (void)gt911_send_data(GT_GSTID_REG, &clear_status, 1);

    // GT911 每个坐标按低字节在前、高字节在后的格式返回
    raw_x = (uint16_t)point_data[0] | ((uint16_t)point_data[1] << 8);
    raw_y = (uint16_t)point_data[2] |((uint16_t)point_data[3] << 8);

    //采用横屏坐标变换：屏幕X=800-原始Y；屏幕Y=原始X
    screen_x = (raw_y < TOUCH_LCD_WIDTH)? (uint16_t)(TOUCH_LCD_WIDTH - 1U - raw_y): 0U;
    screen_y = (raw_x < TOUCH_LCD_HEIGHT)? raw_x : (TOUCH_LCD_HEIGHT - 1U);

    last_point.x = (lv_coord_t)screen_x;  
    last_point.y = (lv_coord_t)screen_y;  

    data->point = last_point;  // 触摸点
    data->state = LV_INDEV_STATE_PRESSED;  //按下
}

void lv_port_indev_init(void)
{
    touch_indev = lv_indev_create();  //创建一个输入设备对象
    lv_indev_set_type(touch_indev, LV_INDEV_TYPE_POINTER);  //设置输入设备的类型为指针
    lv_indev_set_read_cb(touch_indev, gt911_read_cb);  //设置输入设备的读取回调函数为gt911_read_cb
}
