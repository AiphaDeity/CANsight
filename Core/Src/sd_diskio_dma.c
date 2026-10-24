/**
  ******************************************************************************
  * @file    sd_diskio_dma.c
  * @brief   SD Disk I/O driver
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
// 工程自有 SD 驱动，不由 CubeMX 生成；替代默认 sd_diskio.c 并导出相同 SD_Driver 接口。
// DMA 完成块传输后同步等待卡就绪；根 CMake 配置确保两个实现不会同时编译。

// 软件超时单位为毫秒，不使用值为 0xFFFFFFFF 的硬件数据超时常量。
#define SD_TRANSFER_TIMEOUT_MS 1000U
#define SD_READY_TIMEOUT_MS 500U
#define SD_DMA_BLOCKS 8U
/* Includes ------------------------------------------------------------------*/
#include "ff_gen_drv.h"
#include "sd_diskio.h"
#include <string.h>

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
#define SD_DEFAULT_BLOCK_SIZE 512

/*
 * Depending on the use case, the SD card initialization could be done at the
 * application level: if it is the case define the flag below to disable
 * the BSP_SD_Init() call in the SD_Initialize() and add a call to
 * BSP_SD_Init() elsewhere in the application.
 */
/* #define DISABLE_SD_INIT */
/* Private variables ---------------------------------------------------------*/
/* Disk status */
static volatile DSTATUS Stat = STA_NOINIT;

/* Private function prototypes -----------------------------------------------*/
DSTATUS SD_initialize (BYTE);
DSTATUS SD_status (BYTE);
DRESULT SD_read (BYTE, BYTE*, DWORD, UINT);
#if _USE_WRITE == 1
DRESULT SD_write (BYTE, const BYTE*, DWORD, UINT);
#endif /* _USE_WRITE == 1 */
#if _USE_IOCTL == 1
DRESULT SD_ioctl (BYTE, BYTE, void*);
#endif  /* _USE_IOCTL == 1 */

const Diskio_drvTypeDef  SD_Driver =
{
  SD_initialize,
  SD_status,
  SD_read,
#if  _USE_WRITE == 1
  SD_write,
#endif /* _USE_WRITE == 1 */

#if  _USE_IOCTL == 1
  SD_ioctl,
#endif /* _USE_IOCTL == 1 */
};

extern SD_HandleTypeDef hsd;
static SdDiskFault firstFault;
// DMA 不能访问 CCM；本缓冲位于默认普通 SRAM 的 .bss，按 INC4 突发要求对齐。
// 所有 FatFs 缓冲均经此处中转，避免未对齐、只读或非 DMA 可访问地址直接传输。
static uint32_t dmaBuffer[SD_DMA_BLOCKS * SD_DEFAULT_BLOCK_SIZE / sizeof(uint32_t)]
    __attribute__((aligned(16)));
static volatile uint8_t dmaActive;
static volatile uint8_t dmaReadDone;
static volatile uint8_t dmaWriteDone;
static volatile uint8_t dmaFailed;

// 中断回调仅发布结果，不执行 FatFs、串口输出或接收队列消费。
void BSP_SD_ReadCpltCallback(void) { if(dmaActive) dmaReadDone = 1U; }
void BSP_SD_WriteCpltCallback(void) { if(dmaActive) dmaWriteDone = 1U; }
void BSP_SD_AbortCallback(void) { if(dmaActive) dmaFailed = 1U; }
void HAL_SD_ErrorCallback(SD_HandleTypeDef *handle)
{
  if(handle == &hsd && dmaActive) dmaFailed = 1U;
}

// 在任何重试或后续卡状态查询之前捕获首个失败现场；本函数不执行串口或卡访问。
static void SD_RecordFault(const char *operation, DWORD sector, UINT blocks,
                           DRESULT result, uint32_t started)
{
  if(firstFault.valid != 0U) return;
  firstFault.operation = operation;
  firstFault.sector = (uint32_t)sector;
  firstFault.blocks = (uint32_t)blocks;
  firstFault.disk_result = (uint32_t)result;
  firstFault.elapsed_ms = HAL_GetTick() - started;
  firstFault.hal_error = HAL_SD_GetError(&hsd);
  firstFault.hal_state = (uint32_t)HAL_SD_GetState(&hsd);
  firstFault.sdio_status = READ_REG(hsd.Instance->STA);
  firstFault.sdio_command = READ_REG(hsd.Instance->CMD);
  firstFault.response_command = READ_REG(hsd.Instance->RESPCMD);
  firstFault.response1 = READ_REG(hsd.Instance->RESP1);
  firstFault.clock_control = READ_REG(hsd.Instance->CLKCR);
  firstFault.valid = 1U;
}

