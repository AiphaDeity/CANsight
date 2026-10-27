// 历史浏览与 CAN 回放模块，接口都在主循环或其 LVGL 回调中使用，不在中断中调用。
// 阅读入口：Request... 接收界面请求，LogPlayer_Process 推进浏览，LogPlayer_ReplayProcess 推进回放。
// 请求受理不等于操作完成；结果保存在 view / replay 中，由界面定期读取。
// 浏览：扫描目录 -> 选择文件 -> 校验 CSV 并建稀疏索引 -> 筛选与逐帧预览。
// 回放：暂停新帧记录 -> 分批预读日志 -> 按时间戳发送 -> 确认停止并恢复记录。
#include "log_player.h"
#include "can.h"
#include "can_timestamp.h"
#include "fatfs.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

// 浏览最多保存 64 个定位点；文件变大时扩大间隔，不无限增加内存。
#define LOG_INDEX_CAPACITY 64U
// 回放预读队列最多缓存 64 帧，与 main.c 的 CCM 接收队列独立。
#define REPLAY_CAPACITY 64U

// 索引保存行起点之前的完整解析状态，跳转后能恢复全局帧号和旧日志回绕上下文。
typedef struct {
    uint64_t previous_time;  // 前一帧展开后的微秒时间，用于恢复旧日志回绕上下文。
    uint32_t offset;         // 下一待处理字节的偏移，不一定等于 f_read 后的文件指针。
    uint32_t line_number;    // 已处理的物理行数，包含表头和空行。
    uint32_t frames;         // 已解析报文数，不包含表头和空行，不受筛选影响。
    uint32_t matches;        // 已解析且符合当前 ID 筛选的报文数。
} LogCursor;

// 读取流程：界面提交请求，主循环分批访问文件系统，结果通过 view 提供给界面。
static FIL reader;                          // 历史文件对象，与日志模块的写入对象独立。
static DIR directory;                       // 分批扫描期间持续保留的目录对象。
static uint32_t archive_revision;           // 上次观察到的归档版本，用于检测新增历史文件。

static LogPlayerView view;                  // 对外发布的文件列表、预览帧和运行状态。
static LogReplayView replay;                // 回放状态与发送计数，不是待发帧缓冲本身。
static char root_path[260];                 // 日志所在目录，不包含具体文件名。
static uint8_t root_valid;                  // 目录参数非空且长度未超过缓冲区。
static uint32_t close_error;                // 锁存关闭错误，阻止复用未确认释放的文件或目录对象。
static uint8_t active;                      // 历史页激活标志，关闭页面后停止读取。
static uint8_t query_pending;               // 等待主循环受理的目录扫描请求。
static uint8_t open_pending;                // 等待主循环受理的文件打开请求。
static uint8_t storage_pending;             // 仅在进入、刷新或文件变更后查询容量。
static uint8_t directory_open;              // 目录对象是否处于打开状态。
static uint8_t reader_open;                 // 历史文件是否处于打开状态。
static uint8_t header_seen;                 // 当前文件的 CSV 表头是否已通过校验。
static uint16_t line_length;                // 当前尚未解析的文本行长度。
static char line[128];                      // 跨读取批次保留的不完整行，预留字符串结束符。
static uint8_t chunk[1024];                 // 单次读取缓冲区，不随日志大小增长。
static int query_direction;                 // 0：首页；正数：下一页；负数：上一页。
static uint32_t query_anchor;               // 翻页的编号边界，不依赖连续文件编号。
static uint32_t requested_page;             // 本次查询成功后发布的页号，从 0 开始。
static LogHistoryFile candidates[LOG_PLAYER_PAGE_SIZE];  // 当前查询保留的最多四个候选条目。
static uint8_t candidate_count;             // 候选数组中的有效条目数。
static uint32_t scan_total;                 // 本次扫描发现的全部有效历史文件数。

static LogCursor cursor;                   // 当前浏览扫描的位置与解析状态。
static LogCursor checkpoints[LOG_INDEX_CAPACITY];  // RAM 中的稀疏定位点，不是 SD 卡上的索引文件。
static uint8_t checkpoint_count;           // 当前有效定位点数，不超过 LOG_INDEX_CAPACITY。
static uint32_t checkpoint_stride;         // 每隔多少个匹配帧尝试保存定位点，初始为 64。
static uint32_t line_offset;                // 正在拼接的文本行在文件中的绝对字节位置。
static uint8_t building_index;             // 1：扫描整文件建索引；0：利用已有索引定位预览窗口。
static uint8_t read_turn;                   // 目录与文件同时打开时交替推进，防止翻页长期等待。
static uint8_t release_pending;             // 快速退出再进入历史页时，也须释放上一轮读取资源。

// 以下为独立的回放状态；回放占用期间，普通浏览读取暂停。
static FIL replay_file;                    // 回放只读文件对象，由停止流程关闭。
// 依次表示：文件打开、到文件末尾、表头已校验、使用旧版 32 位日志时间。
static uint8_t replay_open, replay_eof, replay_header, replay_legacy;
// inflight：一帧已提交但结果未确认；pause_requested：等待在途帧结束后暂停。
static uint8_t replay_inflight, replay_pause_requested;
static uint8_t replay_have_previous;       // 是否已解析首帧；时间为 0 也合法，不能用时间值代替此标志。
// head / count：队首与帧数；bytes / pos：文本批次长度与解析位置；line_used：半行长度。
static uint16_t replay_head, replay_count, replay_bytes, replay_pos, replay_line_used;
static uint8_t replay_chunk[2048];          // 一次预读最多 2 KiB CSV 文本，未解析部分留到下一轮。
static char replay_line[128];              // 跨批次拼接一行，预留字符串结束符。
static CanFrame replay_frames[REPLAY_CAPACITY];  // 解析后的待发帧，位于普通 SRAM，仅主循环读写。
// 单位 us：上一帧日志时间、首帧日志时间、实际回放起点、实际进入暂停的时刻。
static uint64_t replay_previous, replay_first, replay_base, replay_pause_time;
// 单位 us：在途帧提交时刻、停止流程起始时刻，用于超时判断。
static uint64_t replay_submitted, replay_stop_time;
static uint32_t replay_mailbox;             // HAL 返回的邮箱位掩码（1、2 或 4），不是数组下标。
static LogReplayState replay_final;        // 收尾后发布的目标状态：完成、主动停止或错误。
static uint8_t replay_stop_initialized;    // 是否已发起停止，防止每轮重复提交取消请求。

// 内部调用：选择新文件或改变筛选后，清空旧预览、计数和定位点，进入全文件建索引状态。
// 只修改 RAM；打开文件和实际扫描由后续 Process 执行。
static void reset_browser(void)
{
    checkpoint_count = 0U;
    // 按匹配帧序号分布定位点；筛选变化后，旧定位点不能继续使用。
    checkpoint_stride = 64U;
    building_index = 1U;
    view.index_ready = view.frame_count = 0U;
    view.legacy_time = 0U;
    view.total_frames = view.matched_frames = view.first_match = view.scanned_bytes = 0U;
    view.preview_error = view.line_number = 0U;
    view.preview_state = LOG_PREVIEW_INDEXING;
}

