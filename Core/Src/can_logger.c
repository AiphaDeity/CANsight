// SD 日志业务模块：主循环把接收帧追加为 CSV，再按缓冲容量、同步间隔和归档条件写卡。
// 所有文件操作和本模块状态都由主循环上下文使用；printf 只输出诊断，不在中断调用。
#include "can_logger.h"
#include "can_time.h"

#include "fatfs.h"
#include "main.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

// 一次攒一批文本再写卡，减少小块 SD 写入次数。
#define CAN_LOG_BUFFER_SIZE       4096U  // CSV 文本缓冲容量，单位字节，不是帧数。
#define CAN_LOG_SYNC_INTERVAL_MS  1000U  // 周期写出并同步的最小间隔，单位 ms。
#define CAN_LOG_FILE_NAME         "CANLOG.CSV"  // 当前活动日志名，成功归档后继续创建同名新文件。
#define CAN_LOG_ARCHIVE_MAX       999999999U  // 历史名称最多支持九位十进制编号。

// 新格式时间是 64 位微秒；旧表头只用于识别启动时留下的历史格式。
static const char csvHeader[] = "timestamp_us64,format,type,id,dlc,data_hex\r\n";
static const char legacyHeader[] = "timestamp_us,format,type,id,dlc,data_hex\r\n";
static CanArchiveState archiveState = CAN_ARCHIVE_IDLE;  // 请求待执行、完成、空文件或失败。
static char lastArchiveName[CAN_LOG_ARCHIVE_NAME_SIZE];
static uint32_t nextArchiveNumber = 1U;  // 下一个待尝试的归档编号，遇到同名对象继续递增。
static uint32_t archiveRequestTick;  // 手动归档受理时的毫秒时刻，用于稍后执行文件操作。
static uint32_t archiveRevision;  // 成功生成归档后的变化标记，通知浏览模块刷新目录。

static FIL logFile;  //当前打开的日志文件对象
static uint8_t logReady = 0U;  // 1 表示 SD 卡已挂载并打开日志文件，0 表示不可写
static CanLoggerState logState = CAN_LOG_UNMOUNTED;
static FRESULT lastError = FR_OK;  // 最近一次日志错误；成功初始化后清零。
static uint8_t logErrorReported = 0U;  // 错误只报告一次，避免刷屏
static char logBuffer[CAN_LOG_BUFFER_SIZE];  // RAM 缓冲区，存放 CSV 文本
static UINT logBufferLength = 0U;  // RAM 缓冲区中已使用的字节数
static uint32_t lastSyncTick = 0U;  // 上次同步文件的 HAL_GetTick() 时间戳
static uint32_t loggedFrameCount = 0U;  // 当前活动文件已接纳的帧数，包含 RAM 缓冲。
static CanLoggerDiagnostics diagnostics;  // 跨归档保留本次初始化以来的累计诊断。
static uint8_t logHasFrames;  // 独立记录非空状态，避免累计计数回绕后误判为空。
static uint8_t firstWriteSyncReported = 0U;  // 首批报文完成写入并同步后提示一次，便于不拔卡确认日志工作