const SdDiskFault *SD_GetFirstFault(void) { return &firstFault; }

// 有界等待卡恢复传输状态；失败后交由 FatFs 向上层报告，不在驱动中重试文件操作。
static DRESULT SD_WaitReady(const char *operation, DWORD sector, UINT blocks)
{
  uint32_t started = HAL_GetTick();
  while(BSP_SD_GetCardState() != MSD_OK) {
    if(HAL_GetTick() - started >= SD_READY_TIMEOUT_MS) {
      SD_RecordFault(operation, sector, blocks, RES_NOTRDY, started);
      Stat |= STA_NOINIT;
      return RES_NOTRDY;
    }
  }
  return RES_OK;
}

// 故障现场由调用方预先捕获；停止 DMA 后才允许复用缓冲，故障锁存至重新初始化。
static void SD_StopFailedTransfer(void)
{
  dmaActive = 0U;
  (void)HAL_SD_Abort(&hsd);
  // HAL 出错路径可能已清除 Context，仍须确认两个 DMA 通道均已停止。
  if(hsd.hdmarx != NULL && HAL_DMA_GetState(hsd.hdmarx) == HAL_DMA_STATE_BUSY) {
    (void)HAL_DMA_Abort(hsd.hdmarx);
  }
  if(hsd.hdmatx != NULL && HAL_DMA_GetState(hsd.hdmatx) == HAL_DMA_STATE_BUSY) {
    (void)HAL_DMA_Abort(hsd.hdmatx);
  }
  CLEAR_BIT(hsd.Instance->DCTRL, SDIO_DCTRL_DMAEN | SDIO_DCTRL_DTEN);
  Stat |= STA_NOINIT;
}

// 每次最多传输固定中转缓冲容量；完成回调和卡就绪分别等待，均有超时出口。
static DRESULT SD_TransferDMA(uint8_t writing, DWORD sector, UINT blocks)
{
  const char *operation = writing ? "write-dma" : "read-dma";
  uint32_t started = HAL_GetTick();
  dmaReadDone = dmaWriteDone = dmaFailed = 0U;
  dmaActive = 1U;
  __DMB();
  uint8_t status = writing ? BSP_SD_WriteBlocks_DMA(dmaBuffer, sector, blocks) :
                             BSP_SD_ReadBlocks_DMA(dmaBuffer, sector, blocks);
  if(status != MSD_OK) {
    SD_RecordFault(operation, sector, blocks, RES_ERROR, started);
    SD_StopFailedTransfer();
    return RES_ERROR;
  }

  while((writing ? dmaWriteDone : dmaReadDone) == 0U && dmaFailed == 0U) {
    if(HAL_SD_GetError(&hsd) != HAL_SD_ERROR_NONE ||
       HAL_GetTick() - started >= SD_TRANSFER_TIMEOUT_MS) {
      dmaFailed = 1U;
    }
  }
  if(dmaFailed || HAL_SD_GetError(&hsd) != HAL_SD_ERROR_NONE) {
    SD_RecordFault(operation, sector, blocks, RES_ERROR, started);
    SD_StopFailedTransfer();
    return RES_ERROR;
  }
  __DMB();  // 完成回调发布后，读取端才可复制 DMA 缓冲中的数据。
  dmaActive = 0U;
  return SD_WaitReady(writing ? "write-ready" : "read-ready", sector, blocks);
}
/* Private functions ---------------------------------------------------------*/

/**
  * @brief  Initializes a Drive
  * @param  lun : not used
  * @retval DSTATUS: Operation status
  */
DSTATUS SD_initialize(BYTE lun)
{
  (void)lun;
  firstFault.valid = 0U;
  Stat = STA_NOINIT;

#if !defined(DISABLE_SD_INIT)
  uint32_t started = HAL_GetTick();
  if(BSP_SD_Init() != MSD_OK) {
    SD_RecordFault("initialize", 0U, 0U, RES_NOTRDY, started);
    return Stat;
  }
#endif

  // 仅在初始化成功且卡已就绪后清除 NOINIT；普通状态读取不得重置该标志。
  if(SD_WaitReady("init-ready", 0U, 0U) == RES_OK) {
    Stat &= (DSTATUS)~STA_NOINIT;
  }
  return Stat;
}

