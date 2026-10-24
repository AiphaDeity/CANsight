#include "lv_port_disp.h"
#include "lcd.h"
#include "main.h"
#include <stdint.h>

#define LCD_HOR_RES 800
#define LCD_VER_RES 480
#define LCD_BUF_SIZE (LCD_HOR_RES *10)

// 静态分配缓冲区
static uint16_t draw_buffer[LCD_BUF_SIZE];
static lv_display_t *lcd_display;
// LVGL 通过这个函数获取毫秒计时
static uint32_t lv_port_get_tick(void)
{
    return HAL_GetTick();
}

//LVGL要求刷新的区域坐标右下角也包含在区域内
static void lcd_flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *pixel_map)
{
    LCD_Color_Fill((uint16_t)area->x1, (uint16_t)area->y1, (uint16_t)area->x2, (uint16_t)area->y2, (uint16_t *)pixel_map);
    // 当前像素传输同步完成后通知 LVGL 复用绘图缓冲。
    lv_display_flush_ready(display);
}


//初始化LVGL图形库的显示系统
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

    //配置显示缓冲区，使用部分刷新模式
    lv_display_set_buffers(lcd_display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);

    // 使用HAL层的毫秒时钟作为LVGL系统时基，用于动画和时序计算
    lv_tick_set_cb(lv_port_get_tick);
}
