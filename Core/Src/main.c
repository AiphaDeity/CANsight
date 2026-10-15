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
#include "gpio.h"
#include "stm32_hal_legacy.h"
#include "usart.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "stdio.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* CAN发送报文头、数据和发送邮箱*/
static CAN_TxHeaderTypeDef txHeader;
static uint8_t txData[8];
static uint32_t txMailbox;

static CAN_RxHeaderTypeDef rxHeader;
static uint8_t rxData[8];
static volatile uint8_t rxPending = 0; // 中断和主循环共享，防编译器优化

/*每成功发送一帧后递增，用于确认报文连续变化*/
static uint8_t txSequence;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
 * @brief  The application entry point.
 * @retval int
 */
int main(void) {

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick.
   */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_CAN1_Init();
  MX_USART6_UART_Init();
  /* USER CODE BEGIN 2 */

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

  if (HAL_CAN_ActivateNotification(&hcan1, CAN_IT_RX_FIFO0_MSG_PENDING) !=
      HAL_OK) {
    /* 激活中断失败，进入错误处理 */
    Error_Handler();
  }

  /*设置要发送的CAN帧头*/
  txHeader.StdId = 0x123;                // 11位标准帧ID
  txHeader.ExtId = 0;                    // 标准帧不使用扩展ID
  txHeader.IDE = CAN_ID_STD;             // 标准帧
  txHeader.RTR = CAN_RTR_DATA;           // 数据帧
  txHeader.DLC = 8;                      // 数据长度为 8 字节
  txHeader.TransmitGlobalTime = DISABLE; // 不发送全局时间戳

  printf("CAN1 Sight Start\r\n");

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1) {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    // 主循环检测到新帧标志
    if (rxPending != 0U) 
    {
      // 注意：这里采用了关中断保护"复制＋清标志"
      uint32_t primask = __get_PRIMASK(); /* ① 先记下当前中断开关状态 */
      __disable_irq();                    /* ② 关中断：进入临界区 */
      CAN_RxHeaderTypeDef rxLocal = rxHeader;
      uint8_t rxDataLocal[8];
      for (uint32_t i = 0U; i < 8U; i++) {
        rxDataLocal[i] = rxData[i]; /* ③ 复制（此刻中断不会插入，副本干净） */
      }
      rxPending = 0U; /* ④ 复制完才清标志（顺序正确） */
      if (primask == 0U) {
        __enable_irq(); /* ⑤ 恢复中断：只有进来时是开的才开回去 */
      }

      uint32_t rxId;
      const char *frameType; // 帧格式标签
      const char *frameKind; // 帧类型标签

      if (rxLocal.IDE == CAN_ID_STD) {
        rxId = rxLocal.StdId; // 使用标准 ID
        frameType = "STD";     // 标准帧
      } else {
        rxId = rxLocal.ExtId; // 使用扩展 ID
        frameType = "EXT";     // 扩展帧
      }

      frameKind =
          (rxLocal.RTR == CAN_RTR_DATA) ? "DATA" : "REMOTE"; // 数据帧或远程帧
      printf("CAN1 RX: %s %s ID=0x%lX DLC=%lu Data=", frameType, frameKind,
             (unsigned long)rxId, (unsigned long)rxLocal.DLC);

      if (rxLocal.RTR == CAN_RTR_DATA) 
      {
        for (uint8_t i = 0U; i < rxLocal.DLC && i < 8U; i++) {
          printf("%02X ", (unsigned int)rxDataLocal[i]); // 打印数据字节
        }
      }
      printf("\r\n");
    }

    txData[0] = txSequence; // 将递增的序列号放入数据帧的第一个字节
    txData[1] = 0x11;
    txData[2] = 0x22;
    txData[3] = 0x33;
    txData[4] = 0x44;
    txData[5] = 0x55;
    txData[6] = 0x66;
    txData[7] = 0x77;

    /* 检查 CAN 控制器是否至少有一个空闲发送邮箱 */
    if (HAL_CAN_GetTxMailboxesFreeLevel(&hcan1) > 0) // 检查是否有空闲的发送邮箱
    {
      /* 将帧提交给 HAL/CAN 控制器发送 */
      if (HAL_CAN_AddTxMessage(&hcan1, &txHeader, txData, &txMailbox) ==
          HAL_OK) {
        printf("CAN1 queue: ID:0x123 Sequence=%d\r\n", txSequence);
        txSequence++; // 成功发送后递增序列号
      } else {
        printf("CAN1 Message Send Failed\r\n");
      }
    } else {
      printf("CAN1 No Free Mailbox\r\n");
    }
    HAL_Delay(500); // 延时 500 毫秒
  }
  /* USER CODE END 3 */
}

/**
 * @brief System Clock Configuration
 * @retval None
 */
void SystemClock_Config(void) {
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
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
   */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                                RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK) {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
int __io_putchar(int ch) {
  uint8_t byte = (uint8_t)ch;
  HAL_UART_Transmit(&huart6, &byte, 1, HAL_MAX_DELAY);
  return ch;
}

// 处理接收到的 CAN 消息
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan) {
  if (hcan->Instance == CAN1) // 确认是 CAN1 的中断
  {
    // 从 FIFO0 中读取接收到的消息
    if (HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &rxHeader, rxData) == HAL_OK) {
      rxPending = 1U; // 设置标志，表示有新数据到达
    }
  }
}
/* USER CODE END 4 */

/**
 * @brief  This function is executed in case of error occurrence.
 * @retval None
 */
void Error_Handler(void) {
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
void assert_failed(uint8_t *file, uint32_t line) {
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line
     number, ex: printf("Wrong parameters value: file %s on line %d\r\n", file,
     line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