// 主循环内部调用：尝试关闭目录；未打开或关闭成功返回 0，否则返回 FatFs 错误码。
// 先撤销打开标志；失败由 close_error 锁住后续访问，标志清零不代表关闭成功。
static uint32_t close_directory(void)
{
    if(!directory_open) return 0U;
    directory_open = 0U;
    uint32_t result = (uint32_t)f_closedir(&directory);
    if(result != 0U) close_error = result;
    return result;
}

// 主循环内部调用：尝试关闭浏览文件；未打开或关闭成功返回 0，否则返回 FatFs 错误码。
// 关闭失败会锁住后续文件访问，防止覆盖仍未确认释放的 FIL 对象。
static uint32_t close_reader(void)
{
    if(!reader_open) return 0U;
    reader_open = 0U;
    uint32_t result = (uint32_t)f_close(&reader);
    if(result != 0U) close_error = result;
    return result;
}

// 内部调用：校验规范归档名并返回编号，不合法时返回 0，比较时忽略大小写。
// 只接受 CANLOG_001.CSV 等三至九位规范编号，拒绝活动日志、路径和多余补零。
static uint32_t archive_number(const char *name)
{
    size_t length = strlen(name);
    if(length < 14U || length > 20U) return 0U;
    const char *prefix = "CANLOG_";
    for(unsigned i = 0; i < 7U; i++) {
        if(toupper((unsigned char)name[i]) != prefix[i]) return 0U;
    }
    uint32_t number = 0U;
    // 长度检查已将编号限制为三至九位，转换结果不会超出 uint32_t。
    for(size_t i = 7U; i < length - 4U; i++) {
        if(name[i] < '0' || name[i] > '9') return 0U;
        number = number * 10U + (uint32_t)(name[i] - '0');
    }
    if(number == 0U) return 0U;
    char canonical[CAN_LOG_ARCHIVE_NAME_SIZE];
    // 重新构造规范名称，校验扩展名及补零方式，避免同一编号对应多种命名。
    snprintf(canonical, sizeof(canonical), "CANLOG_%03lu.CSV", (unsigned long)number);
    if(strlen(canonical) != length) return 0U;
    for(size_t i = 0; i < length; i++) {
        if(toupper((unsigned char)name[i]) != canonical[i]) return 0U;
    }
    return number;
}

// main 启动时调用一次：复制根路径并初始化浏览、回放可见状态，不访问 SD 卡。
// 后续切页使用 SetActive / Request 接口，不用此函数重置仍在使用的文件或邮箱。
void LogPlayer_Init(const char *root)
{
    memset(&view, 0, sizeof(view));
    memset(&replay, 0, sizeof(replay));
    root_valid = root != NULL && root[0] != '\0' && strlen(root) < sizeof(root_path);
    snprintf(root_path, sizeof(root_path), "%s", root_valid ? root : "");
    close_error = 0U;
    view.revision = 1U;  // 使用非零初始版本，使界面可以识别初始化后的数据。
    active = query_pending = open_pending = storage_pending = 0U;
    checkpoint_count = building_index = read_turn = 0U;
    release_pending = 0U;
    archive_revision = CanLogger_GetArchiveRevision();
}

// 界面切页时调用：更新历史页激活状态，停用时取消待执行请求并请求停止回放。
// 这里只改请求与 RAM 状态，文件关闭和邮箱停止确认由主循环继续处理。
void LogPlayer_SetActive(uint8_t enabled)
{
    if(!enabled) LogPlayer_ReplayStop();
    active = enabled;
    if(!enabled) {
        release_pending = 1U;
        // 退出时立即撤销未完成窗口，避免快速切回后继续使用已取消的解析状态。
        if(view.preview_state == LOG_PREVIEW_LOADING || view.preview_state == LOG_PREVIEW_INDEXING) {
            view.preview_state = LOG_PREVIEW_IDLE;
            view.frame_count = view.index_ready = 0U;
            view.revision++;
        }
        query_pending = open_pending = storage_pending = 0U;
        // 尚未执行的删除随页面退出取消，已完成的操作不受影响。
        if(view.delete_state == LOG_DELETE_PENDING) view.delete_state = LOG_DELETE_IDLE;
    }
}

// 界面或内部调用：请求重新扫描列表首页并查询容量，保留选中文件及已有预览。
// 回放占用或删除待处理时不受理；关闭错误时发布错误，不继续建立文件操作。
void LogPlayer_RequestRefresh(void)
{
    if(LogPlayer_ReplayBusy()) return;
    if(view.delete_state == LOG_DELETE_PENDING) return;
    if(close_error != 0U) {
        view.list_state = LOG_LIST_ERROR;
        view.list_error = close_error;
        view.storage_state = LOG_STORAGE_ERROR;
        view.storage_error = close_error;
        view.revision++;
        return;
    }
    replay = (LogReplayView){0};
    active = 1U;
    query_direction = 0;
    query_anchor = requested_page = 0U;
    query_pending = 1U;
    storage_pending = 1U;
    view.storage_state = LOG_STORAGE_LOADING;
    view.list_state = LOG_LIST_SCANNING;
    view.revision++;
}

// 界面调用：请求文件列表翻页；direction < 0 为上一页，否则为下一页。
// 列表未就绪或到边界时忽略，只保存编号边界并置请求，不在回调中扫描目录。
void LogPlayer_RequestPage(int direction)
{
    if(LogPlayer_ReplayBusy()) return;
    if(view.delete_state == LOG_DELETE_PENDING) return;
    if(close_error != 0U) { LogPlayer_RequestRefresh(); return; }
    if(view.list_state != LOG_LIST_READY || view.file_count == 0U) return;
    if(direction < 0) {
        if(view.page == 0U) return;
        query_anchor = view.files[0].number;  // 上一页寻找比当前页最大编号更大的文件。
        requested_page = view.page - 1U;
        query_direction = -1;
    }
    else {
        if((view.page + 1U) * LOG_PLAYER_PAGE_SIZE >= view.total_files) return;
        query_anchor = view.files[view.file_count - 1U].number;  // 下一页只查询更小的编号。
        requested_page = view.page + 1U;
        query_direction = 1;
    }
    replay = (LogReplayView){0};
    query_pending = 1U;
    view.list_state = LOG_LIST_SCANNING;
    view.revision++;
}

// 界面选中文件时调用：复制规范文件名、清空旧预览，等待主循环打开和扫描。
// 不保存调用方字符串地址；回放、删除或关闭错误等条件下拒绝请求。
void LogPlayer_RequestOpen(const char *name)
{
    if(LogPlayer_ReplayBusy()) return;
    if(view.delete_state == LOG_DELETE_PENDING) return;
    if(name == NULL || archive_number(name) == 0U) return;
    replay = (LogReplayView){0};
    if(close_error != 0U) {
        view.preview_state = LOG_PREVIEW_IO_ERROR;
        view.preview_error = close_error;
        view.revision++;
        return;
    }
    active = 1U;
    snprintf(view.selected_name, sizeof(view.selected_name), "%s", name);
    view.delete_state = LOG_DELETE_IDLE;
    reset_browser();
    view.selected_size = 0U;
    open_pending = 1U;
    view.revision++;
}

