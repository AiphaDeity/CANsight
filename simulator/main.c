#include <stdint.h>

#include "lvgl.h"
#include "lvgl/drivers/windows/lv_windows_display.h"
#include "lvgl/drivers/windows/lv_windows_input.h"
#include "ui.h"

int main(void)
{
    lv_init();

    /* 创建 800×480 的 Windows 窗口，对应板载 LCD 分辨率 */
    lv_display_t * display = lv_windows_create_display(
        L"CanSight LVGL 9.6 Simulator",
        800,
        480,
        100,
        false,
        true);

    if(display == NULL)  // 创建失败
    {
        return 1;
    }

    /* 使用电脑鼠标模拟触摸输入 */
    if(lv_windows_acquire_pointer_indev(display) == NULL) {
        return 2;
    }

    /* 调用与 STM32 固件共用的界面 */
    ui_init();

    /* 持续运行 LVGL 定时器 */
    while(1) {
        uint32_t wait_ms = lv_timer_handler();

        if(wait_ms == LV_NO_TIMER_READY) {
            wait_ms = LV_DEF_REFR_PERIOD;
        }

        lv_sleep_ms(wait_ms);
    }
}
