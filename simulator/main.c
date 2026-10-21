#include <stdint.h>

#include "lvgl.h"
#include "lvgl/drivers/windows/lv_windows_display.h"
#include "lvgl/drivers/windows/lv_windows_input.h"
#include "ui.h"
#include "can_stats.h"

/**
 * @brief 生成本机模拟报文并更新共用统计模块。
 * @note IDE=4、RTR=2 分别对应 STM32 HAL 的扩展帧和远程帧标志。
 *       本功能不依赖 MCU 连接，桌面任务调度不保证精确发送周期。
 */
static void simulate_can_cb(lv_timer_t *timer)
{
    (void)timer;
    static CanFrame frames[] = {
        {.id=0x7DF, .dlc=8, .data={2,1,12}},
        {.id=0x18FF5000, .ide=4, .dlc=8},
        {.id=0x321, .dlc=2, .data={0xA5,0x5A}},
        {.id=0x18FEF100, .ide=4, .dlc=8, .data={255,255,255,0,18,52,86,120}},
        {.id=0x123, .dlc=4, .data={1,2,3,4}},
        {.id=0x123, .ide=4, .dlc=4, .data={10,11,12,13}},
        {.id=0x237, .dlc=8, .data={18,39,101,116,131,146,135,51}},
        {.id=0x7E8, .dlc=8, .data={4,65,12,26,248}},
        {.id=0x123, .rtr=2, .dlc=4}
    };
    // 各模拟报文的生成间隔，单位为毫秒，索引与 frames 保持一致。
    static const uint32_t intervals_ms[] = {1000,10,20,50,100,200,500,1000,500};
    uint32_t now_us = lv_tick_get() * 1000U;  // 转换为与固件一致的 32 位微秒时间戳。
    for(unsigned i = 0; i < sizeof(frames) / sizeof(frames[0]); i++) {
        if(now_us - frames[i].timestamp_us >= intervals_ms[i] * 1000U) {
            // 到期后生成一帧并更新基准时间，不补发调度延迟期间的报文。
            frames[i].timestamp_us = now_us;
            CanStats_AddFrame(&frames[i]);
        }
    }
}

int main(void)
{
    lv_init();

    // 创建 800×480 的 Windows 窗口，对应板载 LCD 分辨率
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

    // 使用电脑鼠标模拟触摸输入
    if(lv_windows_acquire_pointer_indev(display) == NULL) {
        return 2;
    }

    // 调用与 STM32 固件共用的界面
    CanStats_Init();  // 初始化共用统计模块。
    // 模拟器不读取硬件状态；此快照仅用于预览正常运行状态的显示。
    const CanStatsRuntime runtime = {
        .can_state = CAN_STATS_RUNNING,
        .sd_state = CAN_LOG_RECORDING
    };
    CanStats_SetRuntime(&runtime);
    ui_init();
    lv_timer_create(simulate_can_cb, 10, NULL);  // 注册 10 ms 周期的模拟报文调度任务。

    // 持续运行 LVGL 定时器
    while(1) {
        uint32_t wait_ms = lv_timer_handler();

        if(wait_ms == LV_NO_TIMER_READY) {
            wait_ms = LV_DEF_REFR_PERIOD;
        }

        lv_sleep_ms(wait_ms);
    }
}