// 界面调用：enabled 为 0 表示全部，非零表示按 ID 数值匹配。
// 返回 1 表示条件已受理（含条件未变），不是扫描完成；0 表示拒绝请求。
// 有选中文件时重新建索引，不修改原文件、实时接收统计或整文件回放范围。
uint8_t LogPlayer_SetFilter(uint32_t id, uint8_t enabled)
{
    if(LogPlayer_ReplayBusy()) return 0U;
    if(id > 0x1FFFFFFFU || close_error || view.delete_state == LOG_DELETE_PENDING) return 0U;
    enabled = enabled != 0U;
    if(view.filter_enabled == enabled && (!enabled || view.filter_id == id)) return 1U;
    replay = (LogReplayView){0};
    view.filter_enabled = enabled;
    view.filter_id = id;
    if(view.selected_name[0]) {
        reset_browser();
        open_pending = 1U;
    }
    view.revision++;
    return 1U;
}

// 界面调用：direction < 0 向前一条匹配帧，> 0 向后一条，0 不移动。
// 需要索引完成且有匹配结果；提交定位后最多显示五帧，不是每次翻五帧。
void LogPlayer_RequestStep(int direction)
{
    if(LogPlayer_ReplayBusy()) return;
    if(!active || close_error || !view.index_ready || view.preview_state != LOG_PREVIEW_READY ||
       view.delete_state == LOG_DELETE_PENDING || !view.matched_frames) return;
    if(direction < 0) {
        if(!view.first_match) return;
        view.first_match--;
    }
    else {
        if(direction == 0 || view.first_match + 1U >= view.matched_frames) return;
        view.first_match++;
    }
    replay = (LogReplayView){0};
    building_index = 0U;
    view.preview_state = LOG_PREVIEW_LOADING;
    view.frame_count = 0U;
    view.delete_state = LOG_DELETE_IDLE;
    open_pending = 1U;
    view.revision++;
}

// 界面取得用户确认后调用：锁定当前选中的规范文件名，提交删除请求。
// 返回 1 仅表示受理，0 表示拒绝；实际结果由 view.delete_state / delete_error 发布。
uint8_t LogPlayer_RequestDelete(const char *name)
{
    if(LogPlayer_ReplayBusy()) return 0U;
    if(!active || !root_valid || close_error || name == NULL ||
       archive_number(name) == 0U || strcmp(name, view.selected_name) != 0 ||
       view.delete_state == LOG_DELETE_PENDING) return 0U;
    replay = (LogReplayView){0};
    snprintf(view.delete_name, sizeof(view.delete_name), "%s", name);
    view.delete_error = 0U;
    view.delete_state = LOG_DELETE_PENDING;
    view.revision++;
    return 1U;
}


// 主循环内部调用：处理一次容量查询，结果保存在 view 的容量和错误字段中。
// f_getfree 是同步调用，可能扫描 FAT；失败时清空容量，不沿用旧数值冒充成功。
static void read_storage(void)
{
    storage_pending = 0U;
    uint32_t error = 0U;
    view.total_bytes = view.free_bytes = 0U;
    if(!root_valid) error = 6U;
    else {
        DWORD free_clusters;
        FATFS *filesystem;
        // 这里同步取得空闲簇数；没有可信缓存时，FatFs 可能完整扫描 FAT。
        error = (uint32_t)f_getfree(root_path, &free_clusters, &filesystem);
        if(error == 0U) {
            // 可变扇区配置必须读取卷的实际扇区大小，并在乘法前扩展至 64 位。
#if _MAX_SS != _MIN_SS
            uint32_t sector_size = filesystem->ssize;
#else
            uint32_t sector_size = _MIN_SS;
#endif
            uint64_t cluster_bytes = (uint64_t)filesystem->csize * sector_size;
            view.total_bytes = (uint64_t)(filesystem->n_fatent - 2U) * cluster_bytes;
            view.free_bytes = (uint64_t)free_clusters * cluster_bytes;
        }
    }
    view.storage_error = error;
    view.storage_state = error ? LOG_STORAGE_ERROR : LOG_STORAGE_READY;
    view.revision++;
}

// 主循环内部调用：取消读请求，尝试关闭浏览资源，再检查并删除已确认的文件。
// f_unlink 同步执行；发布结果后请求刷新实际目录和容量，已经成功的删除不能撤销。
static void delete_history(void)
{
    query_pending = open_pending = 0U;
    uint32_t error = close_reader();
    // 即使关闭 reader 失败，也尝试关闭目录；保留优先发生的关闭错误。
    uint32_t directory_error = close_directory();
    if(!error) error = directory_error;
    char path[320];
    snprintf(path, sizeof(path), "%s/%s", root_path, view.delete_name);
    if(!error) {
        FILINFO info;
        error = (uint32_t)f_stat(path, &info);
        // 同名目录不可删除，活动文件已由请求接口的名称检查排除。
        if(!error && (info.fattrib & AM_DIR)) error = (uint32_t)FR_DENIED;
        if(!error) error = (uint32_t)f_unlink(path);
    }
    view.delete_error = error;
    view.delete_state = error ? LOG_DELETE_ERROR : LOG_DELETE_DONE;
    if(!error) {
        view.selected_name[0] = '\0';
        view.selected_size = view.frame_count = view.line_number = view.preview_error = 0U;
        view.preview_state = LOG_PREVIEW_IDLE;
        view.index_ready = 0U;
        view.total_frames = view.matched_frames = 0U;
    }
    else if(view.preview_state == LOG_PREVIEW_LOADING || view.preview_state == LOG_PREVIEW_INDEXING) {
        view.preview_state = LOG_PREVIEW_IO_ERROR;
        view.preview_error = error;
    }
    // 删除失败保留选中项及已读取数据；重新扫描实际目录，避免展示已失效的条目。
    LogPlayer_RequestRefresh();
    view.revision++;
}

// 目录扫描内部调用：根据编号边界，用固定数组保留目标页最近的四个文件。
// 只复制候选信息，不打开文件；不要求编号连续，也不缓存整个目录。
static void retain_candidate(const LogHistoryFile *file)
{
    // 先排除翻页边界之外的条目，再使用插入方式维护固定容量候选数组。
    if(query_direction > 0 && file->number >= query_anchor) return;
    if(query_direction < 0 && file->number <= query_anchor) return;
    unsigned position = 0U;
    while(position < candidate_count && (query_direction < 0 ?
          candidates[position].number < file->number : candidates[position].number > file->number)) {
        position++;
    }
    if(position >= LOG_PLAYER_PAGE_SIZE) return;
    // 数组已满时只保留更接近目标页的条目，末尾被移出的条目直接舍弃。
    if(candidate_count < LOG_PLAYER_PAGE_SIZE) candidate_count++;
    for(unsigned i = candidate_count - 1U; i > position; i--) candidates[i] = candidates[i - 1U];
    candidates[position] = *file;
}

// 主循环内部调用：尝试关闭目录，撤销可见列表并发布扫描错误，不改变活动日志状态。
// 关闭失败另由 close_error 锁存；列表仍保留传入的本次错误码。
static void list_failed(uint32_t error)
{
    (void)close_directory();
    view.list_state = LOG_LIST_ERROR;
    view.list_error = error;
    view.file_count = 0U;
    view.revision++;
}

