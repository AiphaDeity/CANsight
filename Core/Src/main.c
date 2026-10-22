/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Main program body
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2026 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "can.h"
#include "dma.h"
#include "fatfs.h"
#include "i2c.h"
#include "sdio.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"
#include "fsmc.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "stdio.h"
#include "can_logger.h"
#include "can_timestamp.h"
#include "can_stats.h"
#include "log_player.h"
#include "lv_port_disp.h"
#include "lv_port_indev.h"
#include "gt911.h"
#include <lvgl/core/lv_obj_pos.h>
#include "ui.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
// 保留一个槽位区分空与满，实际可用容量为 511 帧。
#define CAN_RX_QUEUE_SIZE 512U
// 每批同时限制帧数与处理时间，避免持续来帧独占主循环。
#define CAN_RX_BATCH_SIZE 256U
#define CAN_RX_BATCH_BUDGET_US 2000U
#define CAN_UI_INTERVAL_MS 5U
#define CAN_HISTORY_INTERVAL_MS 5U
#define CAN_HISTORY_QUEUE_LIMIT 128U
// 接收停止且队列排空后输出一次摘要，避免发送期间的周期串口阻塞。
#define CAN_DIAGNOSTIC_IDLE_MS 1500U
#if (CAN_RX_QUEUE_SIZE < 2U) || ((CAN_RX_QUEUE_SIZE & (CAN_RX_QUEUE_SIZE - 1U)) != 0U)
#error "CAN_RX_QUEUE_SIZE must be a power of two and at least two"
#endif
#if CAN_RX_QUEUE_SIZE > 65536U
#error "CAN RX queue indices require a wider type"
#endif
#ifndef CAN_RX_DEBUG_PRINT
// 逐帧串口诊断输出开关：1 为启用，0 为禁用；默认禁用以降低阻塞开销。
#define CAN_RX_DEBUG_PRINT 0
#endif

#define CAN_RX_QUEUE_MASK (CAN_RX_QUEUE_SIZE - 1U) // 掩码，用于循环队列索引
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

// 保存 CAN 接收报文的环形队列
static CanFrame canRxQueue[CAN_RX_QUEUE_SIZE];

// head 是中断的写入位置；tail 是主循环的读取位置
static volatile uint16_t canRxQueueHead = 0U;
static volatile uint16_t canRxQueueTail = 0U;

// 软件队列满时，无法入队的报文数量
static volatile uint32_t canRxQueueOverflow = 0U;

//成功从CAN硬件FIFO0取出的帧数
static volatile uint32_t canRxFramesReadFromFifo = 0U;

//CAN硬件FIFO0溢出的事件数
static volatile uint32_t canRxFifo0OverrunEvents = 0U;

//HAL 报告过的 CAN 错误位集合，便于调试时查看
static volatile uint32_t canHalErrorFlags = 0U;
static uint8_t canStartupFailed = 0U;  // 初始化、过滤器配置或启动失败标志。
static volatile uint32_t canRxQueuePeak;  // 由生产者更新，避免读改写竞争。
static uint32_t canMaxServiceGapUs;
static uint32_t canMaxBatchUs;
static uint32_t canMaxUiUs;
static uint32_t canMaxHistoryUs;
// 保存同一次最慢 LVGL 调用内的分项耗时，避免将不同调用的峰值相加。
static uint32_t canUiPeakLcdUs;
static uint32_t canUiPeakTouchUs;
static uint32_t canUiPeakOtherUs;
static uint32_t canUiPeakFlushes;
static uint32_t canUiPeakPixels;
static uint32_t canUiPeakRefreshUs;
static uint32_t canUiPeakRenderUs;
static uint32_t canUiPeakRenderCpuUs;
static uint32_t canUiPeakRefreshOtherUs;
static uint32_t canUiPeakStatsUs;
static uint32_t canUiPeakHandlerOtherUs;
static uint32_t canUiPeakInvalidRequests;
static uint32_t canUiPeakInvalidPixels;
static uint32_t canUiCalls;
static uint64_t canUiTotalUs;
// 固定耗时区间，不保存逐次样本；各项互斥，均统计完整 LVGL 调用。
static uint32_t canUiHistogram[6];

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/**
 * @brief 将控制器状态、日志状态及接收异常计数发布至界面数据层。
 * @note 错误历史与当前状态分别维护，历史错误不阻止正常状态恢复。
 */
