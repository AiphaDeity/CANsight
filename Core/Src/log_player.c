#include "log_player.h"
#include "can.h"
#include "can_timestamp.h"
#include "fatfs.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define LOG_INDEX_CAPACITY 64U
#define REPLAY_CAPACITY 64U

// 索引保存行起点之前的完整解析状态，跳转后能恢复全局帧号和旧日志回绕上下文。
typedef struct {
    uint64_t previous_time;
    uint32_t offset;
    uint32_t line_number;
    uint32_t frames;
    uint32_t matches;
} LogCursor;

// 读取流程：界面提交请求，主循环分批访问文件系统，结果通过 view 提供给界面。
static FIL reader;                          // 历史文件对象，与日志模块的写入对象独立。
static DIR directory;                       // 分批扫描期间持续保留的目录对象。
static uint32_t archive_revision;           // 上次观察到的归档版本，用于检测新增历史文件。

static LogPlayerView view;                  // 对外发布的文件列表、预览帧和运行状态。
static LogReplayView replay;
static char root_path[260];                 // 日志所在目录，不包含具体文件名。
static uint8_t root_valid;                  // 目录参数非空且长度未超过缓冲区。
static uint32_t close_error;  // 资源关闭失败后暂停文件访问，避免覆盖尚未释放的对象。
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

static LogCursor cursor;
static LogCursor checkpoints[LOG_INDEX_CAPACITY];
static uint8_t checkpoint_count;
static uint32_t checkpoint_stride;
static uint32_t line_offset;                // 正在拼接的文本行在文件中的绝对字节位置。
static uint8_t building_index;
static uint8_t read_turn;                   // 目录与文件同时打开时交替推进，防止翻页长期等待。
static uint8_t release_pending;             // 快速退出再进入历史页时，也须释放上一轮读取资源。

static FIL replay_file;
static uint8_t replay_open, replay_eof, replay_header, replay_legacy;
static uint8_t replay_inflight, replay_pause_requested;
static uint8_t replay_have_previous;
static uint16_t replay_head, replay_count, replay_bytes, replay_pos, replay_line_used;
static uint8_t replay_chunk[2048];
static char replay_line[128];
static CanFrame replay_frames[REPLAY_CAPACITY];
static uint64_t replay_previous, replay_first, replay_base, replay_pause_time;
static uint64_t replay_submitted, replay_stop_time;
static uint32_t replay_mailbox;
static LogReplayState replay_final;
static uint8_t replay_stop_initialized;

// 重建时立即清除旧结果，避免筛选条件与旧文件窗口混合显示。
static void reset_browser(void)
{
    checkpoint_count = 0U;
    checkpoint_stride = 64U;
    building_index = 1U;
    view.index_ready = view.frame_count = 0U;
    view.legacy_time = 0U;
    view.total_frames = view.matched_frames = view.first_match = view.scanned_bytes = 0U;
    view.preview_error = view.line_number = 0U;
    view.preview_state = LOG_PREVIEW_INDEXING;
}

// 关闭操作仅由主循环执行，保证切页和切文件时释放底层资源。
static uint32_t close_directory(void)
{
    if(!directory_open) return 0U;
    directory_open = 0U;
    uint32_t result = (uint32_t)f_closedir(&directory);
    if(result != 0U) close_error = result;
    return result;
}

// 结束历史文件访问；关闭失败时锁存错误，后续访问暂停至模块重新初始化。
static uint32_t close_reader(void)
{
    if(!reader_open) return 0U;
    reader_open = 0U;
    uint32_t result = (uint32_t)f_close(&reader);
    if(result != 0U) close_error = result;
    return result;
}

// 校验标准归档名称，拒绝路径分隔符、活动文件及非规范的重复编号名称。
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

/**
 * @brief 在启动阶段初始化目录参数及可见数据。
 * @note 不打开文件，不扫描目录；页面进入后由请求接口激活读取流程。
 */
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

// 更新页面激活状态；停用时取消待处理请求，资源关闭交由主循环执行。
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

// 请求重新扫描首页，不改变当前选中文件及已经完成的预览内容。
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

/**
 * @brief 请求相邻的历史文件列表页。
 * @param direction 负数表示上一页，非负数表示下一页。
 * @note 列表未就绪或已到边界时忽略请求，不执行文件系统操作。
 */
