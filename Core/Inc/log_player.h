#ifndef LOG_PLAYER_H
#define LOG_PLAYER_H

#include "can_logger.h"

#define LOG_PLAYER_PAGE_SIZE 4U
#define LOG_PLAYER_PREVIEW_ROWS 5U

typedef enum {
    LOG_LIST_IDLE = 0,
    LOG_LIST_SCANNING,
    LOG_LIST_READY,
    LOG_LIST_ERROR
} LogListState;

typedef enum {
    LOG_PREVIEW_IDLE = 0,
    LOG_PREVIEW_INDEXING,
    LOG_PREVIEW_LOADING,
    LOG_PREVIEW_READY,
    LOG_PREVIEW_EMPTY,
    LOG_PREVIEW_FORMAT_ERROR,
    LOG_PREVIEW_IO_ERROR
} LogPreviewState;

typedef enum {
    LOG_STORAGE_IDLE = 0,
    LOG_STORAGE_LOADING,
    LOG_STORAGE_READY,
    LOG_STORAGE_ERROR
} LogStorageState;

typedef enum {
    LOG_DELETE_IDLE = 0,
    LOG_DELETE_PENDING,
    LOG_DELETE_DONE,
    LOG_DELETE_ERROR
} LogDeleteState;

// 文件名仅接受本项目生成的归档命名格式，禁止选择活动日志或任意路径。
typedef struct {
    char name[CAN_LOG_ARCHIVE_NAME_SIZE];
    uint32_t size;
    uint32_t number;
} LogHistoryFile;

// 只保存当前文件列表页及最多五帧的浏览窗口，内存占用不随日志大小增长。
typedef struct {
    uint32_t revision;          // 可见数据版本，供界面跳过无变化的刷新。
    LogListState list_state;
    LogPreviewState preview_state;
    uint32_t list_error;
    uint32_t preview_error;
    uint32_t total_files;
    uint32_t page;
    uint32_t line_number;       // 最近解析的物理行号，格式错误时用于定位。
    uint32_t selected_size;
    char selected_name[CAN_LOG_ARCHIVE_NAME_SIZE];
    LogHistoryFile files[LOG_PLAYER_PAGE_SIZE];
    uint8_t file_count;
    CanFrame frames[LOG_PLAYER_PREVIEW_ROWS];
    uint8_t frame_count;
    uint8_t index_ready;        // 完整扫描并校验文件后才允许逐帧导航。
    uint8_t legacy_time;        // 旧版 32 位时间日志，仅可按可观察的下降展开回绕。
    uint8_t filter_enabled;
    uint32_t filter_id;
    uint32_t total_frames;      // 全文件报文数量，不受 ID 筛选影响。
    uint32_t matched_frames;
    uint32_t first_match;       // 当前窗口首帧在匹配结果中的位置，从 0 开始。
    uint32_t frame_numbers[LOG_PLAYER_PREVIEW_ROWS];  // 每行在原文件中的报文序号，从 1 开始。
    uint32_t scanned_bytes;
    LogStorageState storage_state;
    uint32_t storage_error;
    uint64_t total_bytes;       // 文件系统可用数据区总容量，不是存储介质标称容量。
    uint64_t free_bytes;        // 最近一次容量查询结果，不包含尚未写入的 RAM 缓冲。
    LogDeleteState delete_state;
    uint32_t delete_error;
    char delete_name[CAN_LOG_ARCHIVE_NAME_SIZE];  // 确认时锁定的目标，不随选中文件变化。
} LogPlayerView;

/**
 * @brief 初始化历史文件读取模块。
 * @param root 固件传入 SDPath，模拟器传入本机日志目录。
 * @note 所有接口仅在主循环及其 LVGL 回调中调用，不支持中断并发访问。
 *       初始化仅在启动阶段执行一次；页面切换使用 SetActive 和请求接口。
 */
void LogPlayer_Init(const char *root);
// 以下请求接口不访问文件系统；实际操作由 Process 分批执行。
void LogPlayer_RequestRefresh(void);
void LogPlayer_RequestPage(int direction);
void LogPlayer_RequestOpen(const char *name);
// ID 按数值精确匹配，同时包含同 ID 的标准/扩展及数据/远程帧。
uint8_t LogPlayer_SetFilter(uint32_t id, uint8_t enabled);
// 负数向前移动一帧，正数向后移动一帧；边界或扫描期间忽略请求。
void LogPlayer_RequestStep(int direction);
// 仅受理当前选中的规范历史文件；调用方必须先取得用户的删除确认。
uint8_t LogPlayer_RequestDelete(const char *name);
void LogPlayer_SetActive(uint8_t active);
void LogPlayer_Process(void);
uint8_t LogPlayer_IsBusy(void);
const LogPlayerView *LogPlayer_GetView(void);

typedef enum {
    LOG_REPLAY_IDLE = 0, LOG_REPLAY_PREPARING, LOG_REPLAY_RUNNING,
    LOG_REPLAY_PAUSED, LOG_REPLAY_STOPPING, LOG_REPLAY_DONE, LOG_REPLAY_ERROR
} LogReplayState;

typedef struct {
    LogReplayState state;
    uint32_t sent_frames;
    uint32_t total_frames;
    uint32_t max_late_us;
    uint32_t error; // 1：文件访问，2：CSV，3：CAN 发送，4：等待超时，5：记录同步。
} LogReplayView;

// 请求仅修改状态；主循环高频调用 ReplayProcess 完成读卡与发送。
uint8_t LogPlayer_ReplayStart(void);
void LogPlayer_ReplayPause(void);
void LogPlayer_ReplayStop(void);
void LogPlayer_ReplayProcess(void);
uint8_t LogPlayer_ReplayBusy(void);
const LogReplayView *LogPlayer_GetReplay(void);

#endif