// 内部调用：禁止继续接纳日志，保存本次错误并将待归档请求置为失败。
// 首次错误时输出底层首故障现场；不自动重挂载、重试写入或修复文件。
static void CanLogger_StopWithError(const char *stage, FRESULT result)
{
  logReady = 0U;  //关闭写入
  logState = CAN_LOG_ERROR;
  lastError = result;
  if(archiveState == CAN_ARCHIVE_PENDING) archiveState = CAN_ARCHIVE_FAILED;

  if (logErrorReported == 0U)  //只报告一次错误，避免刷屏
  {
    printf("SD LOG: error at %s, FatFs result=%d\r\n",
           stage, (int)result);
    // 只在日志首次停止时输出底层首故障，不在扇区读写或接收中断内打印。
    const SdDiskFault *fault = SD_GetFirstFault();
    if(fault->valid != 0U) {
      printf("SD IO first fault: op=%s sector=%lu blocks=%lu result=%lu elapsed_ms=%lu\r\n",
             fault->operation, (unsigned long)fault->sector,
             (unsigned long)fault->blocks, (unsigned long)fault->disk_result,
             (unsigned long)fault->elapsed_ms);
      printf("SD IO: HAL=0x%08lX state=%lu STA=0x%08lX\r\n",
             (unsigned long)fault->hal_error, (unsigned long)fault->hal_state,
             (unsigned long)fault->sdio_status);
      printf("SD CMD: cmd=%lu respcmd=%lu RESP1=0x%08lX CLKCR=0x%08lX ILLEGAL_CMD=%u\r\n",
             (unsigned long)(fault->sdio_command & 0x3FU),
             (unsigned long)fault->response_command,
             (unsigned long)fault->response1, (unsigned long)fault->clock_control,
             (unsigned)((fault->hal_error & HAL_SD_ERROR_ILLEGAL_CMD) != 0U));
      printf("SD IO: TX_UNDERRUN=%u RX_OVERRUN=%u DATA_CRC_FAIL=%u DATA_TIMEOUT=%u CMD_TIMEOUT=%u\r\n",
             (unsigned)((fault->hal_error & HAL_SD_ERROR_TX_UNDERRUN) != 0U),
             (unsigned)((fault->hal_error & HAL_SD_ERROR_RX_OVERRUN) != 0U),
             (unsigned)((fault->hal_error & HAL_SD_ERROR_DATA_CRC_FAIL) != 0U),
             (unsigned)((fault->hal_error & HAL_SD_ERROR_DATA_TIMEOUT) != 0U),
             (unsigned)((fault->hal_error & HAL_SD_ERROR_CMD_RSP_TIMEOUT) != 0U));
    }
    if(result == FR_INVALID_OBJECT) {
      // 比较已知对象字段，不解引用可能损坏的 logFile.obj.fs 指针。
      // 磁盘状态查询仅读取驱动缓存，不在错误报告中再次访问 SD 卡。
      printf("SD OBJ: fs_match=%u file_id=%u volume_id=%u fs_type=%u disk_status=0x%02X\r\n",
             (unsigned)(logFile.obj.fs == &SDFatFS), (unsigned)logFile.obj.id,
             (unsigned)SDFatFS.id, (unsigned)SDFatFS.fs_type,
             (unsigned)SD_Driver.disk_status(0U));
    }
    logErrorReported = 1U;  // 首故障不会被后续历史读取或容量查询错误替代。
  }
}


// 内部调用：同步当前文件，返回原始 FatFs 结果；FR_OK 时更新 synced_frames。
// 同步计数对应最近一次成功同步的累计写入数，不是本次新增帧数。
static FRESULT CanLogger_SyncFile(void)
{
  FRESULT result = f_sync(&logFile);
  if(result == FR_OK) diagnostics.synced_frames = diagnostics.written_frames;
  return result;
}

// 内部调用：将当前 CSV 缓冲完整写入文件，成功或缓冲为空返回 1，失败返回 0。
// 同时检查 FatFs 状态与实际字节数；失败不清空缓冲，也不自动重复写入可能已部分写出的内容。
static uint8_t CanLogger_FlushBuffer(void)
{
  FRESULT result;  //FatFs 函数返回值
  UINT bytesWritten = 0U;  //实际写入的字节数

  if (logBufferLength == 0U)  //没有内容需要写入
  {
    return 1U;  //返回成功
  }

  result = f_write(&logFile, logBuffer, logBufferLength, &bytesWritten);
  if ((result != FR_OK) || (bytesWritten != logBufferLength))  //写入失败或写入字节数不符
  {
    CanLogger_StopWithError("write", result == FR_OK ? FR_DISK_ERR : result);
    return 0U;
  }

  // 仅整块写入成功才把缓冲帧计入 written；文件写入和后续 f_sync 是两个阶段。
  diagnostics.written_frames += diagnostics.buffered_frames;
  diagnostics.buffered_frames = 0U;
  logBufferLength = 0U;  // 完整缓冲写入成功后才释放，部分写入不计入完成帧数。
  return 1U;
}