// 主循环受理目录请求：关闭旧目录、打开根目录并清空本轮候选数组。
// 遍历交给后续 scan_directory 分批执行，本次不扫描完整目录。
static void start_query(void)
{
    query_pending = 0U;
    if(!root_valid) { list_failed(6U); return; }
    uint32_t error = close_directory();
    if(error != 0U) { list_failed(error); return; }
    view.list_error = 0U;
    candidate_count = 0U;
    scan_total = 0U;
    FRESULT result = f_opendir(&directory, root_path);
    if(result != FR_OK) { list_failed((uint32_t)result); return; }
    directory_open = 1U;
}

// 主循环内部调用：每次最多处理四个目录项，下一轮从保留的位置继续。
// 忽略目录和非规范文件，到目录末尾才发布总数、页号和候选列表。
static void scan_directory(void)
{
    // 限制单轮目录项数量；完整扫描结束前不发布移动中的候选列表。
    for(unsigned step = 0; step < 4U; step++) {
        LogHistoryFile file = {0};
        uint8_t end = 0U;
        FILINFO info;
        FRESULT result = f_readdir(&directory, &info);
        if(result != FR_OK) { list_failed((uint32_t)result); return; }
        end = info.fname[0] == '\0';
        if(!end && !(info.fattrib & AM_DIR)) {
            file.number = archive_number(info.fname);
            if(file.number) {
                snprintf(file.name, sizeof(file.name), "%.*s", (int)sizeof(file.name) - 1, info.fname);
                file.size = (uint32_t)info.fsize;
            }
        }
        if(end) {
            uint32_t error = close_directory();
            if(error != 0U) { list_failed(error); return; }
            // 目录遍历结束后统一发布结果，避免扫描过程中按钮位置反复变化。
            view.total_files = scan_total;
            view.page = requested_page;
            view.file_count = candidate_count;
            for(unsigned i = 0; i < candidate_count; i++) {
                // 上一页查询保留的是升序候选，发布时反转，保证界面始终按编号降序显示。
                view.files[i] = candidates[query_direction < 0 ? candidate_count - 1U - i : i];
            }
            view.list_state = LOG_LIST_READY;
            view.revision++;
            return;
        }
        if(file.number) {
            scan_total++;
            retain_candidate(&file);
        }
    }
}

// 浏览流程统一结束出口：关闭文件并发布指定结果；名称虽为 failed，也用于正常结束。
// state 可为 READY、EMPTY 或错误；关闭错误优先，其他失败使原索引失效。
static void preview_failed(LogPreviewState state, uint32_t error)
{
    uint32_t close_error = close_reader();
    view.preview_state = close_error ? LOG_PREVIEW_IO_ERROR : state;
    view.preview_error = close_error ? close_error : error;
    if(view.preview_state != LOG_PREVIEW_READY && view.preview_state != LOG_PREVIEW_EMPTY) view.index_ready = 0U;
    view.revision++;
}

// 主循环受理文件或定位请求：关闭旧 reader，打开选中文件并准备解析状态。
// 建索引从文件开头扫描；已有索引时恢复目标附近定位点，读取目标预览窗口。
static void start_preview(void)
{
    open_pending = 0U;
    uint32_t error = close_reader();
    if(error != 0U) { preview_failed(LOG_PREVIEW_IO_ERROR, error); return; }
    char path[320];
    // 使用旧索引前核对大小；这项检查不能识别大小不变的外部内容改写。
    uint32_t expected_size = view.selected_size;
    snprintf(path, sizeof(path), "%s/%s", root_path, view.selected_name);
    FRESULT result = f_open(&reader, path, FA_READ);
    if(result != FR_OK) { preview_failed(LOG_PREVIEW_IO_ERROR, (uint32_t)result); return; }
    view.selected_size = (uint32_t)f_size(&reader);
    reader_open = 1U;
    if(!building_index && view.selected_size != expected_size) {
        preview_failed(LOG_PREVIEW_IO_ERROR, 9U);
        return;
    }
    memset(&cursor, 0, sizeof(cursor));
    // 建索引重新校验表头；定位浏览沿用已经确认的格式和时间模式。
    header_seen = building_index ? 0U : 1U;
    line_length = 0U;
    if(!building_index) {
        // 固定索引按匹配序号递增，选择不超过目标的最近起点。
        for(unsigned i = 0U; i < checkpoint_count; i++) {
            if(checkpoints[i].matches > view.first_match) break;
            cursor = checkpoints[i];
        }
        error = (uint32_t)f_lseek(&reader, cursor.offset);
        if(error) { preview_failed(LOG_PREVIEW_IO_ERROR, error); return; }
    }
    line_offset = cursor.offset;
    view.revision++;
}

// 内部工具：将非空十进制文本转成 uint64_t，成功返回 1 并写入 value。
// 符号、非数字或溢出返回 0，不把截断值当成合法时间。
static uint8_t parse_timestamp(const char *text, uint64_t *value)
{
    if(!*text) return 0U;
    uint64_t result = 0U;
    while(*text) {
        if(*text < '0' || *text > '9') return 0U;
        unsigned digit = (unsigned)(*text++ - '0');
        if(result > (UINT64_MAX - digit) / 10U) return 0U;
        result = result * 10U + digit;
    }
    *value = result;
    return 1U;
}

// 内部工具：按 base 将文本转成 uint32_t，当前使用十进制或十六进制。
// 成功返回 1 并写入 value；空串、非法字符或溢出返回 0，不自行识别 0x 前缀。
static uint8_t parse_number(const char *text, unsigned base, uint32_t *value)
{
    if(*text == '\0') return 0U;
    uint32_t number = 0U;
    while(*text != '\0') {
        unsigned digit;
        if(*text >= '0' && *text <= '9') digit = (unsigned)(*text - '0');
        else if(*text >= 'A' && *text <= 'F') digit = (unsigned)(*text - 'A') + 10U;
        else if(*text >= 'a' && *text <= 'f') digit = (unsigned)(*text - 'a') + 10U;
        else return 0U;
        // 先检查乘加是否溢出，再更新结果，避免溢出后得到看似合法的较小数值。
        if(digit >= base || number > (UINT32_MAX - digit) / base) return 0U;
        number = number * base + digit;
        text++;
    }
    *value = number;
    return 1U;
}