static void CAN_PublishRuntime(void)
{
  CanStatsRuntime runtime = {0};  // 运行时数据
  HAL_CAN_StateTypeDef state = HAL_CAN_GetState(&hcan1);  // 获取控制器状态 
  uint32_t esr = READ_REG(hcan1.Instance->ESR);  // 获取错误状态寄存器

  if(canStartupFailed != 0U || state == HAL_CAN_STATE_ERROR ||
     (esr & (CAN_ESR_EWGF | CAN_ESR_EPVF | CAN_ESR_BOFF)) != 0U) {
    runtime.can_state = CAN_STATS_ERROR;
  }
  else if(state == HAL_CAN_STATE_LISTENING)
  {
    runtime.can_state = CAN_STATS_RUNNING;
  }
  else {
    runtime.can_state = CAN_STATS_STOPPED;
  }

  // 对齐的 32 位计数可直接读取；各项是相邻时刻的独立快照，不要求原子组合。
  runtime.queue_lost = canRxQueueOverflow;
  runtime.fifo_overruns = canRxFifo0OverrunEvents;
  runtime.error_flags = canHalErrorFlags | HAL_CAN_GetError(&hcan1);
  runtime.sd_state = CanLogger_GetState();
  runtime.sd_error = CanLogger_GetLastError();
  runtime.fifo_frames = canRxFramesReadFromFifo;
  runtime.queue_depth = ((uint32_t)canRxQueueHead - canRxQueueTail) & CAN_RX_QUEUE_MASK;
  runtime.queue_peak = canRxQueuePeak;
  runtime.queue_capacity = CAN_RX_QUEUE_SIZE - 1U;
  runtime.max_service_gap_us = canMaxServiceGapUs;
  runtime.max_batch_us = canMaxBatchUs;
  runtime.max_ui_us = canMaxUiUs;
  runtime.max_history_us = canMaxHistoryUs;
  CanLogger_GetDiagnostics(&runtime.logger);
  runtime.archive_state = CanLogger_GetArchiveState();
  snprintf(runtime.archive_name, sizeof(runtime.archive_name), "%s",
           CanLogger_GetLastArchiveName());
  CanStats_SetRuntime(&runtime);
}