// 启动扫描内部调用：从 CANLOG_ 加三至九位编号及 .CSV 的名称提取编号，忽略大小写。
// 不匹配或编号为零返回 0；这里只识别编号，浏览模块另有更严格的规范名称检查。
static uint32_t CanLogger_ParseArchiveNumber(const char *name)
{
  const char *prefix = "CANLOG_";
  for(unsigned i = 0; i < 7U; i++) {
    if(toupper((unsigned char)name[i]) != prefix[i]) return 0U;
  }
  const char *digits = name + 7;
  uint32_t number = 0U;
  unsigned length = 0U;
  while(*digits >= '0' && *digits <= '9') {
    if(++length > 9U) return 0U;
    number = number * 10U + (uint32_t)(*digits++ - '0');
  }
  if(length < 3U || strlen(digits) != 4U || digits[0] != '.' ||
     toupper((unsigned char)digits[1]) != 'C' ||
     toupper((unsigned char)digits[2]) != 'S' ||
     toupper((unsigned char)digits[3]) != 'V') return 0U;
  return number;
}

// 启动内部调用：同步遍历根目录，在已有最大归档编号后分配下一个编号。
// 返回 FR_OK 或文件系统错误；实际重命名前还会检查目标是否已存在，避免覆盖。
static FRESULT CanLogger_FindNextNumber(void)
{
  DIR directory;
  FILINFO info;
  FRESULT result = f_opendir(&directory, SDPath);
  if(result != FR_OK) return result;
  uint32_t maximum = 0U;
  while((result = f_readdir(&directory, &info)) == FR_OK && info.fname[0] != '\0') {
    uint32_t number = CanLogger_ParseArchiveNumber(info.fname);
    if(number > maximum) maximum = number;
  }
  FRESULT closeResult = f_closedir(&directory);
  if(result != FR_OK) return result;
  if(closeResult != FR_OK) return closeResult;
  nextArchiveNumber = maximum + 1U;
  return FR_OK;
}

// 内部调用，前提是活动文件已关闭：将 CANLOG.CSV 重命名为下一个可用归档名。
// 成功更新最近归档名和版本号，返回 FR_OK；同名文件或目录占用时跳号，编号耗尽或出错则返回错误。
static FRESULT CanLogger_RenameActive(void)
{
  char source[32];
  char target[48];
  char name[CAN_LOG_ARCHIVE_NAME_SIZE];
  FILINFO info;
  snprintf(source, sizeof(source), "%s/%s", SDPath, CAN_LOG_FILE_NAME);
  while(nextArchiveNumber <= CAN_LOG_ARCHIVE_MAX) {
    snprintf(name, sizeof(name), "CANLOG_%03lu.CSV", (unsigned long)nextArchiveNumber++);
    snprintf(target, sizeof(target), "%s/%s", SDPath, name);
    FRESULT result = f_stat(target, &info);
    if(result == FR_OK) continue;
    if(result != FR_NO_FILE) return result;
    result = f_rename(source, target);
    if(result == FR_EXIST) continue;
    if(result != FR_OK) return result;
    snprintf(lastArchiveName, sizeof(lastArchiveName), "%s", name);
    archiveRevision++;
    printf("SD LOG: archived to %s\r\n", target);
    return FR_OK;
  }
  return FR_DENIED;
}

