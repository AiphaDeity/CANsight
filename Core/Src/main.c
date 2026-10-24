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
#include <string.h>
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
// CCM 队列保留一个槽位区分空与满，可用 2047 帧，DMA 不访问此队列。
#define CAN_RX_QUEUE_SIZE 2048U
// 每批同时限制帧数与处理时间，避免持续来帧独占主循环。
#define CAN_RX_BATCH_SIZE 256U
#define CAN_RX_BATCH_BUDGET_US 2000U
#define CAN_UI_INTERVAL_MS 5U
#define CAN_UI_PAUSE_DEPTH 256U
#define CAN_UI_RESUME_DEPTH 64U
#define CAN_HISTORY_INTERVAL_MS 5U
#define CAN_HISTORY_QUEUE_LIMIT 128U
// 接收静默且队列排空后生成一次摘要，文字通过非阻塞缓冲发送。
#define CAN_SUMMARY_IDLE_MS 1500U
#define CAN_SERIAL_BUFFER_SIZE 2048U
#if (CAN_RX_QUEUE_SIZE < 2U) || ((CAN_RX_QUEUE_SIZE & (CAN_RX_QUEUE_SIZE - 1U)) != 0U)
#error "CAN_RX_QUEUE_SIZE must be a power of two and at least two"
#endif
#if CAN_RX_QUEUE_SIZE > 65536U
#error "CAN RX queue indices require a wider type"
#endif

#define CAN_RX_QUEUE_MASK (CAN_RX_QUEUE_SIZE - 1U) // 掩码，用于循环队列索引
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

// 保存 CAN 接收报文的环形队列
static CanFrame canRxQueue[CAN_RX_QUEUE_SIZE] __attribute__((section(".ccmram.rx_queue"), aligned(8)));
static uint8_t canUiDeferred;
// printf 仅由主循环使用；发送也在主循环轮询完成，不在 ISR 中调用 printf。
static uint8_t canSerialBuffer[CAN_SERIAL_BUFFER_SIZE];
static uint16_t canSerialHead, canSerialTail;
static uint32_t canSerialDropped;

static void CAN_ServiceSerial(void)
{
  if(canSerialHead == canSerialTail) return;
  // TXE 表示数据寄存器可写；不等待发送完成，不调用阻塞式 HAL_UART_Transmit。
  if((READ_REG(USART6->SR) & USART_SR_TXE) != 0U &&
     (READ_REG(USART6->CR1) & (USART_CR1_UE | USART_CR1_TE)) == (USART_CR1_UE | USART_CR1_TE)) {
    WRITE_REG(USART6->DR, canSerialBuffer[canSerialTail]);
    canSerialTail = (uint16_t)((canSerialTail + 1U) & (CAN_SERIAL_BUFFER_SIZE - 1U));
  }
}

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

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

// 只读队列快照供主循环诊断使用，不改变生产者和消费者索引。
static uint32_t CAN_RxQueue_Depth(void)
{
  return ((uint32_t)canRxQueueHead - canRxQueueTail) & CAN_RX_QUEUE_MASK;
}


//将控制器状态、日志状态及接收异常计数发布至界面数据层。错误历史与当前状态分别维护，历史错误不阻止正常状态恢复。
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
  CanLogger_GetDiagnostics(&runtime.logger);
  runtime.archive_state = CanLogger_GetArchiveState();
  snprintf(runtime.archive_name, sizeof(runtime.archive_name), "%s",
           CanLogger_GetLastArchiveName());
  CanStats_SetRuntime(&runtime);
}

// 仅由主循环在接收静默后调用，汇总接收、记录和错误计数。
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


// 界面回调仅提交请求，不在 LVGL 事件中直接访问文件系统。
static void CAN_RequestArchive(void)
{
  (void)CanLogger_RequestArchive();
}


//生产者函数：由CAN接收中断调用，把一帧复制进队列；队列满时立即返回，不等待主循环
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

