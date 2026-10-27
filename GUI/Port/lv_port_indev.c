// GT911 到 LVGL 的触摸适配；由 LVGL 在主循环轮询输入，未使用触摸中断来执行 I2C 读取。
#include "lv_port_indev.h"
#include "gt911.h"
#include <stdint.h>

#define TOUCH_LCD_WIDTH  800U
#define TOUCH_LCD_HEIGHT 480U

// 触摸输入设备
static lv_indev_t *touch_indev;
static uint32_t touch_io_errors;  // 状态、坐标读取或清状态失败的累计次数，不等于丢失触点数。

// 主循环诊断查询：返回本次启动以来触摸 I2C 访问失败的累计次数，不清零。
uint32_t lv_port_indev_get_io_errors(void) { return touch_io_errors; }

// 记录最后一次有效坐标，松开时LVGL仍需要最近坐标
static lv_point_t last_point = {0, 0};
static uint8_t last_pressed;  // 最近一次有效的按下/松开状态，用于没有新采样时维持输入。
static uint32_t last_touch_tick;  // 最近有效按下坐标的毫秒时间，超时则报告松开。

// LVGL 输入回调在主循环调用：读取 GT911 状态和首触点，转换坐标后填入 data。
// I2C 失败时报告松开并保留最近坐标；没有新采样时短暂维持状态，不在这里更新业务或画面。
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
        touch_io_errors++;
        last_pressed = 0U;
        data->state = LV_INDEV_STATE_RELEASED;  // 触摸状态读取失败
        data->point= last_point;  // 返回上一次触摸点
        return;
    }

    // ready 位未置位只表示暂无新采样，不代表手指已经松开。
    if((status & 0x80U) == 0U) 
    { 
        // 超过 100 ms 没有有效按下采样时释放，避免状态卡在持续按下。
        if(HAL_GetTick() - last_touch_tick >= 100U) last_pressed = 0U;
        data->state = last_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        data->point = last_point; // 返回上一次触摸点
        return;
    }

    // 状态低四位为触点数量；数量为零时清状态并报告松开。
    touch_count = status & 0x0FU;

    if(touch_count == 0U)
    {
        last_pressed = 0U;
        // 数据已读到，但没有触点：清状态并报告松开
        if(gt911_send_data(GT_GSTID_REG, &clear_status, 1) != HAL_OK) touch_io_errors++;
        data->state = LV_INDEV_STATE_RELEASED;  // 触摸状态为松开
        data->point = last_point;  // 返回上一次触摸点
        return;
    }

    // LVGL指针设备先使用第一个触点
    if (gt911_read_data(GT_TP1_REG, point_data, 4) != HAL_OK)
    {
        touch_io_errors++;
        last_pressed = 0U;
        data->state = LV_INDEV_STATE_RELEASED;
        data->point = last_point;
        return;
    }

    // 告诉GT911本次状态已处理，允许它报告下一次触摸变化
    if(gt911_send_data(GT_GSTID_REG, &clear_status, 1) != HAL_OK) touch_io_errors++;

    // GT911 每个坐标按低字节在前、高字节在后的格式返回
    raw_x = (uint16_t)point_data[0] | ((uint16_t)point_data[1] << 8);
    raw_y = (uint16_t)point_data[2] |((uint16_t)point_data[3] << 8);

    // 当前横屏映射：x = 799 - 原始 y，y = 原始 x；异常坐标被限制到屏幕边界。
    screen_x = (raw_y < TOUCH_LCD_WIDTH)? (uint16_t)(TOUCH_LCD_WIDTH - 1U - raw_y): 0U;
    screen_y = (raw_x < TOUCH_LCD_HEIGHT)? raw_x : (TOUCH_LCD_HEIGHT - 1U);

    last_point.x = (lv_coord_t)screen_x;  
    last_point.y = (lv_coord_t)screen_y;  
    last_pressed = 1U;
    last_touch_tick = HAL_GetTick();

    data->point = last_point;  // 触摸点
    data->state = LV_INDEV_STATE_PRESSED;  //按下
}

// 启动时调用一次：建立指针类型输入设备并注册读取回调，后续由 LVGL 定期查询。
void lv_port_indev_init(void)
{
    touch_indev = lv_indev_create();  //创建一个输入设备对象
    lv_indev_set_type(touch_indev, LV_INDEV_TYPE_POINTER);  //设置输入设备的类型为指针
    lv_indev_set_read_cb(touch_indev, gt911_read_cb);  //设置输入设备的读取回调函数为gt911_read_cb
}