// 启动内部调用：检查上次留下的活动文件，只把零长度或内容完全匹配表头的文件视为空。
// 已有其他内容时先归档保存；旧格式空表头删后重建，返回 FatFs 结果。
static FRESULT CanLogger_ArchivePrevious(void)
{
  char path[32];
  char header[sizeof(csvHeader) - 1U];
  FILINFO info;
  snprintf(path, sizeof(path), "%s/%s", SDPath, CAN_LOG_FILE_NAME);
  FRESULT result = f_stat(path, &info);
  if(result == FR_NO_FILE) return FR_OK;
  if(result != FR_OK) return result;
  if((info.fattrib & AM_DIR) != 0U) return FR_DENIED;
  if(info.fsize == 0U) return FR_OK;
  if(info.fsize == sizeof(csvHeader) - 1U || info.fsize == sizeof(legacyHeader) - 1U) {
    UINT bytesRead = 0U;
    result = f_open(&logFile, path, FA_READ);
    if(result != FR_OK) return result;
    result = f_read(&logFile, header, sizeof(header), &bytesRead);
    FRESULT closeResult = f_close(&logFile);
    if(result != FR_OK) return result;
    if(closeResult != FR_OK) return closeResult;
    if((bytesRead == sizeof(header) && memcmp(header, csvHeader, sizeof(header)) == 0) ||
       (bytesRead == sizeof(legacyHeader) - 1U && memcmp(header, legacyHeader, bytesRead) == 0)) {
      // 旧版仅含表头的活动文件可删除后重建，不能沿用旧表头写入新时间格式。
      if(info.fsize == sizeof(legacyHeader) - 1U) return f_unlink(path);
      return FR_OK;
    }
  }
  result = CanLogger_RenameActive();
  if(result == FR_OK) archiveState = CAN_ARCHIVE_DONE;
  return result;
}

// 内部调用：打开活动日志，写入新表头或复用此前已确认的空日志，不截断已有数据。
// 成功清空本文件计数并进入 RECORDING，返回 1；失败停止日志并返回 0。
static uint8_t CanLogger_OpenActive(void)
{
  char path[32];
  snprintf(path, sizeof(path), "%s/%s", SDPath, CAN_LOG_FILE_NAME);
  FRESULT result = f_open(&logFile, path, FA_OPEN_ALWAYS | FA_WRITE);
  if(result != FR_OK) {
    CanLogger_StopWithError("open", result);
    return 0U;
  }
  if(f_size(&logFile) == 0U) {
    UINT bytesWritten = 0U;
    result = f_write(&logFile, csvHeader, sizeof(csvHeader) - 1U, &bytesWritten);
    if(result == FR_OK && bytesWritten != sizeof(csvHeader) - 1U) result = FR_DISK_ERR;
  }
  else if(f_size(&logFile) == sizeof(csvHeader) - 1U) {
    result = f_lseek(&logFile, f_size(&logFile));
  }
  else {
    result = FR_EXIST;
  }
  if(result == FR_OK) result = CanLogger_SyncFile();
  if(result != FR_OK) {
    CanLogger_StopWithError("prepare active", result);
    (void)f_close(&logFile);
    return 0U;
  }
  logBufferLength = 0U;
  loggedFrameCount = 0U;
  logHasFrames = 0U;
  firstWriteSyncReported = 0U;
  lastSyncTick = HAL_GetTick();
  logReady = 1U;
  logState = CAN_LOG_RECORDING;
  printf("SD LOG: recording to %s\r\n", path);
  return 1U;
}

// main 启动时调用一次：重置诊断、挂载卷、查找编号、归档旧文件并准备新活动日志。
// 返回 1 表示可记录，0 表示失败；不在运行中用此函数强行重置仍打开的文件对象。
uint8_t CanLogger_Init(void)
{
  memset(&diagnostics, 0, sizeof(diagnostics));
  logReady = 0U;
  logState = CAN_LOG_UNMOUNTED;
  lastError = FR_OK;
  logErrorReported = 0U;
  logBufferLength = 0U;
  loggedFrameCount = 0U;
  firstWriteSyncReported = 0U;
  archiveState = CAN_ARCHIVE_IDLE;
  archiveRevision = 0U;
  lastArchiveName[0] = '\0';
  if(retSD != 0U) {
    lastError = FR_NOT_ENABLED;
    printf("SD LOG: FatFs driver link failed, retSD=%u\r\n", (unsigned)retSD);
    return 0U;
  }
  FRESULT result = f_mount(&SDFatFS, (TCHAR const *)SDPath, 1U);
  if(result != FR_OK) {
    lastError = result;
    printf("SD LOG: mount failed, FatFs result=%d\r\n", (int)result);
    return 0U;
  }
  logState = CAN_LOG_READY;
  result = CanLogger_FindNextNumber();
  if(result == FR_OK) result = CanLogger_ArchivePrevious();
  if(result != FR_OK) {
    archiveState = CAN_ARCHIVE_FAILED;
    CanLogger_StopWithError("startup archive", result);
    return 0U;
  }
  if(CanLogger_OpenActive() == 0U) {
    archiveState = CAN_ARCHIVE_FAILED;
    return 0U;
  }
  return 1U;
}

