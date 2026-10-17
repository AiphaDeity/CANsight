#include "can_logger.h"

#include "fatfs.h"
#include "main.h"
#include "stdio.h"
#include "string.h"

/* 一次攒一批文本再写卡，减少小块 SD 写入次数。 */
#define CAN_LOG_BUFFER_SIZE       4096U  //批量缓冲区大小
#define CAN_LOG_SYNC_INTERVAL_MS  1000U  //每秒同步一次文件，降低突然断电时的丢失量
#define CAN_LOG_FILE_NAME         "CANLOG.CSV"  //日志文件名

static FIL logFile;  //当前打开的日志文件对象
static uint8_t logReady = 0U;  // 1 表示 SD 卡已挂载并打开日志文件，0 表示不可写
static uint8_t logErrorReported = 0U;  // 错误只报告一次，避免刷屏
static char logBuffer[CAN_LOG_BUFFER_SIZE];  // RAM 缓冲区，存放 CSV 文本
static UINT logBufferLength = 0U;  // RAM 缓冲区中已使用的字节数
static uint32_t lastSyncTick = 0U;  // 上次同步文件的 HAL_GetTick() 时间戳
static uint32_t loggedFrameCount = 0U;  // 累计写入日志的报文帧数
static uint8_t firstWriteSyncReported = 0U;  // 首批报文完成写入并同步后提示一次，便于不拔卡确认日志工作

/* 发生 SD/FatFs 错误后停止继续写入，并只报告一次，SD卡故障时持续性错误。 */
static void CanLogger_StopWithError(const char *stage, FRESULT result)
{
  logReady = 0U;  //关闭写入

  if (logErrorReported == 0U)  //只报告一次错误，避免刷屏
  {
    printf("SD LOG: error at %s, FatFs result=%d\r\n",
           stage, (int)result);
    logErrorReported = 1U;  //标记已报告
  }
}

/* 把 RAM 中积累的 CSV 内容写入当前文件。 */
static uint8_t CanLogger_FlushBuffer(void)
{
  FRESULT result;  //FatFs 函数返回值
  UINT bytesWritten = 0U;  //实际写入的字节数

  if (logBufferLength == 0U)  //没有内容需要写入
  {
    return 1U;  //返回成功
  }

  result = f_write(&logFile, logBuffer, logBufferLength, &bytesWritten);  //写入文件
  if ((result != FR_OK) || (bytesWritten != logBufferLength))  //写入失败或写入字节数不符
  {
    CanLogger_StopWithError("write", result); //停止写入并报告错误
    return 0U;
  }

  logBufferLength = 0U;  //清空缓冲区
  return 1U;
}

/* 挂载文件系统并以追加方式打开日志文件。 */
uint8_t CanLogger_Init(void)
{
  FRESULT result;  
  char filePath[32];  //日志文件完整路径
  int pathLength;  //日志文件路径长度

  logReady = 0U;  //初始化为不可写
  logErrorReported = 0U;  //初始化为未报告错误
  logBufferLength = 0U;  //清空缓冲区
  loggedFrameCount = 0U;  //累计写入日志的报文帧数
  firstWriteSyncReported = 0U;  //首批报文完成写入并同步后提示一次，便于不拔卡确认日志工作

  if (retSD != 0U)  //FatFs 驱动未链接成功
  {
    printf("SD LOG: FatFs driver link failed, retSD=%u\r\n",
           (unsigned int)retSD);
    return 0U;
  }

  /* 立即挂载；若卡或文件系统不可用，不影响 CAN 启动。 */
  result = f_mount(&SDFatFS, (TCHAR const *)SDPath, 1U);
  if (result != FR_OK)
  {
    printf("SD LOG: mount failed, FatFs result=%d\r\n", (int)result);
    return 0U;
  }

  pathLength = snprintf(filePath, sizeof(filePath), "%s/%s",
                        SDPath, CAN_LOG_FILE_NAME);
  if ((pathLength < 0) || (pathLength >= (int)sizeof(filePath)))
  {
    printf("SD LOG: log file path is too long\r\n");
    (void)f_mount(NULL, (TCHAR const *)SDPath, 0U);
    return 0U;
  }

  /* 文件不存在时创建；已存在时保留旧记录并从文件末尾继续追加。 */
  result = f_open(&logFile, filePath, FA_OPEN_ALWAYS | FA_WRITE);
  if (result != FR_OK)
  {
    printf("SD LOG: open failed, FatFs result=%d\r\n", (int)result);
    (void)f_mount(NULL, (TCHAR const *)SDPath, 0U);
    return 0U;
  }

  if (f_size(&logFile) == 0U)  //文件为空，新建文件时写入 CSV 表头
  {
    static const char csvHeader[] =
        "timestamp_us,format,type,id,dlc,data_hex\r\n";  //CSV 表头
    UINT bytesWritten = 0U;

    result = f_write(&logFile, csvHeader,
                     (UINT)(sizeof(csvHeader) - 1U), &bytesWritten);  //写入表头
    if ((result != FR_OK) || (bytesWritten != (UINT)(sizeof(csvHeader) - 1U)))  //写入失败或写入字节数不符
    {
      CanLogger_StopWithError("write header", result);
      (void)f_close(&logFile);  //关闭文件
      (void)f_mount(NULL, (TCHAR const *)SDPath, 0U);  //卸载文件系统
      return 0U;  
    }
  }
  else
  {
    result = f_lseek(&logFile, f_size(&logFile));  //将文件指针移动到文件末尾，以便追加新日志
    if (result != FR_OK)  //移动文件指针失败
    {
      CanLogger_StopWithError("seek to end", result);  //停止写入并报告错误
      (void)f_close(&logFile);  //关闭文件
      (void)f_mount(NULL, (TCHAR const *)SDPath, 0U);  //卸载文件系统
      return 0U;
    }
  }

  result = f_sync(&logFile);  //同步文件，确保文件系统元数据和日志文件内容一致
  if (result != FR_OK)  //同步失败
  {
    CanLogger_StopWithError("initial sync", result);  //停止写入并报告错误
    (void)f_close(&logFile);
    (void)f_mount(NULL, (TCHAR const *)SDPath, 0U);
    return 0U;
  }

  logReady = 1U;  //标记 SD 日志可写
  lastSyncTick = HAL_GetTick();  //记录当前时间戳，用于后续每秒同步文件
  printf("SD LOG: recording to %s\r\n", filePath);
  return 1U;
}

