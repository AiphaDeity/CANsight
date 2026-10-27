#include "can_timestamp.h"
#include "tim.h"

// TIM2 的软件高 32 位：每次处理硬件计数溢出加 1，主循环和接收中断读取。
static volatile uint32_t timestamp_high;

// main 在 CAN 接收前调用一次：清零高低位和更新标志，再开启 TIM2 更新中断。
// 返回 1 表示计时启动成功，0 表示 HAL 启动失败；运行中不要重复调用，以免时间倒退。
uint8_t CanTimestamp_Start(void)
{
    timestamp_high = 0U;
    __HAL_TIM_SET_COUNTER(&htim2, 0U);
    __HAL_TIM_CLEAR_FLAG(&htim2, TIM_FLAG_UPDATE);
    HAL_NVIC_SetPriority(TIM2_IRQn, 0U, 0U);
    HAL_NVIC_EnableIRQ(TIM2_IRQn);
    return HAL_TIM_Base_Start_IT(&htim2) == HAL_OK;
}

// TIM2 中断入口调用：仅在更新标志和更新中断均有效时累计一次溢出并清标志。
// 与 CAN RX0 同抢占优先级，读取函数另用短临界区保证高低位快照一致。
void CanTimestamp_IRQHandler(void)
{
    if(__HAL_TIM_GET_FLAG(&htim2, TIM_FLAG_UPDATE) != RESET &&
       __HAL_TIM_GET_IT_SOURCE(&htim2, TIM_IT_UPDATE) != RESET) {
        timestamp_high++;
        __HAL_TIM_CLEAR_IT(&htim2, TIM_IT_UPDATE);
    }
}

// 主循环或中断调用：读取软件高 32 位和 TIM2 硬件低 32 位，合成 64 位微秒时间。
// 临时屏蔽可屏蔽中断，避免溢出处理在两次读取之间修改高位；TIM2 硬件仍继续计数。
uint64_t CanTimestamp_GetUs(void)
{
    // 保存调用前的中断屏蔽状态：PRIMASK 为 0 表示未屏蔽，为 1 表示已屏蔽。
    // 退出时恢复原状态，不能无条件开中断，否则会破坏调用方已有的临界区。
    uint32_t mask = __get_PRIMASK();
    __disable_irq();

    // timestamp_high 保存已处理的溢出次数；TIM2 从 0xFFFFFFFF 回到 0 时产生更新事件。
    uint32_t high = timestamp_high;
    uint32_t low = __HAL_TIM_GET_COUNTER(&htim2);

    // 关中断不会停止定时器；更新标志非零，说明已有溢出尚未由中断计入高位。
    if(__HAL_TIM_GET_FLAG(&htim2, TIM_FLAG_UPDATE) != RESET) {
        // 只补偿本次读取的局部高位，不修改 timestamp_high，也不清除更新标志。
        // 后续溢出中断仍会正常更新全局高位，避免这里和中断重复累计同一次溢出。
        high++;

        // 溢出可能发生在刚才读取 low 之后，重新读取以取得与补偿后高位对应的低位。
        low = __HAL_TIM_GET_COUNTER(&htim2);
    }

    // 高低位已保存到局部变量，恢复调用前的中断状态，结束短临界区。
    __set_PRIMASK(mask);

    // 先转成 64 位再左移 32 位，腾出低位空间；按位或拼接 TIM2 计数。
    // 结果等于 high * 2^32 + low；TIM2 每微秒加 1，因此结果单位为 us。
    return ((uint64_t)high << 32) | low;
}