// 仅由主循环在接收静默后调用，所有耗时峰值均取打印之前的快照。
static void CAN_PrintPerformance(void)
{
  CanLoggerDiagnostics log;
  LvPortIndevMetrics touch;
  UiDiagnostics widgets;
  LvPortInvalidArea areas[LV_PORT_INVALID_AREA_LIMIT];
  CanLogger_GetDiagnostics(&log);
  ui_get_diagnostics(&widgets);
  lv_port_indev_get_metrics(&touch);
  printf("CAN PERF: fifo=%lu processed=%lu queue_lost=%lu fifo_overruns=%lu peak=%lu/%lu\r\n",
         (unsigned long)canRxFramesReadFromFifo,
         (unsigned long)CanStats_GetTotalCount(), (unsigned long)canRxQueueOverflow,
         (unsigned long)canRxFifo0OverrunEvents, (unsigned long)canRxQueuePeak,
         (unsigned long)(CAN_RX_QUEUE_SIZE - 1U));
  printf("CAN PERF us: gap=%lu batch=%lu ui=%lu history=%lu write=%lu sync=%lu archive=%lu\r\n",
         (unsigned long)canMaxServiceGapUs, (unsigned long)canMaxBatchUs,
         (unsigned long)canMaxUiUs, (unsigned long)canMaxHistoryUs,
         (unsigned long)log.max_write_us, (unsigned long)log.max_sync_us,
         (unsigned long)log.max_archive_us);
  printf("CAN UI peak: total_us=%lu lcd_us=%lu touch_us=%lu other_us=%lu flushes=%lu pixels=%lu\r\n",
         (unsigned long)canMaxUiUs, (unsigned long)canUiPeakLcdUs,
         (unsigned long)canUiPeakTouchUs, (unsigned long)canUiPeakOtherUs,
         (unsigned long)canUiPeakFlushes, (unsigned long)canUiPeakPixels);
  printf("CAN UI stages us: refresh=%lu render=%lu render_cpu=%lu refresh_other=%lu stats=%lu handler_other=%lu\r\n",
         (unsigned long)canUiPeakRefreshUs, (unsigned long)canUiPeakRenderUs,
         (unsigned long)canUiPeakRenderCpuUs, (unsigned long)canUiPeakRefreshOtherUs,
         (unsigned long)canUiPeakStatsUs, (unsigned long)canUiPeakHandlerOtherUs);
  printf("CAN UI invalid: requests=%lu request_pixels=%lu (may overlap; same peak call)\r\n",
         (unsigned long)canUiPeakInvalidRequests, (unsigned long)canUiPeakInvalidPixels);
  printf("CAN UI stats: freeze=%lu calls=%lu max_us=%lu handler_calls=%lu handler_avg_us=%lu\r\n",
         (unsigned long)widgets.frozen, (unsigned long)widgets.stats_calls,
         (unsigned long)widgets.max_stats_us, (unsigned long)canUiCalls,
         (unsigned long)(canUiCalls ? canUiTotalUs / canUiCalls : 0U));
  printf("CAN UI hist: lt1ms=%lu 1to5ms=%lu 5to20ms=%lu 20to50ms=%lu 50to100ms=%lu ge100ms=%lu\r\n",
         (unsigned long)canUiHistogram[0], (unsigned long)canUiHistogram[1],
         (unsigned long)canUiHistogram[2], (unsigned long)canUiHistogram[3],
         (unsigned long)canUiHistogram[4], (unsigned long)canUiHistogram[5]);
  uint32_t areaCount = lv_port_disp_get_invalid_areas(areas, LV_PORT_INVALID_AREA_LIMIT);
  for(uint32_t i = 0U; i < areaCount; i++) {
    printf("CAN UI area: x1=%ld y1=%ld x2=%ld y2=%ld pixels=%lu hits=%lu\r\n",
           (long)areas[i].area.x1, (long)areas[i].area.y1,
           (long)areas[i].area.x2, (long)areas[i].area.y2,
           (unsigned long)areas[i].pixels, (unsigned long)areas[i].hits);
  }
  printf("CAN TOUCH: callbacks=%lu max_us=%lu io_errors=%lu\r\n",
         (unsigned long)touch.callbacks, (unsigned long)touch.max_read_us,
         (unsigned long)touch.io_errors);
  printf("CAN LOG: accepted=%lu written=%lu synced=%lu buffered=%lu rejected=%lu\r\n",
         (unsigned long)log.accepted_frames, (unsigned long)log.written_frames,
         (unsigned long)log.synced_frames, (unsigned long)log.buffered_frames,
         (unsigned long)log.rejected_frames);
}

// 界面回调仅提交请求，不在 LVGL 事件中直接访问文件系统。
static void CAN_RequestArchive(void)
{
  (void)CanLogger_RequestArchive();
}


/*
生产者函数：由 CAN 接收中断调用
把一帧复制进队列；队列满时立即返回，不等待主循环
*/
static uint8_t CAN_RxQueue_PushFromISR(const CanFrame *frame) {
  uint32_t head = canRxQueueHead;

  // 计算下一个写入位置
  uint32_t nextHead = (head + 1U) & CAN_RX_QUEUE_MASK;

  // 检查队列是否已满
  if (nextHead == canRxQueueTail) {
    // 队列已满，无法入队
    canRxQueueOverflow++;
    return 0U; // 入队失败
  }

  // 将报文复制到队列中
  canRxQueue[head] = *frame;

  // 确保帧内容先写完，再发布新的 head 索引，防止主循环读取到不完整的帧
  __DMB(); // 数据同步屏障，确保内存访问顺序

  canRxQueueHead = (uint16_t)nextHead; // 发布完整报文后更新生产者位置。
  uint32_t depth = (nextHead - canRxQueueTail) & CAN_RX_QUEUE_MASK;
  if(depth > canRxQueuePeak) canRxQueuePeak = depth;

  return 1U;
}

