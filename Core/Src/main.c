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
#include <stdio.h>
#include <string.h>
#include "can_logger.h"
#include "can_timestamp.h"
#include "can_stats.h"
#include "log_player.h"
#include "lv_port_disp.h"
#include "lv_port_indev.h"
#include "gt911.h"
#include "lvgl/core/lv_obj_pos.h"
#include "ui.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
// 接收队列总槽位数，必须是 2 的整数次幂；保留一个空槽位，实际可暂存 2047 帧。
#define CAN_RX_QUEUE_SIZE 2048U
// 主循环每批最多处理的帧数，达到上限后给其他工作执行机会。
#define CAN_RX_BATCH_SIZE 256U
// 每批接收处理的时间预算，单位 us；只在帧与帧之间检查，不能中断正在执行的写卡操作。
#define CAN_RX_BATCH_BUDGET_US 2000U
// LVGL 服务的最小调度间隔，单位 ms；处理触摸和绘制，页面统计数据另按 500 ms 更新。
#define CAN_UI_INTERVAL_MS 5U
// 队列积压达到此帧数时，暂缓 LVGL 服务，优先处理接收。
#define CAN_UI_PAUSE_DEPTH 256U
// 积压降到此帧数后恢复 LVGL；与暂停阈值错开，避免反复切换。
#define CAN_UI_RESUME_DEPTH 64U
// 历史文件浏览的最小调度间隔，单位 ms；不用于限制 CAN 回放发送。
#define CAN_HISTORY_INTERVAL_MS 5U
// 仅在接收积压不超过此帧数时，允许推进历史文件访问。
#define CAN_HISTORY_QUEUE_LIMIT 128U
// 最后一次观察到新接收后，至少等待此时间再打印一次摘要，单位 ms；不是周期打印。
#define CAN_SUMMARY_IDLE_MS 1500U
// 诊断文字发送缓冲区大小，单位字节；与保存 CanFrame 的接收队列不同。
#define CAN_SERIAL_BUFFER_SIZE 2048U
// 编译时检查：掩码循环要求队列大小至少为 2，且是 2 的整数次幂。
#if (CAN_RX_QUEUE_SIZE < 2U) || ((CAN_RX_QUEUE_SIZE & (CAN_RX_QUEUE_SIZE - 1U)) != 0U)
#error "CAN_RX_QUEUE_SIZE must be a power of two and at least two"
#endif
// 队列索引使用 uint16_t，最多能编号 65536 个槽位；实际容量还受 CCM 大小限制。
#if CAN_RX_QUEUE_SIZE > 65536U
#error "CAN RX queue indices require a wider type"
#endif

// 与此掩码按位与，可在越过数组末尾时将索引绕回开头。
#define CAN_RX_QUEUE_MASK (CAN_RX_QUEUE_SIZE - 1U)
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

// 接收帧数组放在 CCMRAM：2048 × 24 字节 = 48 KiB；仅 CPU 访问，DMA 不访问此数组。
static CanFrame canRxQueue[CAN_RX_QUEUE_SIZE] __attribute__((section(".ccmram.rx_queue"), aligned(8)));
// 1 表示因接收积压暂缓界面服务，仅由主循环读写。
static uint8_t canUiDeferred;
// printf 生成的诊断文字先存于此处，主循环再轮询发送；中断中不调用 printf。
static uint8_t canSerialBuffer[CAN_SERIAL_BUFFER_SIZE];
// head 是下一个写入位置，tail 是下一个发送位置；两者都只在主循环上下文使用。
static uint16_t canSerialHead, canSerialTail;
// 串口缓冲区满时丢弃的字符数，不是丢失的 CAN 帧数。
static uint32_t canSerialDropped;

// head 由接收中断写入，tail 由主循环写入；两边读取对方索引判断队列状态。
// volatile 保证读取实际内存中的索引值，帧内容与索引的访问顺序另由 __DMB() 保证。
static volatile uint16_t canRxQueueHead = 0U;
static volatile uint16_t canRxQueueTail = 0U;

// 已从硬件 FIFO 取出，但因软件队列满而被丢弃的帧数。
static volatile uint32_t canRxQueueOverflow = 0U;

