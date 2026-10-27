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
#define SD_DMA_BLOCKS 8U  // 每批最多八个扇区，即 4096 字节，不是八帧 CAN 数据。
/* Includes ------------------------------------------------------------------*/
#include "ff_gen_drv.h"
#include "sd_diskio.h"
#include <string.h>

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
#define SD_DEFAULT_BLOCK_SIZE 512  // SD 逻辑扇区大小，单位字节。

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

// FatFs 通过这张函数表调用本文件的磁盘接口，业务层不直接调用 SD_read / SD_write。
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
static SdDiskFault firstFault;  // 首故障现场；SD_initialize 清 valid 后才开始新一轮记录。
// DMA 不能访问 CCM；本缓冲位于默认普通 SRAM 的 .bss，按 INC4 突发要求对齐。
// 所有 FatFs 缓冲均经此处中转，避免未对齐、只读或非 DMA 可访问地址直接传输。
static uint32_t dmaBuffer[SD_DMA_BLOCKS * SD_DEFAULT_BLOCK_SIZE / sizeof(uint32_t)]
    __attribute__((aligned(16)));
static volatile uint8_t dmaActive;     // 主循环建立传输范围，中断回调据此决定是否更新标志。
static volatile uint8_t dmaReadDone;   // 读完成通知，与卡已就绪不是同一条件。
static volatile uint8_t dmaWriteDone;  // 写完成通知，仍须等待卡结束内部处理。
static volatile uint8_t dmaFailed;     // 回调或主循环检测到错误/超时后置位。

// SD DMA 完成回调在中断上下文调用：仅在本次传输有效时置读完成标志，不复制文件数据。
void BSP_SD_ReadCpltCallback(void) { if(dmaActive) dmaReadDone = 1U; }
// SD 写完成回调在中断上下文调用：只置标志，由主循环等待逻辑继续确认卡就绪。
void BSP_SD_WriteCpltCallback(void) { if(dmaActive) dmaWriteDone = 1U; }
// SD 中止回调在中断上下文调用：标记当前传输失败，不在这里重试或调用 FatFs。
void BSP_SD_AbortCallback(void) { if(dmaActive) dmaFailed = 1U; }
// HAL 的 SD 错误回调：只处理本项目 hsd 的活动传输，通知主循环结束等待并保留故障。
void HAL_SD_ErrorCallback(SD_HandleTypeDef *handle)
{
  if(handle == &hsd && dmaActive) dmaFailed = 1U;
}

// 主循环内部调用：只记录初始化后的首个底层故障，后续失败不覆盖原现场。
// 保存阶段、扇区、耗时及 HAL/SDIO 快照，不打印、不再次访问卡；started 单位为 ms。
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

// 主循环错误报告查询：返回首故障的内部只读地址，先判断 valid；不能修改或释放。
const SdDiskFault *SD_GetFirstFault(void) { return &firstFault; }

// 主循环内部调用：轮询卡是否回到可传输状态，成功返回 RES_OK，超时返回 RES_NOTRDY。
// 失败记录首因并置 STA_NOINIT；循环按毫秒检查超时，底层单次状态查询的耗时也占用主循环。
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

// 主循环错误路径调用：尝试中止 SD 和忙碌 DMA，关闭数据通道并锁存未初始化状态。
// 中止返回值在这里未进一步处理，不能仅凭调用就保证硬件停止成功；后续正常读写会被拒绝。
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

// 主循环内部调用：writing 非零为写、零为读，sector 为起始逻辑扇区，blocks 不超过中转容量。
// 启动 DMA 后同步等完成，再等卡就绪；成功返回 RES_OK，错误返回 RES_ERROR 或 RES_NOTRDY。
// DMA 搬运不代表此接口异步返回；等待期间中断可执行，主循环其他业务不能继续推进。
static DRESULT SD_TransferDMA(uint8_t writing, DWORD sector, UINT blocks)
{
  const char *operation = writing ? "write-dma" : "read-dma";
  uint32_t started = HAL_GetTick();
  dmaReadDone = dmaWriteDone = dmaFailed = 0U;
  dmaActive = 1U;
  // 先建立本次传输标志及中转数据的访问顺序，再允许硬件和中断回调参与本次操作。
  __DMB();
  uint8_t status = writing ? BSP_SD_WriteBlocks_DMA(dmaBuffer, sector, blocks) :
                             BSP_SD_ReadBlocks_DMA(dmaBuffer, sector, blocks);
  if(status != MSD_OK) {
    SD_RecordFault(operation, sector, blocks, RES_ERROR, started);
    SD_StopFailedTransfer();
    return RES_ERROR;
  }

  // 这里只等待当前方向的完成标志；硬件错误或超时均转入失败出口，不复用未完成缓冲。
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

// FatFs 经驱动表在主循环调用：初始化 SD 并等待就绪，返回 DSTATUS 状态位。
// 成功清除 STA_NOINIT，失败保留它；lun 当前未使用，不在查询状态时自动重新初始化。
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

// FatFs 调用：只返回缓存的磁盘状态，不发卡命令，不把瞬时忙误判成未初始化。
// 不提供实时物理插卡检测；实际访问错误由传输和同步路径锁存。
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

// FatFs 主循环读接口：从 sector 起读 count 个 512 字节扇区到 buff，lun 当前未使用。
// 调用方提供足够容量；分批 DMA 到中转缓冲，再由 CPU 复制到目标，返回 DRESULT。
DRESULT SD_read(BYTE lun, BYTE *buff, DWORD sector, UINT count)
{
  (void)lun;
  if(buff == NULL || count == 0U) return RES_PARERR;
  if(Stat & STA_NOINIT) return RES_NOTRDY;
  while(count != 0U) {
    UINT blocks = count > SD_DMA_BLOCKS ? SD_DMA_BLOCKS : count;
    DRESULT result = SD_TransferDMA(0U, sector, blocks);
    if(result != RES_OK) return result;
    // DMA 已完成且卡已就绪，才能把中转结果复制出去；原 buff 无需直接满足 DMA 对齐。
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

// FatFs 主循环写接口：将 buff 中 count 个扇区写入 sector 起的区域，lun 当前未使用。
// 先由 CPU 复制到对齐的普通 SRAM 中转区，再启动 DMA；成功返回 RES_OK，失败立即返回错误。
DRESULT SD_write(BYTE lun, const BYTE *buff, DWORD sector, UINT count)
{
  (void)lun;
  if(buff == NULL || count == 0U) return RES_PARERR;
  if(Stat & STA_NOINIT) return RES_NOTRDY;
  while(count != 0U) {
    UINT blocks = count > SD_DMA_BLOCKS ? SD_DMA_BLOCKS : count;
    // 中转保证 DMA 只访问本文件的对齐缓冲，不直接使用可能未对齐或位于 CCM 的源地址。
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
// FatFs 主循环控制接口：同步卡就绪，或把扇区数、扇区大小等结果写入 buff。
// 写回类型由 cmd 决定，调用方须提供相应指针；未就绪返回 RES_NOTRDY，不支持的命令返回 RES_PARERR。
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

  // 当前按逻辑块大小折算，通常返回 1；此实现没有读取卡的真实物理擦除单元。
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

