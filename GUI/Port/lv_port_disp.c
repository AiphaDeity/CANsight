// LVGL 显示适配：把软件绘制结果交给 LCD 板级驱动，当前由 CPU 经 FSMC 同步写像素。
#include "lv_port_disp.h"
#include "lcd.h"
#include "main.h"
#include <stdint.h>

#define LCD_HOR_RES 800
#define LCD_VER_RES 480
#define LCD_BUF_SIZE (LCD_HOR_RES *10)

// 800 × 10 个 RGB565 像素，共 16000 字节；只是部分绘图缓冲，不是整屏帧缓冲。
static uint16_t draw_buffer[LCD_BUF_SIZE];
static lv_display_t *lcd_display;  // LVGL 显示设备对象，由初始化阶段创建并持有。
// LVGL 在主循环上下文调用的时基回调：返回 HAL 的毫秒计数，不是 TIM2 微秒时间。
static uint32_t lv_port_get_tick(void)
{
    return HAL_GetTick();
}

// LVGL 刷新回调：area 为包含右下角的矩形，pixel_map 为当前块的 RGB565 像素。
// LCD_Color_Fill 同步完成后才调用 flush_ready，通知 LVGL 可以复用该绘图缓冲。
static void lcd_flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *pixel_map)
{
    LCD_Color_Fill((uint16_t)area->x1, (uint16_t)area->y1, (uint16_t)area->x2, (uint16_t)area->y2, (uint16_t *)pixel_map);
    // 当前像素传输同步完成后通知 LVGL 复用绘图缓冲。
    lv_display_flush_ready(display);
}


// 启动时调用一次：初始化 LCD，创建显示设备，注册刷新回调、部分缓冲和毫秒时基。
// 后续绘制由 LVGL 调度，不在每帧 CAN 接收时重建显示对象。
void lv_port_disp_init(void)
{
    // 初始化LCD
    LCD_Init();

    // 创建LCD显示器
    lcd_display = lv_display_create(LCD_HOR_RES, LCD_VER_RES);

    // 设置显示颜色格式为RGB565
    lv_display_set_color_format(lcd_display, LV_COLOR_FORMAT_RGB565);

    // 注册显示刷新回调函数，当LVGL需要刷新屏幕时调用lcd_flush_cb
    lv_display_set_flush_cb(lcd_display, lcd_flush_cb);

    // PARTIAL 模式按小块绘制并传输；第二缓冲为 NULL，当前没有双缓冲或 LCD DMA。
    lv_display_set_buffers(lcd_display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);

    // 使用HAL层的毫秒时钟作为LVGL系统时基，用于动画和时序计算
    lv_tick_set_cb(lv_port_get_tick);
}
