#include "lv_port_disp.h"
#include "lcd.h"
#include "main.h"
#include "can_timestamp.h"
#include <stdint.h>

#define LCD_HOR_RES 800
#define LCD_VER_RES 480
#define LCD_BUF_SIZE (LCD_HOR_RES *10)

// 静态分配缓冲区
static uint16_t draw_buffer[LCD_BUF_SIZE];
static lv_display_t *lcd_display;
static LvPortDispMetrics display_metrics;
static LvPortInvalidArea invalid_areas[LV_PORT_INVALID_AREA_LIMIT];
static uint32_t invalid_area_count;
static uint32_t refresh_started;
static uint32_t render_started;
static uint32_t render_flush_started;
static uint8_t refresh_active;
static uint8_t render_active;

void lv_port_disp_reset_metrics(void)
{
    display_metrics = (LvPortDispMetrics){0};
    invalid_area_count = 0U;
    refresh_active = render_active = 0U;
}

uint32_t lv_port_disp_get_invalid_areas(LvPortInvalidArea *areas, uint32_t capacity)
{
    if(areas == NULL) return 0U;
    uint32_t count = invalid_area_count < capacity ? invalid_area_count : capacity;
    for(uint32_t i = 0U; i < count; i++) areas[i] = invalid_areas[i];
    return count;
}

// 事件内只读取区域和维护固定大小记录，不修改失效范围，不分配内存或输出串口。
static void record_invalid_area(const lv_area_t *area)
{
    if(area == NULL || area->x2 < area->x1 || area->y2 < area->y1) return;
    uint32_t pixels = (uint32_t)(area->x2 - area->x1 + 1) *
                      (uint32_t)(area->y2 - area->y1 + 1);
    display_metrics.invalid_requests++;
    display_metrics.invalid_pixels += pixels;
    uint32_t smallest = 0U;
    for(uint32_t i = 0U; i < invalid_area_count; i++) {
        const lv_area_t *saved = &invalid_areas[i].area;
        if(saved->x1 == area->x1 && saved->y1 == area->y1 &&
           saved->x2 == area->x2 && saved->y2 == area->y2) {
            invalid_areas[i].hits++;
            return;
        }
        if(invalid_areas[i].pixels < invalid_areas[smallest].pixels) smallest = i;
    }
    uint32_t slot;
    if(invalid_area_count < LV_PORT_INVALID_AREA_LIMIT) slot = invalid_area_count++;
    else {
        if(pixels <= invalid_areas[smallest].pixels) return;
        slot = smallest;
    }
    invalid_areas[slot] = (LvPortInvalidArea){.area = *area, .pixels = pixels, .hits = 1U};
}

static void display_diagnostic_cb(lv_event_t *event)
{
    uint32_t now = (uint32_t)CanTimestamp_GetUs();
    switch(lv_event_get_code(event)) {
        case LV_EVENT_INVALIDATE_AREA:
            record_invalid_area((const lv_area_t *)lv_event_get_param(event));
            break;
        case LV_EVENT_REFR_START:
            refresh_started = now;
            refresh_active = 1U;
            break;
        case LV_EVENT_REFR_READY:
            if(refresh_active) display_metrics.refresh_us += now - refresh_started;
            refresh_active = 0U;
            break;
        case LV_EVENT_RENDER_START:
            render_started = now;
            render_flush_started = display_metrics.flush_us;
            render_active = 1U;
            break;
        case LV_EVENT_RENDER_READY:
            if(render_active) {
                uint32_t elapsed = now - render_started;
                uint32_t lcd_us = display_metrics.flush_us - render_flush_started;
                display_metrics.render_us += elapsed;
                display_metrics.render_cpu_us += elapsed > lcd_us ? elapsed - lcd_us : 0U;
            }
            render_active = 0U;
            break;
        default: break;
    }
}

void lv_port_disp_get_metrics(LvPortDispMetrics *metrics)
{
    if(metrics != NULL) *metrics = display_metrics;
}

// LVGL 通过这个函数获取毫秒计时
static uint32_t lv_port_get_tick(void)
{
    return HAL_GetTick();
}

//LVGL要求刷新的区域坐标右下角也包含在区域内
static void lcd_flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *pixel_map)
{
    uint32_t started = (uint32_t)CanTimestamp_GetUs();
    LCD_Color_Fill((uint16_t)area->x1, (uint16_t)area->y1, (uint16_t)area->x2, (uint16_t)area->y2, (uint16_t *)pixel_map);
    display_metrics.flush_us += (uint32_t)CanTimestamp_GetUs() - started;
    display_metrics.flush_calls++;
    display_metrics.pixels += (uint32_t)(area->x2 - area->x1 + 1) *
                             (uint32_t)(area->y2 - area->y1 + 1);
    // 仅统计 LCD 传输，软件渲染和布局耗时由主循环作差区分。
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
    lv_display_add_event_cb(lcd_display, display_diagnostic_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_display_add_event_cb(lcd_display, display_diagnostic_cb, LV_EVENT_REFR_START, NULL);
    lv_display_add_event_cb(lcd_display, display_diagnostic_cb, LV_EVENT_REFR_READY, NULL);
    lv_display_add_event_cb(lcd_display, display_diagnostic_cb, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(lcd_display, display_diagnostic_cb, LV_EVENT_RENDER_READY, NULL);

    // 注册显示刷新回调函数，当LVGL需要刷新屏幕时调用lcd_flush_cb
    lv_display_set_flush_cb(lcd_display, lcd_flush_cb);

    //配置显示缓冲区，使用部分刷新模式
    lv_display_set_buffers(lcd_display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);

    // 使用HAL层的毫秒时钟作为LVGL系统时基，用于动画和时序计算
    lv_tick_set_cb(lv_port_get_tick);
}