// 主循环内部调用：刷出缓冲、同步并关闭当前文件，再重命名并创建新日志。
// 没有帧则发布 EMPTY；任何阶段失败会停止日志，通过状态发布结果而非自动重试。
static void CanLogger_ArchiveCurrent(void)
{
  if(logHasFrames == 0U) {
    archiveState = CAN_ARCHIVE_EMPTY;
    printf("SD LOG: archive skipped, no frames\r\n");
    return;
  }
  if(CanLogger_FlushBuffer() == 0U) {
    archiveState = CAN_ARCHIVE_FAILED;
    return;
  }
  FRESULT result = CanLogger_SyncFile();
  if(result == FR_OK) result = f_close(&logFile);
  if(result != FR_OK) {
    CanLogger_StopWithError("archive close", result);
    archiveState = CAN_ARCHIVE_FAILED;
    return;
  }
  logReady = 0U;
  logState = CAN_LOG_READY;
  result = CanLogger_RenameActive();
  if(result != FR_OK) {
    CanLogger_StopWithError("archive rename", result);
    archiveState = CAN_ARCHIVE_FAILED;
    return;
  }
  archiveState = CanLogger_OpenActive() ? CAN_ARCHIVE_DONE : CAN_ARCHIVE_FAILED;
}

// 界面归档回调在主循环上下文调用：只提交请求，返回 1 表示受理，0 表示拒绝。
// 暂停、日志不可用或已有请求时不受理；真正归档由主循环 Process 处理。
uint8_t CanLogger_RequestArchive(void)
{
  if(logState == CAN_LOG_PAUSED) return 0U;
  if(archiveState == CAN_ARCHIVE_PENDING) return 0U;
  if(logReady == 0U) {
    archiveState = CAN_ARCHIVE_FAILED;
    return 0U;
  }
  archiveState = CAN_ARCHIVE_PENDING;
  archiveRequestTick = HAL_GetTick();
  return 1U;
}