// 浏览和回放共用：原地拆开 CSV 的六个字段，将一行记录转换为 CanFrame。
// 成功返回 1；返回 0 后不得使用输出帧。text 会被修改，须提供可写行缓冲。
// 时间回绕和前后顺序由各自上下文处理，不在本函数判断。
static uint8_t parse_frame(char *text, CanFrame *frame)
{
    char *fields[6];
    fields[0] = text;
    // 将五个分隔逗号替换为字符串结束符，字段指针复用行缓冲，无需额外分配。
    for(unsigned i = 1; i < 6U; i++) {
        char *comma = strchr(fields[i - 1U], ',');
        if(comma == NULL) return 0U;
        *comma = '\0';
        fields[i] = comma + 1;
    }
    if(strchr(fields[5], ',') != NULL) return 0U;
    memset(frame, 0, sizeof(*frame));
    if(!parse_timestamp(fields[0], &frame->timestamp_us)) return 0U;
    // EXT=4、REMOTE=2 与 HAL 标志值一致；STD 和 DATA 沿用结构体零初始化的值。
    if(strcmp(fields[1], "EXT") == 0) frame->ide = 4U;
    else if(strcmp(fields[1], "STD") != 0) return 0U;
    if(strcmp(fields[2], "REMOTE") == 0) frame->rtr = 2U;
    else if(strcmp(fields[2], "DATA") != 0) return 0U;
    if(strlen(fields[3]) < 3U || fields[3][0] != '0' ||
       (fields[3][1] != 'x' && fields[3][1] != 'X') ||
       !parse_number(fields[3] + 2, 16U, &frame->id)) return 0U;
    if(frame->id > (frame->ide ? 0x1FFFFFFFU : 0x7FFU)) return 0U;  // 分别限制 29 位和 11 位 ID。
    uint32_t dlc;
    if(!parse_number(fields[4], 10U, &dlc) || dlc > 8U) return 0U;
    frame->dlc = (uint8_t)dlc;
    char *data = fields[5];
    if(frame->rtr) return *data == '\0';  // 远程帧保留 DLC，但数据字段必须为空。
    for(unsigned i = 0; i < dlc; i++) {
        while(*data == ' ') data++;
        if(data[0] == '\0' || data[1] == '\0') return 0U;
        char byte[3] = {data[0], data[1], '\0'};
        uint32_t value;
        if(!parse_number(byte, 16U, &value)) return 0U;
        frame->data[i] = (uint8_t)value;
        data += 2;
        if(*data != '\0' && *data != ' ') return 0U;  // 每字节必须恰好为两个十六进制字符。
    }
    while(*data == ' ') data++;
    return *data == '\0';
}

// 建索引内部调用：在匹配帧状态更新前，按间隔保存该行开始前的位置和计数。
// 满后保留第 0、2、4 等项并加倍间隔，降低定位密度，覆盖更长文件。
static void retain_checkpoint(void)
{
    if(cursor.matches % checkpoint_stride) return;
    if(checkpoint_count == LOG_INDEX_CAPACITY) {
        for(unsigned i = 0; i < LOG_INDEX_CAPACITY / 2U; i++) checkpoints[i] = checkpoints[i * 2U];
        checkpoint_count /= 2U;
        checkpoint_stride *= 2U;
        if(cursor.matches % checkpoint_stride) return;
    }
    // 时间和计数仍是当前帧之前的状态，跳回后能完整重读这一帧。
    LogCursor point = cursor;
    point.offset = line_offset;
    point.line_number--;  // 定位到当前帧解析前，重新读取时行号只增加一次。
    checkpoints[checkpoint_count++] = point;
}

// 浏览内部调用：处理一条完整行，包括表头、时间展开、筛选计数和预览。
// 成功返回 1 并清空行长度，格式或时间非法返回 0，由调用方结束读取并发布错误。
static uint8_t consume_line(void)
{
    cursor.line_number++;
    view.line_number = cursor.line_number;
    if(line_length && line[line_length - 1U] == '\r') line_length--;  // 同时兼容 CRLF 和 LF。
    line[line_length] = '\0';
    char *text = line;
    // 仅在文件第一行允许 UTF-8 BOM，跳过后仍需完整匹配表头。
    if(view.line_number == 1U && line_length >= 3U &&
       (uint8_t)text[0] == 0xEFU && (uint8_t)text[1] == 0xBBU && (uint8_t)text[2] == 0xBFU) text += 3;
    if(!header_seen) {
        if(strcmp(text, "timestamp_us,format,type,id,dlc,data_hex") == 0) view.legacy_time = 1U;
        else if(strcmp(text, "timestamp_us64,format,type,id,dlc,data_hex") == 0) view.legacy_time = 0U;
        else return 0U;
        header_seen = 1U;
    }
    else if(*text != '\0') {
        CanFrame frame;
        if(!parse_frame(text, &frame)) return 0U;
        if(view.legacy_time) {
            if(frame.timestamp_us > UINT32_MAX) return 0U;
            // 旧日志看到低 32 位下降就补一次 2^32，不能推断无报文期间的多次回绕。
            uint64_t base = cursor.previous_time & ~((uint64_t)UINT32_MAX);
            if(cursor.frames && frame.timestamp_us < (uint32_t)cursor.previous_time) {
                if(base > UINT64_MAX - (1ULL << 32)) return 0U;
                base += 1ULL << 32;
            }
            frame.timestamp_us += base;
        }
        else if(cursor.frames && frame.timestamp_us < cursor.previous_time) return 0U;
        // 所有帧先校验时间顺序，再决定是否匹配；筛选不能丢掉时间上下文。
        uint8_t matches = !view.filter_enabled || frame.id == view.filter_id;
        // 先存当前帧之前的定位状态，再覆盖上一帧时间和递增计数。
        if(building_index && matches) retain_checkpoint();
        cursor.previous_time = frame.timestamp_us;
        cursor.frames++;
        if(matches) {
            if(cursor.matches >= view.first_match && view.frame_count < LOG_PLAYER_PREVIEW_ROWS) {
                view.frame_numbers[view.frame_count] = cursor.frames;
                view.frames[view.frame_count++] = frame;
            }
            cursor.matches++;
        }
        if(building_index) {
            view.total_frames = cursor.frames;
            view.matched_frames = cursor.matches;
        }
    }
    line_length = 0U;  // 清空当前行长度，下一条记录从缓冲区起点继续拼接。
    return 1U;
}

// 浏览读到文件末尾后调用：检查表头并发布扫描结果，通过统一出口关闭文件。
// 全文件校验结束才置 index_ready；零字节、无报文和筛选无匹配按当前结果区分。
static void finish_scan(void)
{
    if(!header_seen) {
        preview_failed(cursor.offset ? LOG_PREVIEW_FORMAT_ERROR : LOG_PREVIEW_EMPTY, 0U);
        return;
    }
    if(building_index) {
        view.index_ready = 1U;
        view.scanned_bytes = view.selected_size;
    }
    preview_failed(view.frame_count ? LOG_PREVIEW_READY : LOG_PREVIEW_EMPTY, 0U);
}

// 主循环内部调用：每次最多读 1024 字节并逐行解析，跨批次半行留到下一轮。
// 建索引扫描整文件；已有索引时收集满五个匹配帧即可结束，不加载整份文件。
static void read_preview(void)
{
    unsigned length;
    UINT bytes;
    FRESULT result = f_read(&reader, chunk, sizeof(chunk), &bytes);
    if(result != FR_OK) { preview_failed(LOG_PREVIEW_IO_ERROR, (uint32_t)result); return; }
    length = bytes;
    // 历史文件应保持只读；读取期间大小变化时终止，防止继续使用失效索引。
    if(cursor.offset > view.selected_size || length > view.selected_size - cursor.offset ||
       (length == 0U && cursor.offset != view.selected_size)) {
        preview_failed(LOG_PREVIEW_IO_ERROR, 9U);
        return;
    }
    for(unsigned i = 0; i < length; i++) {
        cursor.offset++;
        if(chunk[i] == '\n') {
            // 换行表示当前行完整；解析成功后才检查是否达到五帧预览上限。
            if(!consume_line()) { preview_failed(LOG_PREVIEW_FORMAT_ERROR, 0U); return; }
            line_offset = cursor.offset;
            if(!building_index && view.frame_count == LOG_PLAYER_PREVIEW_ROWS) {
                preview_failed(LOG_PREVIEW_READY, 0U);
                return;
            }
        }
        else {
            if(chunk[i] == 0U || line_length >= sizeof(line) - 1U) {
                // 文本中出现 NUL 或行长度超过容量时，停止预览并报告当前物理行号。
                view.line_number = cursor.line_number + 1U;
                preview_failed(LOG_PREVIEW_FORMAT_ERROR, 0U);
                return;
            }
            line[line_length++] = (char)chunk[i];
        }
    }
    if(building_index) view.scanned_bytes = cursor.offset;
    view.revision++;
    if(length == 0U) {
        // 文件末尾可能没有换行符，剩余内容仍按最后一行校验，不能直接丢弃。
        if(line_length && !consume_line()) { preview_failed(LOG_PREVIEW_FORMAT_ERROR, 0U); return; }
        finish_scan();
    }
}

