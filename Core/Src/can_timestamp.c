#include "can_timestamp.h"
#include "tim.h"

static volatile uint32_t timestamp_high;

// 在 CAN 接收启动前建立时间基准，清除初始化产生的更新标志。
uint8_t CanTimestamp_Start(void)
{
    timestamp_high = 0U;
    __HAL_TIM_SET_COUNTER(&htim2, 0U);
    __HAL_TIM_CLEAR_FLAG(&htim2, TIM_FLAG_UPDATE);
    HAL_NVIC_SetPriority(TIM2_IRQn, 0U, 0U);
    HAL_NVIC_EnableIRQ(TIM2_IRQn);
    return HAL_TIM_Base_Start_IT(&htim2) == HAL_OK;
}

// 与 CAN RX0 使用相同抢占优先级，避免读取中途进入本溢出处理函数。
void CanTimestamp_IRQHandler(void)
{
    if(__HAL_TIM_GET_FLAG(&htim2, TIM_FLAG_UPDATE) != RESET &&
       __HAL_TIM_GET_IT_SOURCE(&htim2, TIM_IT_UPDATE) != RESET) {
        timestamp_high++;
        __HAL_TIM_CLEAR_IT(&htim2, TIM_IT_UPDATE);
    }
}

// 原子读取高低位；溢出已发生但 IRQ 尚未受理时，在快照中补计一次。
uint64_t CanTimestamp_GetUs(void)
{
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    uint32_t high = timestamp_high;
    uint32_t low = __HAL_TIM_GET_COUNTER(&htim2);
    if(__HAL_TIM_GET_FLAG(&htim2, TIM_FLAG_UPDATE) != RESET) {
        high++;
        low = __HAL_TIM_GET_COUNTER(&htim2);
    }
    __set_PRIMASK(mask);
    return ((uint64_t)high << 32) | low;
}
