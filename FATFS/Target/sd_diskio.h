/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    sd_diskio.h
  * @brief   Header for sd_diskio.c module
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

/* Note: code generation based on sd_diskio_template.h */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __SD_DISKIO_H
#define __SD_DISKIO_H

/* USER CODE BEGIN firstSection */
/* can be used to modify / undefine following code or add new definitions */
/* USER CODE END firstSection */

/* Includes ------------------------------------------------------------------*/
#include "bsp_driver_sd.h"
/* Exported types ------------------------------------------------------------*/
/* Exported constants --------------------------------------------------------*/
/* Exported functions ------------------------------------------------------- */
extern const Diskio_drvTypeDef  SD_Driver;

/* USER CODE BEGIN lastSection */
// 保留首次块传输或卡就绪等待失败现场，后续文件访问不得覆盖首因。
// hal_error 使用 HAL_SD_ERROR_* 位；sdio_status 是 HAL 返回后的寄存器快照。
typedef struct {
  const char *operation;     // read-dma、write-dma、read-ready、write-ready 或 sync-ready。
  uint32_t sector;           // 失败请求的起始逻辑扇区；独立同步时为 0。
  uint32_t blocks;           // 请求扇区数；独立同步时为 0。
  uint32_t disk_result;      // 本层返回的 RES_ERROR 或 RES_NOTRDY。
  uint32_t elapsed_ms;       // 当前块传输或就绪等待阶段的耗时。
  uint32_t hal_error;        // 在后续状态查询可能改写之前保存 HAL 错误位。
  uint32_t hal_state;        // HAL SD 驱动状态，不等同于卡片协议状态。
  uint32_t sdio_status;      // 部分硬件错误位可能已被 HAL 清除，以 hal_error 为准。
  uint8_t valid;             // 初始化后尚未发生故障时为 0。
} SdDiskFault;

// 只读接口，仅供主循环文件系统调用链及错误报告使用，不在中断中调用。
const SdDiskFault *SD_GetFirstFault(void);
/* USER CODE END lastSection */

#endif /* __SD_DISKIO_H */