// 主循环每取出一帧后调用：格式化一整行 CSV，追加到 RAM 缓冲并累计接纳数。
// 容量不足或跨文件上限时可能同步写卡、归档；返回不等于该帧已经同步到 SD。
// 回放暂停记录期间直接跳过，不累计 rejected；其他不可写或追加失败的有效帧累计拒收。
void CanLogger_AddFrame(const CanFrame *frame)
{
  if(logState == CAN_LOG_PAUSED) return;
  char row[96];  //一帧报文格式化成的 CSV 行，足够容纳最长的行
  int rowLength;  //格式化后的行长度
  int used;  //已使用的缓冲区字节数

  if(frame == NULL) return;  // 空指针不代表收到一帧，不计入拒收统计。
  if(logReady == 0U) {
    diagnostics.rejected_frames++;
    return;
  }

  // 64 位微秒值最多 20 位十进制数字，另留 1 字节字符串结束符。
  char timestamp[21];
  CanTime_FormatUnsigned(timestamp, frame->timestamp_us);
  rowLength = snprintf(row, sizeof(row), "%s,%s,%s,0x%lX,%u,",
                      timestamp,
                      (frame->ide == CAN_ID_STD) ? "STD" : "EXT",
                      (frame->rtr == CAN_RTR_DATA) ? "DATA" : "REMOTE",
                      (unsigned long)frame->id,
                      (unsigned int)frame->dlc);
  if ((rowLength < 0) || (rowLength >= (int)sizeof(row)))  //格式化失败或行长度超过缓冲区大小
  {
    diagnostics.rejected_frames++;
    CanLogger_StopWithError("format record", FR_INT_ERR);
    return;
  }

  used = rowLength;  //记录已使用的缓冲区字节数
  if (frame->rtr == CAN_RTR_DATA) {
    static const char hexDigits[] = "0123456789ABCDEF";
    uint8_t dataLength = (frame->dlc <= 8U) ? frame->dlc : 8U;
    // 数据区长度为 3N-1；提前检查完整空间，并为 CRLF 和字符串结束符保留位置。
    size_t dataBytes = dataLength ? (size_t)dataLength * 3U - 1U : 0U;
    if(dataBytes + 3U > sizeof(row) - (size_t)used) {
      diagnostics.rejected_frames++;
      CanLogger_StopWithError("format data", FR_INT_ERR);
      return;
    }
    // 固定两位大写十六进制，字节间保留单空格，不再逐字节调用格式化库。
    for(uint8_t i = 0U; i < dataLength; i++) {
      uint8_t value = frame->data[i];
      if(i != 0U) row[used++] = ' ';
      row[used++] = hexDigits[value >> 4];
      row[used++] = hexDigits[value & 0x0FU];
    }
    row[used] = '\0';
  }

  
  if ((used + 2) >= (int)sizeof(row))  //行结尾格式化失败或行长度超过缓冲区大小
  {
    diagnostics.rejected_frames++;
    CanLogger_StopWithError("format line ending", FR_INT_ERR);
    return;
  }
  row[used++] = '\r';
  row[used++] = '\n';

  // 在完整报文边界切分，文件大小计算包含尚未写出的 RAM 缓冲。
  if(logHasFrames && (uint32_t)f_size(&logFile) + logBufferLength + (UINT)used > CAN_LOG_MAX_FILE_SIZE) {
    CanLogger_ArchiveCurrent();
    if(logReady == 0U) {
      diagnostics.rejected_frames++;
      return;
    }
  }

  // 若剩余空间容纳不下这一整行，先写出已有内容，再将新行放入清空后的缓冲。
  if ((UINT)used > (CAN_LOG_BUFFER_SIZE - logBufferLength)) 
  {
    if (CanLogger_FlushBuffer() == 0U)  //缓冲区写满时立即写卡
    {
      diagnostics.rejected_frames++;
      return;
    }
  }

  memcpy(&logBuffer[logBufferLength], row, (size_t)used);  //将行放入缓冲区
  logBufferLength += (UINT)used;  //记录已使用的缓冲区字节数
  loggedFrameCount++;  // 当前活动文件接纳数，不表示已经完成同步。
  // accepted 仅说明已复制到 RAM，written 和 synced 在实际写入、同步后分别更新。
  diagnostics.accepted_frames++;
  diagnostics.buffered_frames++;
  logHasFrames = 1U;
}