//消费者函数：由主循环调用。从队列中取出一帧；队列空时立即返回，不等待中断
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

  // 启动汇编不初始化CCM；在启用 CAN 接收前显式清空NOLOAD队列。
  __HAL_RCC_CCMDATARAMEN_CLK_ENABLE();
  memset(canRxQueue, 0, sizeof(canRxQueue));

  // GT911 复位及I2C地址选择。必须在LCD_Init之前执行。
  init_gt911(GT911_I2C_SLAVE);

  lv_init();
  lv_port_disp_init();
  lv_port_indev_init();
  CanStats_Init();
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
  printf("CAN UI: period=500ms\r\n");


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

  // CAN 启动后初始化界面、历史浏览及摘要调度。
  uint32_t lastUiTick = HAL_GetTick() - CAN_UI_INTERVAL_MS;
  uint32_t lastHistoryTick = HAL_GetTick() - CAN_HISTORY_INTERVAL_MS;
  uint32_t observedFifoCount = 0U;
  uint32_t lastReceiveTick = HAL_GetTick();
  uint8_t summaryPending = 0U;

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1) {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */

    CAN_ServiceSerial();

    // 回放使用微秒时间戳，每轮服务发送邮箱；不受历史浏览的 5 ms 调度限制。
    LogPlayer_ReplayProcess();

    // 优先消费接收队列；时间预算只在完整报文之间检查，不中断文件操作。
    uint32_t batchStarted = (uint32_t)CanTimestamp_GetUs();
    CanFrame frame;

    // 队列为空或达到处理上限后结束本批次，剩余报文留待下一轮处理。
    for (uint32_t processed = 0U;
         processed < CAN_RX_BATCH_SIZE && CAN_RxQueue_Pop(&frame) != 0U;
         processed++) {
      // 只在主循环里写日志；CAN 中断只负责接收并把数据放入队列。
      CanLogger_AddFrame(&frame);
      // 统计处理独立于日志可写状态及界面筛选条件。
      CanStats_AddFrame(&frame);

      if((uint32_t)CanTimestamp_GetUs() - batchStarted >= CAN_RX_BATCH_BUDGET_US) break;
    }

    // 定期把缓冲记录同步到卡；不在 CAN 中断里调用 FatFs。
    CanLogger_Process();
    LogPlayer_ReplayProcess();

    // 界面按时间调度，持续来帧时仍保留输入及状态刷新的执行机会。
    uint32_t uiQueueDepth = CAN_RxQueue_Depth();
    if(uiQueueDepth >= CAN_UI_PAUSE_DEPTH) canUiDeferred = 1U;
    else if(uiQueueDepth <= CAN_UI_RESUME_DEPTH) canUiDeferred = 0U;
    if(!canUiDeferred && HAL_GetTick() - lastUiTick >= CAN_UI_INTERVAL_MS) {
      CAN_PublishRuntime();
      lv_timer_handler();
      lastUiTick = HAL_GetTick();
    }

    // 接收积压较多时延后历史访问，优先处理收到的报文。
    uint32_t queued = ((uint32_t)canRxQueueHead - canRxQueueTail) & CAN_RX_QUEUE_MASK;
    if(queued <= CAN_HISTORY_QUEUE_LIMIT &&
       HAL_GetTick() - lastHistoryTick >= CAN_HISTORY_INTERVAL_MS) {
      LogPlayer_Process();
      lastHistoryTick = HAL_GetTick();
    }

    // 仅在接收已静默且队列排空后打印一次；再次来帧后重新等待静默。
    uint32_t fifoCount = canRxFramesReadFromFifo;
    if(fifoCount != observedFifoCount) {
      observedFifoCount = fifoCount;
      lastReceiveTick = HAL_GetTick();
      summaryPending = 1U;
    }
    else if(summaryPending && !LogPlayer_ReplayBusy() && canRxQueueHead == canRxQueueTail &&
            canSerialHead == canSerialTail &&
            HAL_GetTick() - lastReceiveTick >= CAN_SUMMARY_IDLE_MS) {
      summaryPending = 0U;
      CAN_PrintSummary();
      // 摘要仅格式化入队；串口逐字节发送期间仍正常处理接收。
    }

    // 仅在没有接收积压时短暂让出 CPU；有积压立即进入下一批处理。
    CAN_ServiceSerial();
    if(canRxQueueHead == canRxQueueTail && !LogPlayer_ReplayBusy() && canSerialHead == canSerialTail) {
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
int __io_putchar(int ch) {
  uint16_t next = (uint16_t)((canSerialHead + 1U) & (CAN_SERIAL_BUFFER_SIZE - 1U));
  if(next == canSerialTail) {
    canSerialDropped++; // 诊断队列满只丢字符，不阻塞 CAN 接收。
    return ch;
  }
  canSerialBuffer[canSerialHead] = (uint8_t)ch;
  canSerialHead = next;
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