/* 把一帧格式化成CSV行并放入RAM缓冲区。 */
void CanLogger_AddFrame(const CanFrame *frame)
{
  char row[96];  //一帧报文格式化成的 CSV 行，足够容纳最长的行
  int rowLength;  //格式化后的行长度
  int used;  //已使用的缓冲区字节数

  if ((logReady == 0U) || (frame == NULL))  //如果日志不可写或帧指针为空，则直接返回
  {
    return;
  }

  rowLength = snprintf(row, sizeof(row), "%lu,%s,%s,0x%lX,%u,",
                      (unsigned long)frame->timestamp_us,
                      (frame->ide == CAN_ID_STD) ? "STD" : "EXT",
                      (frame->rtr == CAN_RTR_DATA) ? "DATA" : "REMOTE",
                      (unsigned long)frame->id,
                      (unsigned int)frame->dlc);
  if ((rowLength < 0) || (rowLength >= (int)sizeof(row)))  //格式化失败或行长度超过缓冲区大小
  {
    CanLogger_StopWithError("format record", FR_INT_ERR);
    return;
  }

  used = rowLength;  //记录已使用的缓冲区字节数
  if (frame->rtr == CAN_RTR_DATA)  //如果是数据帧，则格式化数据内容
  {
    uint8_t dataLength = (frame->dlc <= 8U) ? frame->dlc : 8U;  //确保数据长度不超过 8 字节
    for (uint8_t i = 0U; i < dataLength; i++)  //遍历数据字节
    {
      int written = snprintf(&row[used], sizeof(row) - (size_t)used,
                             (i == 0U) ? "%02X" : " %02X",
                             (unsigned int)frame->data[i]);
      if ((written < 0) || (written >= (int)(sizeof(row) - (size_t)used)))  //格式化失败或写入字节数不符
      {
        CanLogger_StopWithError("format data", FR_INT_ERR);  //停止写入并报告错误
        return;
      }
      used += written;   //记录已使用的缓冲区字节数
    } 
  }

  
  if ((used + 2) >= (int)sizeof(row))  //行结尾格式化失败或行长度超过缓冲区大小
  {
    CanLogger_StopWithError("format line ending", FR_INT_ERR);
    return;
  }
  row[used++] = '\r';
  row[used++] = '\n';

  /* 若当前行长度超过缓冲区大小，先写完已有内容，再放入新行。 */
  if ((UINT)used > (CAN_LOG_BUFFER_SIZE - logBufferLength)) 
  {
    if (CanLogger_FlushBuffer() == 0U)  //缓冲区写满时立即写卡
    {
      return;
    }
  }

  memcpy(&logBuffer[logBufferLength], row, (size_t)used);  //将行放入缓冲区
  logBufferLength += (UINT)used;  //记录已使用的缓冲区字节数
  loggedFrameCount++;  //记录已写入的帧数
}

/* 在主循环调用；SD 文件操作不在 CAN 中断中执行。 */
void CanLogger_Process(void)
{
  FRESULT result;  

  if (logReady == 0U)  //如果日志不可写，则直接返回
  {
    return;
  }

  /* 缓冲区写满时立即写卡，避免覆盖尚未保存的报文。 */
  if (logBufferLength == CAN_LOG_BUFFER_SIZE)
  {
    if (CanLogger_FlushBuffer() == 0U)  //缓冲区写满时立即写卡
    {
      return;
    }
  }

  /* 每秒写出尚未满缓冲区的记录并同步，降低突然断电时的丢失量。 */
  if ((HAL_GetTick() - lastSyncTick) >= CAN_LOG_SYNC_INTERVAL_MS)
  {
    if (CanLogger_FlushBuffer() == 0U)  
    {
      return;
    }

    result = f_sync(&logFile);  //同步文件
    if (result != FR_OK)  //同步失败
    {
      CanLogger_StopWithError("sync", result);  //停止写入并报告错误
      return;
    }

    lastSyncTick = HAL_GetTick();  //记录当前时间戳

    /* 首批报文完成写入并同步后提示一次，便于不拔卡确认日志工作。 */
    if ((loggedFrameCount > 0U) && (firstWriteSyncReported == 0U))
    {
      printf("SD LOG: first CAN batch synced, frames=%lu\r\n",
             (unsigned long)loggedFrameCount);
      firstWriteSyncReported = 1U;  //标记已报告
    }
  }
}

/* 停止写入并报告错误。 */
uint8_t CanLogger_IsReady(void)  
{
  return logReady;
}