/*
消费者函数：由主循环调用
从队列中取出一帧；队列空时立即返回，不等待中断
*/
static uint8_t CAN_RxQueue_Pop(CanFrame *frame) {
  uint32_t tail = canRxQueueTail;

  // 检查队列是否为空
  if (tail == canRxQueueHead) {
    // 队列为空，无法出队
    return 0U; // 出队失败
  }

  // 确保读取到生产者已经写入的队列内容
  __DMB();

  // 把队列槽位复制到主循环自己的变量
  *frame = canRxQueue[tail];

  // 复制完成后，移动 tail，释放这个槽位给生产者
  __DMB(); // 数据同步屏障，确保内存访问顺序

  canRxQueueTail = (uint16_t)((tail + 1U) & CAN_RX_QUEUE_MASK); // 更新 tail 索引

  return 1U; // 出队成功
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */

  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_CAN1_Init();
  MX_USART6_UART_Init();
  MX_TIM2_Init();
  MX_SDIO_SD_Init();
  MX_FATFS_Init();
  MX_FSMC_Init();
  MX_I2C1_Init();
  /* USER CODE BEGIN 2 */

  // GT911 复位及 I2C 地址选择。必须在 LCD_Init 之前执行。
  init_gt911(GT911_I2C_SLAVE);

  lv_init();
  lv_port_disp_init();
  lv_port_indev_init();
  CanStats_Init();  // 在界面读取统计数据前完成模块初始化。
  ui_set_archive_callback(CAN_RequestArchive);
  ui_init();

  if(CanTimestamp_Start() == 0U)
  {
    Error_Handler();
  }

  /*
   * 尝试挂载 SD 卡并打开 CANLOG.CSV。
   * 初始化失败只关闭日志功能，不阻止 CAN 接收和串口显示。
   */
  if (CanLogger_Init() == 0U)
  {
    printf("SD LOG: disabled; CAN reception will continue\r\n");
  }
  LogPlayer_Init(SDPath);

  // CAN 尚未启动时完成初始整屏绘制，避免首次全屏渲染占用接收处理窗口。
  CAN_PublishRuntime();
  lv_timer_handler();
  lv_refr_now(NULL);
  lv_port_disp_reset_metrics();
  ui_reset_diagnostics();
  printf("CAN UI experiment: freeze_stats=%u; widget period=100ms\r\n",
         (unsigned)UI_DIAG_FREEZE_STATS);


  // CAN1过滤器配置，ID和Mask都设为0，表示接收所有符合格式的报文。
  CAN_FilterTypeDef canFilter = {0};

  canFilter.FilterBank = 0;                      // 使用过滤器组 0
  canFilter.FilterMode = CAN_FILTERMODE_IDMASK;  // 标识符屏蔽模式
  canFilter.FilterScale = CAN_FILTERSCALE_32BIT; // 使用 32 位过滤器

  canFilter.FilterIdHigh = 0;                        // ID 高 16 位
  canFilter.FilterIdLow = 0;                         // ID 低 16 位
  canFilter.FilterMaskIdHigh = 0;                    // Mask 高 16 位：不屏蔽
  canFilter.FilterMaskIdLow = 0;                     // Mask 低 16 位：不屏蔽
  canFilter.FilterFIFOAssignment = CAN_FILTER_FIFO0; // 将过滤器分配到 FIFO 0
  canFilter.FilterActivation = ENABLE;               // 启用过滤器
  canFilter.SlaveStartFilterBank = 14; // CAN1/CAN2 共用过滤器的分界值

  //收到报文时通知：FIFO0溢出时也通知
  uint32_t canNotifications = CAN_IT_RX_FIFO0_MSG_PENDING | CAN_IT_RX_FIFO0_OVERRUN |
      CAN_IT_ERROR_WARNING | CAN_IT_ERROR_PASSIVE | CAN_IT_BUSOFF |
      CAN_IT_LAST_ERROR_CODE | CAN_IT_ERROR;

  // 逐项确认初始化与启动结果，失败时保留界面及日志任务运行。
  if (HAL_CAN_GetState(&hcan1) != HAL_CAN_STATE_READY ||
      HAL_CAN_ConfigFilter(&hcan1, &canFilter) != HAL_OK ||
      HAL_CAN_Start(&hcan1) != HAL_OK ||
      HAL_CAN_ActivateNotification(&hcan1, canNotifications) != HAL_OK) {
    canStartupFailed = 1U;
    canHalErrorFlags |= HAL_CAN_GetError(&hcan1);
    // 部分启动成功后通知配置失败时，停止控制器，避免未处理的中断持续触发。
    if(HAL_CAN_GetState(&hcan1) == HAL_CAN_STATE_LISTENING) {
      (void)HAL_CAN_Stop(&hcan1);
    }
    printf("CAN1 start failed, HAL error=0x%08lX\r\n", (unsigned long)canHalErrorFlags);
  }
  else {
    // 错误状态中断独立于 RX0 中断，确保无接收报文时仍能记录总线错误。
    HAL_NVIC_SetPriority(CAN1_SCE_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(CAN1_SCE_IRQn);
    printf("CAN1 Sight Start\r\n");
  }

  CAN_PublishRuntime();

  // 运行阶段的耗时统计从 CAN 启动后开始，排除硬件及文件系统初始化。
  uint32_t lastServiceUs = (uint32_t)CanTimestamp_GetUs();
  uint32_t lastUiTick = HAL_GetTick() - CAN_UI_INTERVAL_MS;
  uint32_t lastHistoryTick = HAL_GetTick() - CAN_HISTORY_INTERVAL_MS;
  uint32_t observedFifoCount = 0U;
  uint32_t lastReceiveTick = HAL_GetTick();
  uint8_t performancePending = 0U;

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1) {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */

    // 优先消费接收队列；时间预算只在完整报文之间检查，不中断文件操作。
    uint32_t batchStarted = (uint32_t)CanTimestamp_GetUs();
    uint32_t serviceGap = batchStarted - lastServiceUs;
    lastServiceUs = batchStarted;
    if(serviceGap > canMaxServiceGapUs) canMaxServiceGapUs = serviceGap;
    CanFrame frame;

    // 队列为空或达到处理上限后结束本批次，剩余报文留待下一轮处理。
    for (uint32_t processed = 0U;
         processed < CAN_RX_BATCH_SIZE && CAN_RxQueue_Pop(&frame) != 0U;
         processed++) {
      // 只在主循环里写日志；CAN 中断只负责接收并把数据放入队列。
      CanLogger_AddFrame(&frame);
      // 统计处理独立于日志可写状态及界面筛选条件。
      CanStats_AddFrame(&frame);

#if CAN_RX_DEBUG_PRINT
      const char *frameType = (frame.ide == CAN_ID_STD) ? "STD" : "EXT";
      const char *frameRTR = (frame.rtr == CAN_RTR_DATA) ? "DATA" : "REMOTE";

      // 打印帧格式、类型、ID、DLC
      printf("CAN1 RX: %s %s ID:0x%lX DLC:%u Data:", frameType, frameRTR,
             (unsigned long)frame.id, (unsigned int)frame.dlc);

      //远程数据帧没有数据字节，只有数据帧打印
      if (frame.rtr == CAN_RTR_DATA) 
      {
        for (uint8_t i = 0U; i < frame.dlc; i++) 
        {
          printf(" %02X", (unsigned int)frame.data[i]);
        }
      }

      printf("\r\n");
#endif
      if((uint32_t)CanTimestamp_GetUs() - batchStarted >= CAN_RX_BATCH_BUDGET_US) break;
    }
    uint32_t elapsed = (uint32_t)CanTimestamp_GetUs() - batchStarted;
    if(elapsed > canMaxBatchUs) canMaxBatchUs = elapsed;

    // 定期把缓冲记录同步到卡；不在 CAN 中断里调用 FatFs。
    CanLogger_Process();

    // 界面按时间调度，持续来帧时仍保留输入及状态刷新的执行机会。
    if(HAL_GetTick() - lastUiTick >= CAN_UI_INTERVAL_MS) {
      CAN_PublishRuntime();
      LvPortDispMetrics displayBefore, displayAfter;
      LvPortIndevMetrics touchBefore, touchAfter;
      UiDiagnostics statsBefore, statsAfter;
      lv_port_disp_get_metrics(&displayBefore);
      ui_get_diagnostics(&statsBefore);
      lv_port_indev_get_metrics(&touchBefore);
      uint32_t started = (uint32_t)CanTimestamp_GetUs();
      lv_timer_handler();
      elapsed = (uint32_t)CanTimestamp_GetUs() - started;
      lv_port_disp_get_metrics(&displayAfter);
      lv_port_indev_get_metrics(&touchAfter);
      ui_get_diagnostics(&statsAfter);
      canUiCalls++;
      canUiTotalUs += elapsed;
      uint32_t bucket = elapsed < 1000U ? 0U : elapsed < 5000U ? 1U :
                        elapsed < 20000U ? 2U : elapsed < 50000U ? 3U :
                        elapsed < 100000U ? 4U : 5U;
      canUiHistogram[bucket]++;
      if(elapsed > canMaxUiUs) {
        canMaxUiUs = elapsed;
        canUiPeakLcdUs = displayAfter.flush_us - displayBefore.flush_us;
        canUiPeakTouchUs = touchAfter.read_us - touchBefore.read_us;
        uint32_t measured = canUiPeakLcdUs + canUiPeakTouchUs;
        canUiPeakOtherUs = elapsed > measured ? elapsed - measured : 0U;
        canUiPeakFlushes = displayAfter.flush_calls - displayBefore.flush_calls;
        canUiPeakPixels = displayAfter.pixels - displayBefore.pixels;
        canUiPeakRefreshUs = displayAfter.refresh_us - displayBefore.refresh_us;
        canUiPeakRenderUs = displayAfter.render_us - displayBefore.render_us;
        canUiPeakRenderCpuUs = displayAfter.render_cpu_us - displayBefore.render_cpu_us;
        canUiPeakRefreshOtherUs = canUiPeakRefreshUs > canUiPeakRenderUs ?
                                 canUiPeakRefreshUs - canUiPeakRenderUs : 0U;
        canUiPeakStatsUs = statsAfter.stats_us - statsBefore.stats_us;
        measured = canUiPeakRefreshUs + canUiPeakTouchUs + canUiPeakStatsUs;
        canUiPeakHandlerOtherUs = elapsed > measured ? elapsed - measured : 0U;
        canUiPeakInvalidRequests = displayAfter.invalid_requests - displayBefore.invalid_requests;
        canUiPeakInvalidPixels = displayAfter.invalid_pixels - displayBefore.invalid_pixels;
      }
      lastUiTick = HAL_GetTick();
    }

    // 接收积压较多时延后历史访问；单次同步文件操作仍需由耗时统计评估。
    uint32_t queued = ((uint32_t)canRxQueueHead - canRxQueueTail) & CAN_RX_QUEUE_MASK;
    if(queued <= CAN_HISTORY_QUEUE_LIMIT &&
       HAL_GetTick() - lastHistoryTick >= CAN_HISTORY_INTERVAL_MS) {
      uint32_t started = (uint32_t)CanTimestamp_GetUs();
      LogPlayer_Process();
      elapsed = (uint32_t)CanTimestamp_GetUs() - started;
      if(elapsed > canMaxHistoryUs) canMaxHistoryUs = elapsed;
      lastHistoryTick = HAL_GetTick();
    }

    // 仅在接收已静默且队列排空后打印一次；再次来帧后重新等待静默。
    uint32_t fifoCount = canRxFramesReadFromFifo;
    if(fifoCount != observedFifoCount) {
      observedFifoCount = fifoCount;
      lastReceiveTick = HAL_GetTick();
      performancePending = 1U;
    }
    else if(performancePending && canRxQueueHead == canRxQueueTail &&
            HAL_GetTick() - lastReceiveTick >= CAN_DIAGNOSTIC_IDLE_MS) {
      performancePending = 0U;
      CAN_PrintPerformance();
      // 静默期间的串口输出不计为下一轮接收处理停顿。
      lastServiceUs = (uint32_t)CanTimestamp_GetUs();
    }

    // 仅在没有接收积压时短暂让出 CPU；有积压立即进入下一批处理。
    if(canRxQueueHead == canRxQueueTail) HAL_Delay(1);
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 4;
  RCC_OscInitStruct.PLL.PLLN = 168;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 7;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
int __io_putchar(int ch) {
  uint8_t byte = (uint8_t)ch;
  HAL_UART_Transmit(&huart6, &byte, 1, HAL_MAX_DELAY);
  return ch;
}

// 从硬件 FIFO 取出报文，整理成 CanFrame，再放入软件队列
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
  if (hcan->Instance != CAN1) // 确认是 CAN1 的中断
  {
    return;
  }

  // 从硬件 FIFO 读取报文
  while (HAL_CAN_GetRxFifoFillLevel(hcan, CAN_RX_FIFO0) > 0U)
    {
        CAN_RxHeaderTypeDef header;
        uint8_t rxBytes[8] = {0};

        // 从 CAN 控制器的 FIFO0 读取一帧
        if (HAL_CAN_GetRxMessage(
                hcan,
                CAN_RX_FIFO0,
                &header,
                rxBytes) != HAL_OK)
        {
            // 读取失败时结束本次回调
            break;
        }

        canRxFramesReadFromFifo++; // 成功读取一帧，计数器加 1

        // 把 HAL 收到的数据整理到项目统一的报文结构
        CanFrame frame = {0};

        frame.timestamp_us = CanTimestamp_GetUs(); // 在接收时保存已展开的微秒时间。
        frame.ide = (uint8_t)header.IDE;
        frame.rtr = (uint8_t)header.RTR;

        // 保护 data[8]：不允许 DLC 超过经典 CAN 的 8 字节
        frame.dlc = (header.DLC <= 8U) ? (uint8_t)header.DLC : 8U;

        // 根据帧格式选择正确的 ID 字段
        if (header.IDE == CAN_ID_STD)
        {
            frame.id = header.StdId;
        }
        else
        {
            frame.id = header.ExtId;
        }

        // 复制报文数据；远程帧的 DLC 不表示这里有数据可复制
        if (header.RTR == CAN_RTR_DATA)
        {
            for (uint8_t i = 0U; i < frame.dlc; i++)
            {
                frame.data[i] = rxBytes[i];
            }
        }

        // 入队失败表示软件队列已满，函数内部会增加溢出计数
        (void)CAN_RxQueue_PushFromISR(&frame);
    }
}

void HAL_CAN_ErrorCallback(CAN_HandleTypeDef *hcan)
{
  //只处理 CAN1 的错误
    if (hcan->Instance != CAN1)
    {
        return;
    }

    // 获读取 HAL 当前累积的 CAN 错误位
    uint32_t error = HAL_CAN_GetError(hcan);

    // 保存错误位，方便之后通过调试器查看
    canHalErrorFlags |= error;

    // 检查是否发生了 FIFO0 溢出
    if ((error & HAL_CAN_ERROR_RX_FOV0) != 0U)
    {
        canRxFifo0OverrunEvents++; // FIFO0 溢出事件计数器加 1
    }

    //HAL错误位是累积保存的，先保存到canHalErrorFlags，再清除HAL内部错误状态
    (void)HAL_CAN_ResetError(hcan);
}
/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1) {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line
     number, ex: printf("Wrong parameters value: file %s on line %d\r\n", file,
     line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