// 主循环按积压和间隔调用：按请求优先级推进目录、容量、删除、索引和预览。
// 回放占用时暂停浏览；分批不等于异步，单次 FatFs 调用仍可能阻塞主循环。
void LogPlayer_Process(void)
{
    // 1. 回放优先；之后检查关闭错误，阻止继续使用不确定的文件资源。
    if(LogPlayer_ReplayBusy()) return;
    if(close_error != 0U) {
        // 资源关闭失败后不继续覆盖文件对象，需复位重新初始化。
        query_pending = open_pending = storage_pending = 0U;
        if(view.storage_state == LOG_STORAGE_LOADING) {
            view.storage_state = LOG_STORAGE_ERROR;
            view.storage_error = close_error;
            view.revision++;
        }
        return;
    }
    // 2. 页面快速退出又进入时，也先释放上一轮浏览资源。
    if(release_pending) {
        release_pending = 0U;
        uint32_t error = close_reader();
        if(error) preview_failed(LOG_PREVIEW_IO_ERROR, error);
        error = close_directory();
        if(error) list_failed(error);
        if(close_error) return;
    }
    if(!active) {
        // 页面退出后释放资源；已完成的预览保留，未完成的预览取消。
        uint32_t error = close_reader();
        if(error) preview_failed(LOG_PREVIEW_IO_ERROR, error);
        if(view.preview_state == LOG_PREVIEW_LOADING || view.preview_state == LOG_PREVIEW_INDEXING) {
            view.preview_state = LOG_PREVIEW_IDLE;
            view.frame_count = 0U;
            view.index_ready = 0U;
            view.revision++;
        }
        error = close_directory();
        if(error) list_failed(error);
        return;
    }
    uint32_t revision = CanLogger_GetArchiveRevision();
    // 手动或自动归档生成新文件后，历史页刷新至首页；切页本身不触发归档。
    if(revision != archive_revision) {
        archive_revision = revision;
        LogPlayer_RequestRefresh();
    }
    if(CanLogger_GetState() == CAN_LOG_UNMOUNTED) {
        // 日志未挂载时拒绝文件访问，但不更改日志模块自身的错误状态。
        query_pending = open_pending = storage_pending = 0U;
        if(view.storage_state != LOG_STORAGE_ERROR || view.storage_error != 3U) {
            view.storage_state = LOG_STORAGE_ERROR;
            view.storage_error = 3U;
            view.revision++;
        }
        if(view.delete_state == LOG_DELETE_PENDING) {
            view.delete_state = LOG_DELETE_ERROR;
            view.delete_error = 3U;
            view.revision++;
        }
        if(view.list_state != LOG_LIST_ERROR) list_failed(3U);
        if(reader_open || view.preview_state == LOG_PREVIEW_LOADING ||
           view.preview_state == LOG_PREVIEW_INDEXING) preview_failed(LOG_PREVIEW_IO_ERROR, 3U);
        return;
    }
    // 3. 删除优先于扫描、打开和容量查询；本次处理后返回，其他工作留到下一轮。
    if(view.delete_state == LOG_DELETE_PENDING) { delete_history(); return; }
    // 新请求优先于旧任务；打开新文件时由 start_preview 统一结束旧读取。
    if(query_pending) { start_query(); return; }
    if(open_pending) { start_preview(); return; }
    // 容量请求只执行一次；目录扫描与文件读取交替推进，避免长文件阻塞文件列表翻页。
    if(storage_pending) { read_storage(); return; }
    if(reader_open && (!directory_open || read_turn)) {
        read_turn = 0U;
        read_preview();
    }
    else if(directory_open) {
        read_turn = 1U;
        scan_directory();
    }
}

// 历史页激活且无关闭错误时，有待处理请求或打开的浏览资源才返回非零。
// 不表示 SD 硬件忙，也不包含 CAN 回放占用；回放使用 ReplayBusy 查询。
uint8_t LogPlayer_IsBusy(void)
{
    return active && !close_error && (release_pending || query_pending || open_pending || directory_open || reader_open ||
        storage_pending || view.delete_state == LOG_DELETE_PENDING);
}

// 界面调用：返回内部浏览状态的只读地址，不能修改或释放。
// 后续主循环会更新内容；这不是独立分配的数据副本。
const LogPlayerView *LogPlayer_GetView(void) { return &view; }

// 界面或主循环调用：返回内部回放状态与发送计数的只读地址，不复制或分配数据。
const LogReplayView *LogPlayer_GetReplay(void) { return &replay; }

// 准备、运行、暂停、停止中或停止未确认时返回非零，表示回放仍占用资源。
// 暂停也不释放占用；正常完成收尾后才允许新回放或普通浏览。
uint8_t LogPlayer_ReplayBusy(void)
{
    return replay.state == LOG_REPLAY_PREPARING || replay.state == LOG_REPLAY_RUNNING ||
           replay.state == LOG_REPLAY_PAUSED || replay.state == LOG_REPLAY_STOPPING ||
           replay.state == LOG_REPLAY_STOP_FAILED;
}

// 主循环内部调用：读取在途邮箱结果，确认邮箱空闲后完成一次记账。
// 0：尚未结束；1：无在途帧或发送成功；2：已经结束但没有成功。
// cancelling 非零时允许将完成的取消记为 cancelled；本函数本身不发起取消。
static uint8_t replay_poll_tx(uint8_t cancelling)
{
    if(!replay_inflight) return 1U;
    // TME 表示邮箱空闲；未空闲时，不将“提交成功”提前计为“发送成功”。
    uint32_t flags = READ_REG(hcan1.Instance->TSR);
    if((flags & (replay_mailbox << CAN_TSR_TME0_Pos)) == 0U) return 0U;
    // 三个邮箱的完成、成功和错误标志每隔 8 位排列，用邮箱掩码选择对应组。
    uint32_t shift = replay_mailbox == CAN_TX_MAILBOX0 ? 0U :
                     replay_mailbox == CAN_TX_MAILBOX1 ? 8U : 16U;
    // 请求完成且 TXOK 置位才算成功；当前实现将仲裁失败也计入 failed。
    uint8_t success = (flags & (CAN_TSR_RQCP0 << shift)) && (flags & (CAN_TSR_TXOK0 << shift));
    if(success) replay.sent_frames++;
    else if(flags & ((CAN_TSR_TERR0 | CAN_TSR_ALST0) << shift)) replay.failed_frames++;
    else if(cancelling && (flags & (CAN_TSR_RQCP0 << shift))) replay.cancelled_frames++;
    else replay.unconfirmed_frames++;
    // RQCP 写 1 清除；撤销在途标志，避免重复记账同一次发送结果。
    WRITE_REG(hcan1.Instance->TSR, CAN_TSR_RQCP0 << shift);
    replay_inflight = replay.pending_frames = 0U;
    return success ? 1U : 2U;
}