void LogPlayer_RequestPage(int direction)
{
    if(LogPlayer_ReplayBusy()) return;
    if(view.delete_state == LOG_DELETE_PENDING) return;
    if(close_error != 0U) { LogPlayer_RequestRefresh(); return; }
    if(view.list_state != LOG_LIST_READY || view.file_count == 0U) return;
    if(direction < 0) {
        if(view.page == 0U) return;
        query_anchor = view.files[0].number;  // 上一页应位于当前页最大编号之前。
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

// 校验并复制文件名，立即清除旧预览，实际关闭和打开操作延后至主循环。
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

// 筛选只重建只读索引，不修改接收统计、日志内容或归档规则。
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

// 每次移动窗口首帧一条记录；读入最多五个匹配帧，首行为当前帧。
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

// 请求中锁定已确认的历史文件名；不允许活动日志、任意路径或重复提交。
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


// 容量查询独立于目录、预览和写入状态；失败时不使用上次结果冒充当前容量。
static void read_storage(void)
{
    storage_pending = 0U;
    uint32_t error = 0U;
    view.total_bytes = view.free_bytes = 0U;
    if(!root_valid) error = 6U;
    else {
        DWORD free_clusters;
        FATFS *filesystem;
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

// 删除仅在主循环执行；先关闭扫描和读取资源，再检查目标属性并删除。
static void delete_history(void)
{
    query_pending = open_pending = 0U;
    uint32_t error = close_reader();
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

// 每次查询仅保留四个最优候选：下一页取较小编号中的最大四项，上一页反向取最近四项。
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

// 终止目录扫描并发布错误，不修改活动日志状态或已读取的报文。
static void list_failed(uint32_t error)
{
    (void)close_directory();
    view.list_state = LOG_LIST_ERROR;
    view.list_error = error;
    view.file_count = 0U;
    view.revision++;
}

// 主循环受理扫描请求：关闭旧目录，清空候选结果，再建立新的目录搜索。
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

// 扫描批次之间保留目录位置和候选数组，每轮最多处理四个目录项。
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

// 统一结束预览，成功、空文件和错误均通过此处关闭文件并发布最终状态。
// state 可传入 READY、EMPTY 或错误状态；资源关闭失败时优先报告关闭错误。
static void preview_failed(LogPreviewState state, uint32_t error)
{
    uint32_t close_error = close_reader();
    view.preview_state = close_error ? LOG_PREVIEW_IO_ERROR : state;
    view.preview_error = close_error ? close_error : error;
    if(view.preview_state != LOG_PREVIEW_READY && view.preview_state != LOG_PREVIEW_EMPTY) view.index_ready = 0U;
    view.revision++;
}

// 主循环受理文件选择请求，先关闭旧文件，再以只读模式打开新文件。
static void start_preview(void)
{
    open_pending = 0U;
    uint32_t error = close_reader();
    if(error != 0U) { preview_failed(LOG_PREVIEW_IO_ERROR, error); return; }
    char path[320];
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

// 十进制时间单独使用 64 位转换，拒绝负数、尾随字符及整数溢出。
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

// 严格解析无符号整数，拒绝符号、尾随字符及 32 位溢出。
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

/**
 * @brief 将一行 CSV 原地拆分并转换为 CanFrame。
 * @note 仅支持项目规定的六字段格式；转换失败时调用方不得使用该帧。
 */
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
    // IDE=4、RTR=2 与 STM32 HAL 报文标志保持一致，解析模块无需依赖 HAL。
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

// 索引满时保留偶数项并加倍间隔，使任何大小的文件最多使用 64 个定位点。
static void retain_checkpoint(void)
{
    if(cursor.matches % checkpoint_stride) return;
    if(checkpoint_count == LOG_INDEX_CAPACITY) {
        for(unsigned i = 0; i < LOG_INDEX_CAPACITY / 2U; i++) checkpoints[i] = checkpoints[i * 2U];
        checkpoint_count /= 2U;
        checkpoint_stride *= 2U;
        if(cursor.matches % checkpoint_stride) return;
    }
    LogCursor point = cursor;
    point.offset = line_offset;
    point.line_number--;  // 定位到当前帧解析前，重新读取时行号只增加一次。
    checkpoints[checkpoint_count++] = point;
}

// 先按完整文件顺序处理时间，再应用 ID 筛选，防止被过滤报文影响时间展开。
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
            uint64_t base = cursor.previous_time & ~((uint64_t)UINT32_MAX);
            if(cursor.frames && frame.timestamp_us < (uint32_t)cursor.previous_time) {
                if(base > UINT64_MAX - (1ULL << 32)) return 0U;
                base += 1ULL << 32;
            }
            frame.timestamp_us += base;
        }
        else if(cursor.frames && frame.timestamp_us < cursor.previous_time) return 0U;
        uint8_t matches = !view.filter_enabled || frame.id == view.filter_id;
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

// 完整扫描结束后发布准确计数；无匹配与空文件在界面分别说明。
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

// 单轮最多读取 1024 字节，跨批次的不完整行保留在 line 中，不加载整份文件。
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

/**
 * @brief 在日志写入之后推进目录扫描或文件预览。
 * @note 目录和预览按有限批次执行；容量查询及删除使用同步接口，可能短暂阻塞主循环。
 */
void LogPlayer_Process(void)
{
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

// 查询历史浏览是否仍有待处理的目录、文件或索引操作。
uint8_t LogPlayer_IsBusy(void)
{
    return active && !close_error && (release_pending || query_pending || open_pending || directory_open || reader_open ||
        storage_pending || view.delete_state == LOG_DELETE_PENDING);
}

// 返回内部数据的只读视图，界面不得修改或释放，后续 Process 可能更新其内容。
const LogPlayerView *LogPlayer_GetView(void) { return &view; }

const LogReplayView *LogPlayer_GetReplay(void) { return &replay; }

uint8_t LogPlayer_ReplayBusy(void)
{
    return replay.state == LOG_REPLAY_PREPARING || replay.state == LOG_REPLAY_RUNNING ||
           replay.state == LOG_REPLAY_PAUSED || replay.state == LOG_REPLAY_STOPPING ||
           replay.state == LOG_REPLAY_STOP_FAILED;
}

// 直接检查硬件邮箱，避免 HAL 状态异常时 IsTxMessagePending 返回 0 被误认为发送结束。
static uint8_t replay_poll_tx(uint8_t cancelling)
{
    if(!replay_inflight) return 1U;
    uint32_t flags = READ_REG(hcan1.Instance->TSR);
    if((flags & (replay_mailbox << CAN_TSR_TME0_Pos)) == 0U) return 0U;
    uint32_t shift = replay_mailbox == CAN_TX_MAILBOX0 ? 0U :
                     replay_mailbox == CAN_TX_MAILBOX1 ? 8U : 16U;
    uint8_t success = (flags & (CAN_TSR_RQCP0 << shift)) && (flags & (CAN_TSR_TXOK0 << shift));
    if(success) replay.sent_frames++;
    else if(flags & ((CAN_TSR_TERR0 | CAN_TSR_ALST0) << shift)) replay.failed_frames++;
    else if(cancelling && (flags & (CAN_TSR_RQCP0 << shift))) replay.cancelled_frames++;
    else replay.unconfirmed_frames++;
    WRITE_REG(hcan1.Instance->TSR, CAN_TSR_RQCP0 << shift);
    replay_inflight = replay.pending_frames = 0U;
    return success ? 1U : 2U;
}

static void replay_finish(LogReplayState state, uint32_t error)
{
    // 统一交给停止状态收尾，先确认邮箱，再关闭文件和恢复记录。
    replay_final = state;
    replay.error = error;
    replay.state = LOG_REPLAY_STOPPING;
    replay_stop_initialized = 0U;
}

// 解析与历史浏览共用严格的 CSV 转换，回放始终包含文件内全部报文。
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
        replay_frames[(replay_head + replay_count) % REPLAY_CAPACITY] = frame;
        replay_count++;
    }
    replay_line_used = 0U;
    return 1U;
}

// 每轮最多读一个 2 KiB 批次；固定预读队列避免每发一帧都访问 SD。
static void replay_fill(void)
{
    if(replay_eof || replay_count == REPLAY_CAPACITY) return;
    if(replay_pos == replay_bytes) {
        UINT bytes = 0U;
        if(f_read(&replay_file, replay_chunk, sizeof(replay_chunk), &bytes) != FR_OK) {
            replay_finish(LOG_REPLAY_ERROR, 1U);
            return;
        }
        replay_pos = 0U;
        replay_bytes = (uint16_t)bytes;
        if(bytes == 0U) {
            replay_eof = 1U;
            if((replay_line_used && !replay_consume_line()) || !replay_header)
                replay_finish(LOG_REPLAY_ERROR, 2U);
            return;
        }
    }
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

uint8_t LogPlayer_ReplayStart(void)
{
    if(LogPlayer_ReplayBusy() || !active || close_error || !view.index_ready ||
       !view.total_frames || LogPlayer_IsBusy() || archive_number(view.selected_name) == 0U ||
       CanLogger_GetState() != CAN_LOG_RECORDING ||
       CanLogger_GetArchiveState() == CAN_ARCHIVE_PENDING) return 0U;
    replay = (LogReplayView){ .state = LOG_REPLAY_PREPARING, .total_frames = view.total_frames };
    replay_open = replay_inflight = replay_pause_requested = 0U;
    replay_eof = replay_header = replay_have_previous = 0U;
    replay_head = replay_count = replay_bytes = replay_pos = replay_line_used = 0U;
    replay_previous = 0U;
    return 1U;
}

void LogPlayer_ReplayPause(void)
{
    if(replay.state == LOG_REPLAY_RUNNING) replay_pause_requested = 1U;
    else if(replay.state == LOG_REPLAY_PAUSED) {
        replay_base += CanTimestamp_GetUs() - replay_pause_time;
        replay.state = LOG_REPLAY_RUNNING;
    }
}

void LogPlayer_ReplayStop(void)
{
    // 文件关闭和邮箱取消由主循环执行，不在触摸事件中阻塞。
    if(LogPlayer_ReplayBusy() && replay.state != LOG_REPLAY_STOPPING &&
       replay.state != LOG_REPLAY_STOP_FAILED) replay_finish(LOG_REPLAY_STOPPED, 0U);
}

void LogPlayer_ReplayProcess(void)
{
    if(!LogPlayer_ReplayBusy()) return;
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
        (void)CanLogger_SetPaused(0U);
        replay.state = replay_final;
        return;
    }
    if(replay.state == LOG_REPLAY_PAUSED) return;
    if(replay.state == LOG_REPLAY_PREPARING && !replay_open) {
        if(!CanLogger_SetPaused(1U)) { replay_finish(LOG_REPLAY_ERROR, 5U); return; }
        if(close_reader() || close_directory()) { replay_finish(LOG_REPLAY_ERROR, 1U); return; }
        char path[320];
        snprintf(path, sizeof(path), "%s/%s", root_path, view.selected_name);
        if(f_open(&replay_file, path, FA_READ) != FR_OK) { replay_finish(LOG_REPLAY_ERROR, 1U); return; }
        replay_open = 1U;
        if(f_size(&replay_file) != view.selected_size) { replay_finish(LOG_REPLAY_ERROR, 1U); return; }
    }
    uint64_t now = CanTimestamp_GetUs();
    if(replay_inflight) {
        uint8_t tx_result = replay_poll_tx(0U);
        if(tx_result == 0U) {
            if(now - replay_submitted > 1000000U) replay_finish(LOG_REPLAY_ERROR, 4U);
            else if(replay_count < REPLAY_CAPACITY / 2U && !replay_pause_requested) replay_fill();
            return;
        }
        if(tx_result != 1U) {
            replay_finish(LOG_REPLAY_ERROR, 3U);
            return;
        }
    }
    if(replay_pause_requested) {
        replay_pause_requested = 0U;
        replay_pause_time = now;
        replay.state = LOG_REPLAY_PAUSED;
        return;
    }
    if(replay.state == LOG_REPLAY_PREPARING) {
        replay_fill();
        if(replay.state == LOG_REPLAY_STOPPING) return;
        if(!replay_eof && replay_count < REPLAY_CAPACITY) return;
        if(!replay_count) { replay_finish(LOG_REPLAY_ERROR, 2U); return; }
        replay_first = replay_frames[replay_head].timestamp_us;
        replay_base = CanTimestamp_GetUs();
        replay.state = LOG_REPLAY_RUNNING;
    }
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
        uint64_t late = now - replay_base - relative;
        uint32_t bounded = late > UINT32_MAX ? UINT32_MAX : (uint32_t)late;
        if(bounded > replay.max_late_us) replay.max_late_us = bounded;
        replay_submitted = now;
        replay_inflight = 1U;
        replay.pending_frames = 1U;
        replay.submitted_frames++;
        replay_head = (replay_head + 1U) % REPLAY_CAPACITY;
        replay_count--;
        return;
    }
    if(replay_count < REPLAY_CAPACITY / 2U) replay_fill();
}