// 主循环调用：推进自动/手动归档，按间隔写出缓冲并同步；暂停或不可写时不继续正常记录。
// 此处使用同步 FatFs 接口，单次调用仍可能阻塞，不能保证每次严格在固定时间内完成。
void CanLogger_Process(void)
{
  if(logState == CAN_LOG_PAUSED) return;
  FRESULT result;  

  if(logReady && logHasFrames && (uint32_t)f_size(&logFile) + logBufferLength >= CAN_LOG_MAX_FILE_SIZE) {
    CanLogger_ArchiveCurrent();
  }

  // 请求由界面提交，实际文件操作仅在主循环执行。
  // 至少延后 50 ms，给请求提示留出绘制机会；不保证界面一定完成刷新，期间仍正常记录。
  if(archiveState == CAN_ARCHIVE_PENDING && HAL_GetTick() - archiveRequestTick >= 50U) {
    CanLogger_ArchiveCurrent();
  }

  if (logReady == 0U)  //如果日志不可写，则直接返回
  {
    return;
  }

  // 缓冲区写满时立即写卡，避免覆盖尚未保存的报文。
  if (logBufferLength == CAN_LOG_BUFFER_SIZE)
  {
    if (CanLogger_FlushBuffer() == 0U)  //缓冲区写满时立即写卡
    {
      return;
    }
  }

  // 每秒写出尚未满缓冲区的记录并同步，降低突然断电时的丢失量。
  if ((HAL_GetTick() - lastSyncTick) >= CAN_LOG_SYNC_INTERVAL_MS)
  {
    if (CanLogger_FlushBuffer() == 0U)  
    {
      return;
    }

    result = CanLogger_SyncFile();  //同步文件
    if (result != FR_OK)  //同步失败
    {
      CanLogger_StopWithError("sync", result);  //停止写入并报告错误
      return;
    }

    lastSyncTick = HAL_GetTick();  //记录当前时间戳

    // 每个活动文件首次周期同步成功后提示一次，输出当时该文件累计接纳数。
    if ((loggedFrameCount > 0U) && (firstWriteSyncReported == 0U))
    {
      printf("SD LOG: first periodic sync OK, file_frames=%lu\r\n",
             (unsigned long)loggedFrameCount);
      firstWriteSyncReported = 1U;  //标记已报告
    }
  }
}

// 回放流程在主循环调用：paused 非零时先刷出并同步已有记录，成功后进入 PAUSED。
// paused 为零时尝试恢复 RECORDING；返回 1 表示日志可用，0 表示不可用或暂停失败。
// 暂停不会缓存期间的新接收数据，恢复也不会修复已经发生的日志错误。
uint8_t CanLogger_SetPaused(uint8_t paused)
{
  // 只有原状态确为 PAUSED 才恢复；ERROR 等状态不会在这里被清除。
  if(!paused) {
    if(logState == CAN_LOG_PAUSED) {
      logState = CAN_LOG_RECORDING;
      lastSyncTick = HAL_GetTick();
    }
    return logReady;
  }
  if(logState == CAN_LOG_PAUSED) return 1U;
  if(!logReady || archiveState == CAN_ARCHIVE_PENDING) return 0U;
  if(!CanLogger_FlushBuffer()) return 0U;
  FRESULT result = CanLogger_SyncFile();
  if(result != FR_OK) {
    CanLogger_StopWithError("pause sync", result);
    return 0U;
  }
  logState = CAN_LOG_PAUSED;
  return 1U;
}

// 主循环或界面查询：返回日志业务状态，不是物理插卡检测结果。
CanLoggerState CanLogger_GetState(void) { return logState; }
// 返回最近保存的 FatFs 日志错误码；不是 SDIO 寄存器位，也不是错误次数。
uint32_t CanLogger_GetLastError(void) { return (uint32_t)lastError; }
// 返回当前归档请求或最近归档的结果，不直接执行文件操作。
CanArchiveState CanLogger_GetArchiveState(void) { return archiveState; }
// 返回最近成功归档名的内部只读地址，没有成功归档时为空字符串；不能修改或释放。
const char *CanLogger_GetLastArchiveName(void) { return lastArchiveName; }
// 返回成功生成归档文件后递增的版本号，供历史列表发现变化，不等于归档总文件数。
uint32_t CanLogger_GetArchiveRevision(void) { return archiveRevision; }

// 主循环或界面查询：将累计计数复制到 snapshot，NULL 时不写入。
// 计数跨文件归档保留，按 uint32_t 自然回绕；不能将 accepted、written、synced 相加。
void CanLogger_GetDiagnostics(CanLoggerDiagnostics *snapshot)
{
  if(snapshot != NULL) *snapshot = diagnostics;
}