// 内部调用：保存收尾后应发布的状态和错误码，进入 STOPPING，等待后续调用收尾。
// 不表示已经停止发送、关闭文件或恢复日志。
static void replay_finish(LogReplayState state, uint32_t error)
{
    // 统一交给停止状态收尾，先确认邮箱，再关闭文件和恢复记录。
    replay_final = state;
    replay.error = error;
    replay.state = LOG_REPLAY_STOPPING;
    replay_stop_initialized = 0U;
}

// 回放内部调用：解析一行并恢复时间上下文，将报文复制到待发队列，成功返回 1。
// 非法行返回 0；不应用浏览 ID 筛选，表头和空行不占槽位，调用方保证帧队列未满。
static uint8_t replay_consume_line(void)
{
    if(replay_line_used && replay_line[replay_line_used - 1U] == '\r') replay_line_used--;
    replay_line[replay_line_used] = '\0';
    char *text = replay_line;
    if(!replay_header) {
        if(replay_line_used >= 3U && (uint8_t)text[0] == 0xEFU &&
           (uint8_t)text[1] == 0xBBU && (uint8_t)text[2] == 0xBFU) text += 3;
        replay_legacy = strcmp(text, "timestamp_us,format,type,id,dlc,data_hex") == 0;
        if(!replay_legacy && strcmp(text, "timestamp_us64,format,type,id,dlc,data_hex") != 0) return 0U;
        replay_header = 1U;
    }
    else if(*text) {
        CanFrame frame;
        if(!parse_frame(text, &frame)) return 0U;
        if(replay_legacy) {
            if(frame.timestamp_us > UINT32_MAX) return 0U;
            // 按原文件顺序展开旧日志可观察到的 32 位回绕，与浏览规则一致。
            uint64_t base = replay_previous & ~((uint64_t)UINT32_MAX);
            if(replay_have_previous && frame.timestamp_us < (uint32_t)replay_previous) {
                if(base > UINT64_MAX - (1ULL << 32)) return 0U;
                base += 1ULL << 32;
            }
            frame.timestamp_us += base;
        }
        else if(replay_have_previous && frame.timestamp_us < replay_previous) return 0U;
        replay_previous = frame.timestamp_us;
        replay_have_previous = 1U;
        // 队首加有效数量得到队尾，复制整帧而不是保存局部变量地址。
        replay_frames[(replay_head + replay_count) % REPLAY_CAPACITY] = frame;
        replay_count++;
    }
    replay_line_used = 0U;
    return 1U;
}

// 回放内部调用：每次最多执行一次 2 KiB 读取，解析到文本用完或帧队列满。
// 满或已到文件末尾则返回；剩余文本和半行留到下一轮，错误转入停止收尾。
static void replay_fill(void)
{
    if(replay_eof || replay_count == REPLAY_CAPACITY) return;
    // 当前文本解析完才重新读卡，避免覆盖还没转换为帧的文本。
    if(replay_pos == replay_bytes) {
        UINT bytes = 0U;
        if(f_read(&replay_file, replay_chunk, sizeof(replay_chunk), &bytes) != FR_OK) {
            replay_finish(LOG_REPLAY_ERROR, 1U);
            return;
        }
        replay_pos = 0U;
        replay_bytes = (uint16_t)bytes;
        // 到文件末尾后，仍要处理最后一条没有换行符的记录。
        if(bytes == 0U) {
            replay_eof = 1U;
            if((replay_line_used && !replay_consume_line()) || !replay_header)
                replay_finish(LOG_REPLAY_ERROR, 2U);
            return;
        }
    }
    // 帧队列满时可中途退出，文本位置和半行数据留到下一轮接着解析。
    while(replay_pos < replay_bytes && replay_count < REPLAY_CAPACITY) {
        uint8_t c = replay_chunk[replay_pos++];
        if(c == '\n') {
            if(!replay_consume_line()) { replay_finish(LOG_REPLAY_ERROR, 2U); return; }
        }
        else {
            if(c == 0U || replay_line_used >= sizeof(replay_line) - 1U) {
                replay_finish(LOG_REPLAY_ERROR, 2U);
                return;
            }
            replay_line[replay_line_used++] = (char)c;
        }
    }
}

// 界面调用：索引完成、文件操作空闲且日志正常记录时，受理整文件回放。
// 1 表示进入 PREPARING，0 表示拒绝；这里尚未打开文件或提交 CAN 帧。
uint8_t LogPlayer_ReplayStart(void)
{
    if(LogPlayer_ReplayBusy() || !active || close_error || !view.index_ready ||
       !view.total_frames || LogPlayer_IsBusy() || archive_number(view.selected_name) == 0U ||
       CanLogger_GetState() != CAN_LOG_RECORDING ||
       CanLogger_GetArchiveState() == CAN_ARCHIVE_PENDING) return 0U;
    // 总帧数来自完整文件，不是筛选后的 matched_frames；重置本轮计数和准备状态。
    replay = (LogReplayView){ .state = LOG_REPLAY_PREPARING, .total_frames = view.total_frames };
    replay_open = replay_inflight = replay_pause_requested = 0U;
    replay_eof = replay_header = replay_have_previous = 0U;
    replay_head = replay_count = replay_bytes = replay_pos = replay_line_used = 0U;
    replay_previous = 0U;
    return 1U;
}

// 界面调用：运行中提交暂停请求，等在途帧结果确认后才进入 PAUSED。
// 已暂停时再次调用表示继续，后移起点扣除暂停时长；其他状态不处理。
void LogPlayer_ReplayPause(void)
{
    if(replay.state == LOG_REPLAY_RUNNING) replay_pause_requested = 1U;
    else if(replay.state == LOG_REPLAY_PAUSED) {
        // 将起点后移暂停时长，避免继续时把暂停期间的帧集中当作到期帧补发。
        replay_base += CanTimestamp_GetUs() - replay_pause_time;
        replay.state = LOG_REPLAY_RUNNING;
    }
}

// 界面或切页调用：请求主动结束回放，邮箱取消和文件关闭由 ReplayProcess 处理。
// 已经停止中或停止未确认时不重复初始化停止流程，不强制重启 CAN。
void LogPlayer_ReplayStop(void)
{
    // 文件关闭和邮箱取消由主循环执行，不在触摸事件中阻塞。
    if(LogPlayer_ReplayBusy() && replay.state != LOG_REPLAY_STOPPING &&
       replay.state != LOG_REPLAY_STOP_FAILED) replay_finish(LOG_REPLAY_STOPPED, 0U);
}