/**
  * @brief  Gets Disk Status
  * @param  lun : not used
  * @retval DSTATUS: Operation status
  */
DSTATUS SD_status(BYTE lun)
{
  (void)lun;
  // FatFs 在文件和目录校验时调用本函数，NOINIT 代表初始化状态而非瞬时忙碌。
  // 不在此处发送 CMD13；实际块传输、CTRL_SYNC 的有界等待负责检测并锁存故障。
  return Stat;
}

/**
  * @brief  Reads Sector(s)
  * @param  lun : not used
  * @param  *buff: Data buffer to store read data
  * @param  sector: Sector address (LBA)
  * @param  count: Number of sectors to read (1..128)
  * @retval DRESULT: Operation result
  */

DRESULT SD_read(BYTE lun, BYTE *buff, DWORD sector, UINT count)
{
  (void)lun;
  if(buff == NULL || count == 0U) return RES_PARERR;
  if(Stat & STA_NOINIT) return RES_NOTRDY;
  while(count != 0U) {
    UINT blocks = count > SD_DMA_BLOCKS ? SD_DMA_BLOCKS : count;
    DRESULT result = SD_TransferDMA(0U, sector, blocks);
    if(result != RES_OK) return result;
    memcpy(buff, dmaBuffer, blocks * SD_DEFAULT_BLOCK_SIZE);
    buff += blocks * SD_DEFAULT_BLOCK_SIZE;
    sector += blocks;
    count -= blocks;
  }
  return RES_OK;
}

/**
  * @brief  Writes Sector(s)
  * @param  lun : not used
  * @param  *buff: Data to be written
  * @param  sector: Sector address (LBA)
  * @param  count: Number of sectors to write (1..128)
  * @retval DRESULT: Operation result
  */
#if _USE_WRITE == 1

DRESULT SD_write(BYTE lun, const BYTE *buff, DWORD sector, UINT count)
{
  (void)lun;
  if(buff == NULL || count == 0U) return RES_PARERR;
  if(Stat & STA_NOINIT) return RES_NOTRDY;
  while(count != 0U) {
    UINT blocks = count > SD_DMA_BLOCKS ? SD_DMA_BLOCKS : count;
    memcpy(dmaBuffer, buff, blocks * SD_DEFAULT_BLOCK_SIZE);
    DRESULT result = SD_TransferDMA(1U, sector, blocks);
    if(result != RES_OK) return result;
    buff += blocks * SD_DEFAULT_BLOCK_SIZE;
    sector += blocks;
    count -= blocks;
  }
  return RES_OK;
}
#endif /* _USE_WRITE == 1 */

/**
  * @brief  I/O control operation
  * @param  lun : not used
  * @param  cmd: Control code
  * @param  *buff: Buffer to send/receive control data
  * @retval DRESULT: Operation result
  */
#if _USE_IOCTL == 1
DRESULT SD_ioctl(BYTE lun, BYTE cmd, void *buff)
{
  DRESULT res = RES_ERROR;
  BSP_SD_CardInfo CardInfo;

  if (Stat & STA_NOINIT) return RES_NOTRDY;

  switch (cmd)
  {
  /* Make sure that no pending write process */
  case CTRL_SYNC :
    res = SD_WaitReady("sync-ready", 0U, 0U);
    break;

  /* Get number of sectors on the disk (DWORD) */
  case GET_SECTOR_COUNT :
    BSP_SD_GetCardInfo(&CardInfo);
    *(DWORD*)buff = CardInfo.LogBlockNbr;
    res = RES_OK;
    break;

  /* Get R/W sector size (WORD) */
  case GET_SECTOR_SIZE :
    BSP_SD_GetCardInfo(&CardInfo);
    *(WORD*)buff = CardInfo.LogBlockSize;
    res = RES_OK;
    break;

  /* Get erase block size in unit of sector (DWORD) */
  case GET_BLOCK_SIZE :
    BSP_SD_GetCardInfo(&CardInfo);
    *(DWORD*)buff = CardInfo.LogBlockSize / SD_DEFAULT_BLOCK_SIZE;
    res = RES_OK;
    break;

  default:
    res = RES_PARERR;
  }

  return res;
}
#endif /* _USE_IOCTL == 1 */

