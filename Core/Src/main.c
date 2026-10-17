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
#include "sdio.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "stdio.h"
#include "can_logger.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
// 队列大小
#define CAN_RX_QUEUE_SIZE 64U

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
static volatile uint8_t canRxQueueHead = 0U;
static volatile uint8_t canRxQueueTail = 0U;

// 软件队列满时，无法入队的报文数量
static volatile uint32_t canRxQueueOverflow = 0U;

//成功从CAN硬件FIFO0取出的帧数
static volatile uint32_t canRxFramesReadFromFifo = 0U;

//CAN硬件FIFO0溢出的事件数
static volatile uint32_t canRxFifo0OverrunEvents = 0U;

//HAL 报告过的 CAN 错误位集合，便于调试时查看
static volatile uint32_t canHalErrorFlags = 0U;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */


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

  canRxQueueHead = nextHead; // 更新 head 索引，表示新帧已入队

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

  canRxQueueTail = (tail + 1U) & CAN_RX_QUEUE_MASK; // 更新 tail 索引

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
  /* USER CODE BEGIN 2 */

  if(HAL_TIM_Base_Start(&htim2) != HAL_OK)
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


  /*CAN1过滤器配置，ID和Mask都设为0，表示接收所有符合格式的报文。*/
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
  uint32_t canNotifications = CAN_IT_RX_FIFO0_MSG_PENDING | CAN_IT_RX_FIFO0_OVERRUN;

  /* 将过滤器配置写入 CAN1 */
  if (HAL_CAN_ConfigFilter(&hcan1, &canFilter) != HAL_OK) {
    /* 过滤器配置失败，进入错误处理 */
    Error_Handler();
  }

  /* 启动 CAN1 */
  if (HAL_CAN_Start(&hcan1) != HAL_OK) {
    /* CAN 启动失败，进入错误处理 */
    Error_Handler();
  }

  if (HAL_CAN_ActivateNotification(&hcan1, canNotifications) !=
      HAL_OK) {
    /* 激活中断失败，进入错误处理 */
    Error_Handler();
  }

  printf("CAN1 Sight Start\r\n");

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1) {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */

    CanFrame frame;

    while (CAN_RxQueue_Pop(&frame) != 0U) {
      /* 只在主循环里写日志；CAN 中断只负责接收并把数据放入队列。 */
      CanLogger_AddFrame(&frame);

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
    }

    /* 定期把缓冲记录同步到卡；不在 CAN 中断里调用 FatFs。 */
    CanLogger_Process();
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

        /* 从 CAN 控制器的 FIFO0 读取一帧 */
        if (HAL_CAN_GetRxMessage(
                hcan,
                CAN_RX_FIFO0,
                &header,
                rxBytes) != HAL_OK)
        {
            /* 读取失败时结束本次回调 */
            break;
        }

        canRxFramesReadFromFifo++; // 成功读取一帧，计数器加 1

        /* 把 HAL 收到的数据整理到项目统一的报文结构 */
        CanFrame frame = {0};

        frame.timestamp_us = __HAL_TIM_GET_COUNTER(&htim2); // 记录接收时间戳
        frame.ide = (uint8_t)header.IDE;
        frame.rtr = (uint8_t)header.RTR;

        /* 保护 data[8]：不允许 DLC 超过经典 CAN 的 8 字节 */
        frame.dlc = (header.DLC <= 8U) ? (uint8_t)header.DLC : 8U;

        /* 根据帧格式选择正确的 ID 字段 */
        if (header.IDE == CAN_ID_STD)
        {
            frame.id = header.StdId;
        }
        else
        {
            frame.id = header.ExtId;
        }

        /* 复制报文数据；远程帧的 DLC 不表示这里有数据可复制 */
        if (header.RTR == CAN_RTR_DATA)
        {
            for (uint8_t i = 0U; i < frame.dlc; i++)
            {
                frame.data[i] = rxBytes[i];
            }
        }

        /* 入队失败表示软件队列已满，函数内部会增加溢出计数 */
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