// 主循环高频调用：按状态推进准备、预读、发送、暂停或停止，不等待整份文件回放完。
// 只保留一个在途邮箱以保持顺序；提交不等于发送成功，回放及暂停期间不记录新接收帧。
// 其中的 SD 读取、同步和关闭仍是同步调用，可能影响下一次服务时刻。
void LogPlayer_ReplayProcess(void)
{
    // 1. 空闲或已完成收尾时，无需推进回放。
    if(!LogPlayer_ReplayBusy()) return;
    // 2. 停止优先：先确认邮箱，再关文件、请求恢复记录并发布最终状态。
    if(replay.state == LOG_REPLAY_STOPPING || replay.state == LOG_REPLAY_STOP_FAILED) {
        if(!replay_stop_initialized) {
            replay_stop_initialized = 1U;
            replay_stop_time = CanTimestamp_GetUs();
            // 已经发出的帧先记账；取消请求本身不等于硬件已经停止。
            if(replay_poll_tx(1U) == 0U &&
               HAL_CAN_AbortTxRequest(&hcan1, replay_mailbox) != HAL_OK) {
                replay.error = 6U;
                replay_final = LOG_REPLAY_ERROR;
                replay.state = LOG_REPLAY_STOP_FAILED;
            }
        }
        // 取消不等于已经停止；超时仍保持占用，后续调用继续检查是否终于结束。
        if(replay_poll_tx(1U) == 0U) {
            if(CanTimestamp_GetUs() - replay_stop_time > 1000000U) {
                replay.error = 6U;
                replay_final = LOG_REPLAY_ERROR;
                replay.state = LOG_REPLAY_STOP_FAILED;
            }
            return; // 未确认停止时保留占用状态及记录暂停，不启动新回放，也不重启 CAN。
        }
        if(replay.failed_frames) { replay.error = 3U; replay_final = LOG_REPLAY_ERROR; }
        if(replay.unconfirmed_frames) { replay.error = 6U; replay_final = LOG_REPLAY_ERROR; }
        if(replay_open) {
            FRESULT result = f_close(&replay_file);
            replay_open = 0U;
            if(result != FR_OK) {
                close_error = (uint32_t)result;
                replay.error = 1U;
                replay_final = LOG_REPLAY_ERROR;
            }
        }
        // 尝试恢复记录；该接口不会把已经故障的日志自动修复为正常。
        (void)CanLogger_SetPaused(0U);
        replay.state = replay_final;
        return;
    }
    // 3. 暂停时不读卡、不提交新帧；接收和统计继续，新接收数据不补存。
    if(replay.state == LOG_REPLAY_PAUSED) return;
    // 4. 准备阶段先刷出并同步现有日志、暂停新帧记录，再打开回放文件。
    if(replay.state == LOG_REPLAY_PREPARING && !replay_open) {
        if(!CanLogger_SetPaused(1U)) { replay_finish(LOG_REPLAY_ERROR, 5U); return; }
        // || 会短路：reader 关闭成功后才尝试关目录；任一失败就结束准备。
        if(close_reader() || close_directory()) { replay_finish(LOG_REPLAY_ERROR, 1U); return; }
        char path[320];
        snprintf(path, sizeof(path), "%s/%s", root_path, view.selected_name);
        if(f_open(&replay_file, path, FA_READ) != FR_OK) { replay_finish(LOG_REPLAY_ERROR, 1U); return; }
        replay_open = 1U;
        if(f_size(&replay_file) != view.selected_size) { replay_finish(LOG_REPLAY_ERROR, 1U); return; }
    }
    uint64_t now = CanTimestamp_GetUs();
    // 5. 一次只保留一帧在途，先确认结果再提交下一帧，保持文件中的顺序。
    if(replay_inflight) {
        uint8_t tx_result = replay_poll_tx(0U);
        if(tx_result == 0U) {
            if(now - replay_submitted > 1000000U) replay_finish(LOG_REPLAY_ERROR, 4U);
            // 等待硬件期间可补充预读，但同步读卡仍可能延迟下一次发送服务。
            else if(replay_count < REPLAY_CAPACITY / 2U && !replay_pause_requested) replay_fill();
            return;
        }
        if(tx_result != 1U) {
            replay_finish(LOG_REPLAY_ERROR, 3U);
            return;
        }
    }
    // 6. 到这里已无在途帧，此时才正式记录暂停时刻。
    if(replay_pause_requested) {
        replay_pause_requested = 0U;
        replay_pause_time = now;
        replay.state = LOG_REPLAY_PAUSED;
        return;
    }
    // 7. 先尽量预读满 64 帧或到文件末尾，再建立实际起点，避免把初次读卡算成延迟。
    if(replay.state == LOG_REPLAY_PREPARING) {
        replay_fill();
        if(replay.state == LOG_REPLAY_STOPPING) return;
        if(!replay_eof && replay_count < REPLAY_CAPACITY) return;
        if(!replay_count) { replay_finish(LOG_REPLAY_ERROR, 2U); return; }
        replay_first = replay_frames[replay_head].timestamp_us;
        replay_base = CanTimestamp_GetUs();
        replay.state = LOG_REPLAY_RUNNING;
    }
    // 8. 队列耗尽先补充；文件结束且无在途帧时，核对成功发送数再结束。
    if(!replay_count) {
        if(replay_eof) {
            replay_finish(replay.sent_frames == replay.total_frames ? LOG_REPLAY_DONE : LOG_REPLAY_ERROR,
                          replay.sent_frames == replay.total_frames ? 0U : 2U);
            return;
        }
        replay_fill();
        return;
    }
    CanFrame *frame = &replay_frames[replay_head];
    // 9. 文件首帧作为时间零点，不等待日志中的绝对启动时间；单位均为 us。
    uint64_t relative = frame->timestamp_us - replay_first;
    now = CanTimestamp_GetUs();
    if(now - replay_base >= relative) {
        CAN_TxHeaderTypeDef header = {0};
        header.IDE = frame->ide;
        header.RTR = frame->rtr;
        header.StdId = frame->ide ? 0U : frame->id;
        header.ExtId = frame->ide ? frame->id : 0U;
        header.DLC = frame->dlc;
        if(!HAL_CAN_GetTxMailboxesFreeLevel(&hcan1) ||
           HAL_CAN_AddTxMessage(&hcan1, &header, frame->data, &replay_mailbox) != HAL_OK) {
            replay_finish(LOG_REPLAY_ERROR, 3U);
            return;
        }
        // 这里统计本轮检查到的提交延迟，不包含后续总线仲裁和线上传输耗时。
        uint64_t late = now - replay_base - relative;
        uint32_t bounded = late > UINT32_MAX ? UINT32_MAX : (uint32_t)late;
        if(bounded > replay.max_late_us) replay.max_late_us = bounded;
        replay_submitted = now;
        replay_inflight = 1U;
        replay.pending_frames = 1U;
        replay.submitted_frames++;
        // 帧已复制到硬件邮箱，可以释放软件槽位；成功计数仍须等硬件确认。
        replay_head = (replay_head + 1U) % REPLAY_CAPACITY;
        replay_count--;
        return;
    }
    // 当前帧未到时，利用等待窗口补充队列；下一轮继续检查发送时刻。
    if(replay_count < REPLAY_CAPACITY / 2U) replay_fill();
}