// 成功从 CAN 硬件 FIFO0 取出的帧数，不表示已入队或已保存到 SD 卡。
static volatile uint32_t canRxFramesReadFromFifo = 0U;

// HAL 报告的硬件 FIFO0 溢出事件数，不能据此确定实际丢失了多少帧。
static volatile uint32_t canRxFifo0OverrunEvents = 0U;

// 累积保留出现过的 CAN 错误位；不同错误通过按位或合并，不是当前硬件状态。
static volatile uint32_t canHalErrorFlags = 0U;
// 1 表示 main 中的 CAN 状态检查、过滤器配置、启动或中断通知配置失败。
static uint8_t canStartupFailed = 0U;
// 接收队列的历史最大占用帧数；仅由接收中断更新，不代表当前积压。
static volatile uint32_t canRxQueuePeak;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
static uint32_t CAN_RxQueue_Depth(void);
static uint8_t CAN_RxQueue_PushFromISR(const CanFrame *frame);
static uint8_t CAN_RxQueue_Pop(CanFrame *frame);
static void CAN_ServiceSerial(void);
static void CAN_PublishRuntime(void);
static void CAN_PrintSummary(void);
static void CAN_RequestArchive(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

// 主循环调用：返回当前尚未取出的帧数，不改变读写位置。
// 中断可能随时继续入队，因此结果只代表读取时附近的队列状态。
static uint32_t CAN_RxQueue_Depth(void)
{
  return ((uint32_t)canRxQueueHead - canRxQueueTail) & CAN_RX_QUEUE_MASK;
}

// CAN 接收中断调用：将 frame 指向的一帧复制入队，供主循环稍后处理。
// 返回 1 表示成功；返回 0 表示队列已满，本帧被丢弃并计入软件丢帧数。
static uint8_t CAN_RxQueue_PushFromISR(const CanFrame *frame)
{
  uint32_t head = canRxQueueHead;

  // 先计算写完本槽位后的位置；到达数组末尾时通过掩码绕回 0。
  uint32_t nextHead = (head + 1U) & CAN_RX_QUEUE_MASK;

  // 若再前进一格就追上读取位置，说明只剩保留空槽，队列已满。
  if (nextHead == canRxQueueTail)
  {
    canRxQueueOverflow++;
    return 0U;
  }

  // 先复制完整帧，随后才能通知主循环这里已有新数据。
  canRxQueue[head] = *frame;

  // 数据内存屏障：保证帧内容先写完，再更新 head，防止主循环读取到半帧数据。
  __DMB();

  canRxQueueHead = (uint16_t)nextHead;
  uint32_t depth = (nextHead - canRxQueueTail) & CAN_RX_QUEUE_MASK;
  if(depth > canRxQueuePeak) canRxQueuePeak = depth;

  return 1U;
}

// 主循环调用：取出队首帧并复制到 frame 指向的变量，随后释放该槽位。
// 返回 1 表示成功；返回 0 表示队列为空，不等待新帧。
static uint8_t CAN_RxQueue_Pop(CanFrame *frame)
{
  uint32_t tail = canRxQueueTail;

  // 读写位置相同表示队列为空。
  if (tail == canRxQueueHead)
  {
    return 0U;
  }

  // 确认 head 已前进后，再读取该槽位内已经写好的帧内容。
  __DMB();

  // 复制到主循环自己的变量，后续处理不再依赖这个队列槽位。
  *frame = canRxQueue[tail];

  // 保证复制完成后才更新 tail，避免中断提前复用这个槽位。
  __DMB();

  canRxQueueTail = (uint16_t)((tail + 1U) & CAN_RX_QUEUE_MASK);

  return 1U;
}

// 主循环调用：串口可写时发送一个诊断字符，没有待发字符或硬件未就绪时立即返回。
static void CAN_ServiceSerial(void)
{
  if(canSerialHead == canSerialTail) return;
  // TXE 表示发送数据寄存器可写；同时确认串口和发送功能已启用，不等待整字节发送完成。
  if((READ_REG(USART6->SR) & USART_SR_TXE) != 0U &&
     (READ_REG(USART6->CR1) & (USART_CR1_UE | USART_CR1_TE)) == (USART_CR1_UE | USART_CR1_TE))
  {
    WRITE_REG(USART6->DR, canSerialBuffer[canSerialTail]);
    canSerialTail = (uint16_t)((canSerialTail + 1U) & (CAN_SERIAL_BUFFER_SIZE - 1U));
  }
}

// 主循环调用：收集 CAN、日志和队列状态，复制到统计模块，供界面读取。
// 当前运行状态与累计错误记录分开保存，历史错误不直接决定当前状态。
static void CAN_PublishRuntime(void)
{
  CanStatsRuntime runtime = {0};
  HAL_CAN_StateTypeDef state = HAL_CAN_GetState(&hcan1);
  uint32_t esr = READ_REG(hcan1.Instance->ESR);

  if(canStartupFailed != 0U || state == HAL_CAN_STATE_ERROR ||
     (esr & (CAN_ESR_EWGF | CAN_ESR_EPVF | CAN_ESR_BOFF)) != 0U)
  {
    runtime.can_state = CAN_STATS_ERROR;
  }
  else if(state == HAL_CAN_STATE_LISTENING)
  {
    runtime.can_state = CAN_STATS_RUNNING;
  }
  else
  {
    runtime.can_state = CAN_STATS_STOPPED;
  }

  // 中断可能在两次赋值之间更新计数，因此这些值不是严格同一时刻的整体快照。
  runtime.queue_lost = canRxQueueOverflow;
  runtime.fifo_overruns = canRxFifo0OverrunEvents;
  runtime.error_flags = canHalErrorFlags | HAL_CAN_GetError(&hcan1);
  runtime.sd_state = CanLogger_GetState();
  runtime.sd_error = CanLogger_GetLastError();
  runtime.fifo_frames = canRxFramesReadFromFifo;
  runtime.queue_depth = ((uint32_t)canRxQueueHead - canRxQueueTail) & CAN_RX_QUEUE_MASK;
  runtime.queue_peak = canRxQueuePeak;
  runtime.queue_capacity = CAN_RX_QUEUE_SIZE - 1U;
  CanLogger_GetDiagnostics(&runtime.logger);
  runtime.archive_state = CanLogger_GetArchiveState();
  snprintf(runtime.archive_name, sizeof(runtime.archive_name), "%s",
           CanLogger_GetLastArchiveName());
  CanStats_SetRuntime(&runtime);
}

// 主循环在接收静默、队列排空且无回放时调用，打印接收、记录和错误摘要。
// printf 只将文字写入串口缓冲区；各层计数含义不同，不能直接相加。
static void CAN_PrintSummary(void)
{
  CanLoggerDiagnostics log;
  CanLogger_GetDiagnostics(&log);
  printf("CAN: fifo=%lu processed=%lu lost=%lu overruns=%lu peak=%lu/%lu\r\n",
         (unsigned long)canRxFramesReadFromFifo, (unsigned long)CanStats_GetTotalCount(),
         (unsigned long)canRxQueueOverflow, (unsigned long)canRxFifo0OverrunEvents,
         (unsigned long)canRxQueuePeak, (unsigned long)(CAN_RX_QUEUE_SIZE - 1U));
  printf("SD: accepted=%lu written=%lu synced=%lu buffered=%lu rejected=%lu\r\n",
         (unsigned long)log.accepted_frames, (unsigned long)log.written_frames,
         (unsigned long)log.synced_frames, (unsigned long)log.buffered_frames,
         (unsigned long)log.rejected_frames);
  printf("ERROR: can=0x%08lX sd=%lu touch=%lu serial_dropped=%lu\r\n",
         (unsigned long)canHalErrorFlags, (unsigned long)CanLogger_GetLastError(),
         (unsigned long)lv_port_indev_get_io_errors(), (unsigned long)canSerialDropped);
}

// LVGL 归档按钮在主循环上下文中调用；只提交请求，文件操作交给日志模块处理。
static void CAN_RequestArchive(void)
{
  (void)CanLogger_RequestArchive();
}

/* USER CODE END 0 */

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

  // CCM 队列使用 NOLOAD 段，启动汇编不会清零；启用 CAN 接收前先开时钟并清空数组。
  __HAL_RCC_CCMDATARAMEN_CLK_ENABLE();
  memset(canRxQueue, 0, sizeof(canRxQueue));

  // 复位 GT911 并选择 I2C 地址，保留当前板卡要求的先触摸复位、后 LCD 初始化顺序。
  init_gt911(GT911_I2C_SLAVE);

  // 初始化图形库、显示与触摸适配，再建立统计表、界面回调和控件。
  lv_init();
  lv_port_disp_init();
  lv_port_indev_init();
  CanStats_Init();
  ui_set_archive_callback(CAN_RequestArchive);
  ui_init();

  // 建立微秒时间基准，供接收时间戳和回放调度使用；失败时不继续启动采集。
  if(CanTimestamp_Start() == 0U)
  {
    Error_Handler();
  }

  // 挂载 SD 卡并准备 CANLOG.CSV；失败后仍继续启动 CAN 接收、界面和串口诊断。
  if (CanLogger_Init() == 0U)
  {
    printf("SD LOG: disabled; CAN reception will continue\r\n");
  }
  // 历史模块这里只保存根路径和初始状态，进入 SD 页面后才处理浏览请求。
  LogPlayer_Init(SDPath);

  // CAN 尚未启动时完成初始整屏绘制，避免首次全屏渲染占用接收处理窗口。
  CAN_PublishRuntime();
  lv_timer_handler();
  lv_refr_now(NULL);
  printf("CAN UI: period=500ms\r\n");


  // 配置全接收过滤器：掩码全为 0，不按 ID 排除标准帧、扩展帧或远程帧。
  CAN_FilterTypeDef canFilter = {0};

  canFilter.FilterBank = 0;                      // 使用过滤器组 0。
  canFilter.FilterMode = CAN_FILTERMODE_IDMASK;  // 按 ID 与掩码进行匹配。
  canFilter.FilterScale = CAN_FILTERSCALE_32BIT; // 使用单个 32 位过滤器。

  canFilter.FilterIdHigh = 0;                        // ID 高 16 位。
  canFilter.FilterIdLow = 0;                         // ID 低 16 位。
  canFilter.FilterMaskIdHigh = 0;                    // 掩码高 16 位为 0，不比较对应位。
  canFilter.FilterMaskIdLow = 0;                     // 掩码低 16 位为 0，不比较对应位。
  canFilter.FilterFIFOAssignment = CAN_FILTER_FIFO0; // 匹配帧进入硬件 FIFO0。
  canFilter.FilterActivation = ENABLE;               // 启用本组过滤器。
  canFilter.SlaveStartFilterBank = 14; // CAN1/CAN2 共用过滤器组的分界编号。

  // 开启 FIFO0 新帧、溢出及 CAN 错误通知；SCE 中断负责错误状态处理。
  uint32_t canNotifications = CAN_IT_RX_FIFO0_MSG_PENDING | CAN_IT_RX_FIFO0_OVERRUN |
      CAN_IT_ERROR_WARNING | CAN_IT_ERROR_PASSIVE | CAN_IT_BUSOFF |
      CAN_IT_LAST_ERROR_CODE | CAN_IT_ERROR;

  // 依次检查 CAN 状态、配置过滤器、启动和开启通知；这里失败后仍进入主循环。
  if (HAL_CAN_GetState(&hcan1) != HAL_CAN_STATE_READY ||
      HAL_CAN_ConfigFilter(&hcan1, &canFilter) != HAL_OK ||
      HAL_CAN_Start(&hcan1) != HAL_OK ||
      HAL_CAN_ActivateNotification(&hcan1, canNotifications) != HAL_OK)
  {
    canStartupFailed = 1U;
    canHalErrorFlags |= HAL_CAN_GetError(&hcan1);
    // 部分启动成功后通知配置失败时，停止控制器，避免未处理的中断持续触发。
    if(HAL_CAN_GetState(&hcan1) == HAL_CAN_STATE_LISTENING)
    {
      (void)HAL_CAN_Stop(&hcan1);
    }
    printf("CAN1 start failed, HAL error=0x%08lX\r\n", (unsigned long)canHalErrorFlags);
  }
  else
  {
    // 错误状态中断独立于 RX0 中断，确保无接收报文时仍能记录总线错误。
    HAL_NVIC_SetPriority(CAN1_SCE_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(CAN1_SCE_IRQn);
    printf("CAN1 Sight Start\r\n");
  }

  CAN_PublishRuntime();

  // 保存各项调度的毫秒时间；减去间隔，让界面和历史服务在首轮即可满足时间条件。
  uint32_t lastUiTick = HAL_GetTick() - CAN_UI_INTERVAL_MS;
  uint32_t lastHistoryTick = HAL_GetTick() - CAN_HISTORY_INTERVAL_MS;
  uint32_t observedFifoCount = 0U;  // 上次观察到的硬件取帧累计数，用于判断是否继续来帧。
  uint32_t lastReceiveTick = HAL_GetTick();  // 最近观察到取帧计数变化的时刻。
  uint8_t summaryPending = 0U;  // 收到新帧后置 1，成功生成本轮摘要后清零。

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */

    // 先尝试发送一个诊断字符；没有待发文字时立即返回。
    CAN_ServiceSerial();

    // 1. 推进 CAN 回放：检查发送结果和时间戳，到时提交下一帧，不等待历史浏览调度。
    LogPlayer_ReplayProcess();

    // 2. 分批处理接收队列：同时限制帧数和耗时，避免持续来帧占满整轮主循环。
    uint32_t batchStarted = (uint32_t)CanTimestamp_GetUs();
    CanFrame frame;

    // 队列为空或达到帧数上限时退出；每处理完一帧，再检查时间预算。
    for (uint32_t processed = 0U;
         processed < CAN_RX_BATCH_SIZE && CAN_RxQueue_Pop(&frame) != 0U;
         processed++)
    {
      // 日志通常先写 RAM 缓冲；空间不足或需要归档时，此调用也可能同步写卡。
      CanLogger_AddFrame(&frame);
      // 无论记录是否成功，都更新接收统计；界面筛选也不影响这里。
      CanStats_AddFrame(&frame);

      if((uint32_t)CanTimestamp_GetUs() - batchStarted >= CAN_RX_BATCH_BUDGET_US) break;
    }

    // 3. 推进日志同步和归档，再次服务回放，减少前面接收处理造成的发送延迟。
    CanLogger_Process();
    LogPlayer_ReplayProcess();

    // 4. 队列积压高时暂缓 LVGL，降到恢复阈值后再处理触摸、定时器和绘制。
    uint32_t uiQueueDepth = CAN_RxQueue_Depth();
    if(uiQueueDepth >= CAN_UI_PAUSE_DEPTH) canUiDeferred = 1U;
    else if(uiQueueDepth <= CAN_UI_RESUME_DEPTH) canUiDeferred = 0U;
    if(!canUiDeferred && HAL_GetTick() - lastUiTick >= CAN_UI_INTERVAL_MS)
    {
      CAN_PublishRuntime();
      lv_timer_handler();
      lastUiTick = HAL_GetTick();
    }

    // 5. 接收积压较少且达到调度间隔时，推进历史目录、索引、预览或删除请求。
    uint32_t queued = ((uint32_t)canRxQueueHead - canRxQueueTail) & CAN_RX_QUEUE_MASK;
    if(queued <= CAN_HISTORY_QUEUE_LIMIT &&
       HAL_GetTick() - lastHistoryTick >= CAN_HISTORY_INTERVAL_MS)
    {
      LogPlayer_Process();
      lastHistoryTick = HAL_GetTick();
    }

    // 6. 取帧计数有变化就重新计时；静默达到 1.5 秒且接收、串口队列均为空、无回放时打印一次。
    uint32_t fifoCount = canRxFramesReadFromFifo;
    if(fifoCount != observedFifoCount)
    {
      observedFifoCount = fifoCount;
      lastReceiveTick = HAL_GetTick();
      summaryPending = 1U;
    }
    else if(summaryPending && !LogPlayer_ReplayBusy() && canRxQueueHead == canRxQueueTail &&
            canSerialHead == canSerialTail &&
            HAL_GetTick() - lastReceiveTick >= CAN_SUMMARY_IDLE_MS)
    {
      summaryPending = 0U;
      CAN_PrintSummary();
      // 摘要文字先入串口缓冲，后续每轮继续发送，同时照常处理新收到的 CAN 帧。
    }

    // 7. 再尝试发送一个诊断字符；接收队列为空、无回放且无待发文字时才延时 1 ms。
    CAN_ServiceSerial();
    if(canRxQueueHead == canRxQueueTail && !LogPlayer_ReplayBusy() && canSerialHead == canSerialTail)
    {
      HAL_Delay(1);
    }
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
// printf 经底层输出接口在主循环上下文中调用：只将一个字符入队，不等待串口。
// 缓冲区满时丢弃该字符并计数，仍返回 ch；丢字符情况由摘要单独报告。
int __io_putchar(int ch)
{
  uint16_t next = (uint16_t)((canSerialHead + 1U) & (CAN_SERIAL_BUFFER_SIZE - 1U));
  if(next == canSerialTail)
  {
    canSerialDropped++;
    return ch;
  }
  canSerialBuffer[canSerialHead] = (uint8_t)ch;
  canSerialHead = next;
  return ch;
}

// HAL 在 CAN 接收中断上下文调用：取出 FIFO0 中的帧，添加时间戳并复制进软件队列。
// 此处不访问 FatFs、不更新界面、不打印文字；日志和统计交给主循环。
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
  // 只处理本项目使用的 CAN1。
  if (hcan->Instance != CAN1)
  {
    return;
  }

  // 一次回调尽量取空硬件 FIFO0，避免只取一帧后其余帧继续占用硬件槽位。
  while (HAL_CAN_GetRxFifoFillLevel(hcan, CAN_RX_FIFO0) > 0U)
  {
    CAN_RxHeaderTypeDef header;
    uint8_t rxBytes[8] = {0};

    // 读取一帧的头信息和数据；成功后该硬件 FIFO 槽位被释放。
    if (HAL_CAN_GetRxMessage(
        hcan,
        CAN_RX_FIFO0,
        &header,
        rxBytes) != HAL_OK)
    {
      // 读取失败就退出本次回调，避免在中断中反复重试。
      break;
    }

    canRxFramesReadFromFifo++;

    // 将 HAL 的帧头和数据转换为项目共用的 CanFrame；零初始化清除未使用的数据字节。
    CanFrame frame = {0};

    // 记录 CPU 从 FIFO 取帧后的微秒时间，不是 CAN 控制器锁存的线上到达时刻。
    frame.timestamp_us = CanTimestamp_GetUs();
    frame.ide = (uint8_t)header.IDE;
    frame.rtr = (uint8_t)header.RTR;

    // 将 DLC 限制在 0～8，保护经典 CAN 的 data[8] 数组。
    frame.dlc = (header.DLC <= 8U) ? (uint8_t)header.DLC : 8U;

    // HAL 用不同字段保存标准帧和扩展帧 ID，这里统一写入 frame.id。
    if (header.IDE == CAN_ID_STD)
    {
      frame.id = header.StdId;
    }
    else
    {
      frame.id = header.ExtId;
    }

    // 仅数据帧复制有效字节；远程帧保留 DLC，但不携带数据字节。
    if (header.RTR == CAN_RTR_DATA)
    {
      for (uint8_t i = 0U; i < frame.dlc; i++)
      {
        frame.data[i] = rxBytes[i];
      }
    }

    // 入队时复制整帧，不保存局部变量的地址；队列满时由入队函数丢弃并计数。
    (void)CAN_RxQueue_PushFromISR(&frame);
  }
}

// HAL 在 CAN 中断上下文调用：累计错误位和 FIFO0 溢出事件，供主循环读取。
// 此回调只记录错误，不执行 Bus-off 恢复或重启控制器。
void HAL_CAN_ErrorCallback(CAN_HandleTypeDef *hcan)
{
  // 只处理本项目使用的 CAN1。
  if (hcan->Instance != CAN1)
  {
    return;
  }

  // 先读取 HAL 已记录的错误位，再合并到本项目的历史错误记录。
  uint32_t error = HAL_CAN_GetError(hcan);

  canHalErrorFlags |= error;

  // FIFO0 溢出按事件计数；一次事件可能对应不止一帧丢失。
  if ((error & HAL_CAN_ERROR_RX_FOV0) != 0U)
  {
    canRxFifo0OverrunEvents++;
  }

  // 清除 HAL 软件错误码，不清除上述历史记录，也不等于恢复 Bus-off。
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
  while (1)
  {
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
