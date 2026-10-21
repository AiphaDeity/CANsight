#include "log_player.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

// 读取流程：界面提交请求，主循环分批访问文件系统，结果通过 view 提供给界面。
// 两个平台共用目录分页和 CSV 解析，仅文件、目录访问接口不同。
#ifdef CAN_PLAYER_SIMULATOR
#include <io.h>
#include <errno.h>
static FILE *reader;                         // 模拟器当前打开的只读文件。
static intptr_t directory = -1;             // Windows 目录搜索句柄，-1 表示无效。
static struct _finddata_t directory_entry;   // 最近一次目录搜索得到的条目。
static uint8_t directory_first;             // 首个条目已由 _findfirst 返回，无需立即调用 _findnext。
#else
#include "fatfs.h"
static FIL reader;                          // 历史文件对象，与日志模块的写入对象独立。
static DIR directory;                       // 分批扫描期间持续保留的目录对象。
static uint32_t archive_revision;           // 上次观察到的归档版本，用于检测新增历史文件。
#endif

static LogPlayerView view;                  // 对外发布的文件列表、预览帧和运行状态。
static char root_path[260];                 // 日志所在目录，不包含具体文件名。
static uint8_t root_valid;                  // 目录参数非空且长度未超过缓冲区。
static uint32_t close_error;  // 资源关闭失败后暂停文件访问，避免覆盖尚未释放的对象。
static uint8_t active;                      // 历史页激活标志，关闭页面后停止读取。
static uint8_t query_pending;               // 等待主循环受理的目录扫描请求。
static uint8_t open_pending;                // 等待主循环受理的文件打开请求。
static uint8_t directory_open;              // 目录对象是否处于打开状态。
static uint8_t reader_open;                 // 历史文件是否处于打开状态。
static uint8_t header_seen;                 // 当前文件的 CSV 表头是否已通过校验。
static uint16_t line_length;                // 当前尚未解析的文本行长度。
static char line[128];                      // 跨读取批次保留的不完整行，预留字符串结束符。
static uint8_t chunk[256];                  // 单次读取缓冲区，不随日志大小增长。
static int query_direction;                 // 0：首页；正数：下一页；负数：上一页。
static uint32_t query_anchor;               // 翻页的编号边界，不依赖连续文件编号。
static uint32_t requested_page;             // 本次查询成功后发布的页号，从 0 开始。
static LogHistoryFile candidates[LOG_PLAYER_PAGE_SIZE];  // 当前查询保留的最多四个候选条目。
static uint8_t candidate_count;             // 候选数组中的有效条目数。
static uint32_t scan_total;                 // 本次扫描发现的全部有效历史文件数。

// 关闭操作仅由主循环执行，保证切页和切文件时释放底层资源。
static uint32_t close_directory(void)
{
    if(!directory_open) return 0U;
    directory_open = 0U;
#ifdef CAN_PLAYER_SIMULATOR
    int result = _findclose(directory);
    directory = -1;
    if(result != 0) close_error = 1U;
    return result == 0 ? 0U : 1U;
#else
    uint32_t result = (uint32_t)f_closedir(&directory);
    if(result != 0U) close_error = result;
    return result;
#endif
}

// 结束历史文件访问；关闭失败时锁存错误，后续访问暂停至模块重新初始化。
static uint32_t close_reader(void)
{
    if(!reader_open) return 0U;
    reader_open = 0U;
#ifdef CAN_PLAYER_SIMULATOR
    int result = fclose(reader);
    reader = NULL;
    if(result != 0) close_error = 1U;
    return result == 0 ? 0U : 1U;
#else
    uint32_t result = (uint32_t)f_close(&reader);
    if(result != 0U) close_error = result;
    return result;
#endif
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
    root_valid = root != NULL && root[0] != '\0' && strlen(root) < sizeof(root_path);
    snprintf(root_path, sizeof(root_path), "%s", root_valid ? root : "");
    close_error = 0U;
    view.revision = 1U;  // 使用非零初始版本，使界面可以识别初始化后的数据。
    active = query_pending = open_pending = 0U;
#ifndef CAN_PLAYER_SIMULATOR
    archive_revision = CanLogger_GetArchiveRevision();
#endif
}

// 更新页面激活状态；停用时取消待处理请求，资源关闭交由主循环执行。
void LogPlayer_SetActive(uint8_t enabled)
{
    active = enabled;
    if(!enabled) query_pending = open_pending = 0U;
}

// 请求重新扫描首页，不改变当前选中文件及已经完成的预览内容。
void LogPlayer_RequestRefresh(void)
{
    if(close_error != 0U) {
        view.list_state = LOG_LIST_ERROR;
        view.list_error = close_error;
        view.revision++;
        return;
    }
    active = 1U;
    query_direction = 0;
    query_anchor = requested_page = 0U;
    query_pending = 1U;
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
    query_pending = 1U;
    view.list_state = LOG_LIST_SCANNING;
    view.revision++;
}

// 校验并复制文件名，立即清除旧预览，实际关闭和打开操作延后至主循环。
void LogPlayer_RequestOpen(const char *name)
{
    if(name == NULL || archive_number(name) == 0U) return;
    if(close_error != 0U) {
        view.preview_state = LOG_PREVIEW_IO_ERROR;
        view.preview_error = close_error;
        view.revision++;
        return;
    }
    active = 1U;
    snprintf(view.selected_name, sizeof(view.selected_name), "%s", name);
    view.preview_state = LOG_PREVIEW_LOADING;
    view.preview_error = view.frame_count = view.line_number = view.selected_size = 0U;
    open_pending = 1U;
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
#ifdef CAN_PLAYER_SIMULATOR
    char pattern[320];
    snprintf(pattern, sizeof(pattern), "%s/*", root_path);
    directory = _findfirst(pattern, &directory_entry);
    if(directory == -1) { list_failed(5U); return; }
    directory_first = 1U;
#else
    FRESULT result = f_opendir(&directory, root_path);
    if(result != FR_OK) { list_failed((uint32_t)result); return; }
#endif
    directory_open = 1U;
}

// 扫描批次之间保留目录位置和候选数组，每轮最多处理四个目录项。
static void scan_directory(void)
{
    // 限制单轮目录项数量；完整扫描结束前不发布移动中的候选列表。
    for(unsigned step = 0; step < 4U; step++) {
        LogHistoryFile file = {0};
        uint8_t end = 0U;
#ifdef CAN_PLAYER_SIMULATOR
        if(!directory_first && _findnext(directory, &directory_entry) != 0) {
            if(errno != ENOENT) { list_failed(1U); return; }
            end = 1U;
        }
        directory_first = 0U;
        if(!end && !(directory_entry.attrib & _A_SUBDIR)) {
            file.number = archive_number(directory_entry.name);
            if(file.number) {
                snprintf(file.name, sizeof(file.name), "%.*s", (int)sizeof(file.name) - 1, directory_entry.name);
                file.size = (uint32_t)directory_entry.size;
            }
        }
#else
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
#endif
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
    view.revision++;
}

// 主循环受理文件选择请求，先关闭旧文件，再以只读模式打开新文件。
static void start_preview(void)
{
    open_pending = 0U;
    uint32_t error = close_reader();
    if(error != 0U) { preview_failed(LOG_PREVIEW_IO_ERROR, error); return; }
    char path[320];
    snprintf(path, sizeof(path), "%s/%s", root_path, view.selected_name);
#ifdef CAN_PLAYER_SIMULATOR
    reader = fopen(path, "rb");
    if(reader == NULL) {
        preview_failed(LOG_PREVIEW_IO_ERROR, errno == ENOENT ? 4U : errno == EACCES ? 7U : 1U);
        return;
    }
    // 模拟器通过文件定位获得大小，随后回到文件开头，保证首行仍为 CSV 表头。
    if(fseek(reader, 0, SEEK_END) != 0) {
        reader_open = 1U;
        preview_failed(LOG_PREVIEW_IO_ERROR, 1U);
        return;
    }
    long size = ftell(reader);
    if(size < 0 || fseek(reader, 0, SEEK_SET) != 0) {
        reader_open = 1U;
        preview_failed(LOG_PREVIEW_IO_ERROR, 1U);
        return;
    }
    view.selected_size = (uint32_t)size;
#else
    FRESULT result = f_open(&reader, path, FA_READ);
    if(result != FR_OK) { preview_failed(LOG_PREVIEW_IO_ERROR, (uint32_t)result); return; }
    view.selected_size = (uint32_t)f_size(&reader);
#endif
    reader_open = 1U;
    // 文件切换后必须重新校验表头，并丢弃上一个文件留下的未完整文本行。
    header_seen = 0U;
    line_length = 0U;
    view.revision++;
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
    if(!parse_number(fields[0], 10U, &frame->timestamp_us)) return 0U;
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

// 处理一条完整文本行：首行校验表头，其余非空行转换为预览帧。
static uint8_t consume_line(void)
{
    view.line_number++;
    if(line_length && line[line_length - 1U] == '\r') line_length--;  // 同时兼容 CRLF 和 LF。
    line[line_length] = '\0';
    char *text = line;
    // 仅在文件第一行允许 UTF-8 BOM，跳过后仍需完整匹配表头。
    if(view.line_number == 1U && line_length >= 3U &&
       (uint8_t)text[0] == 0xEFU && (uint8_t)text[1] == 0xBBU && (uint8_t)text[2] == 0xBFU) text += 3;
    if(!header_seen) {
        if(strcmp(text, "timestamp_us,format,type,id,dlc,data_hex") != 0) return 0U;
        header_seen = 1U;
    }
    else if(*text != '\0') {
        if(!parse_frame(text, &view.frames[view.frame_count])) return 0U;
        view.frame_count++;
        view.revision++;
    }
    line_length = 0U;  // 清空当前行长度，下一条记录从缓冲区起点继续拼接。
    return 1U;
}

// 单轮最多读取 256 字节，跨批次的不完整行保留在 line 中，不加载整份文件。
static void read_preview(void)
{
    unsigned length;
#ifdef CAN_PLAYER_SIMULATOR
    length = (unsigned)fread(chunk, 1U, sizeof(chunk), reader);
    if(ferror(reader)) { preview_failed(LOG_PREVIEW_IO_ERROR, 1U); return; }
#else
    UINT bytes;
    FRESULT result = f_read(&reader, chunk, sizeof(chunk), &bytes);
    if(result != FR_OK) { preview_failed(LOG_PREVIEW_IO_ERROR, (uint32_t)result); return; }
    length = bytes;
#endif
    for(unsigned i = 0; i < length; i++) {
        if(chunk[i] == '\n') {
            // 换行表示当前行完整；解析成功后才检查是否达到五帧预览上限。
            if(!consume_line()) { preview_failed(LOG_PREVIEW_FORMAT_ERROR, 0U); return; }
            if(view.frame_count == LOG_PLAYER_PREVIEW_ROWS) {
                // 达到上限即关闭文件，剩余内容留待后续逐帧浏览功能处理。
                preview_failed(LOG_PREVIEW_READY, 0U);
                return;
            }
        }
        else {
            if(chunk[i] == 0U || line_length >= sizeof(line) - 1U) {
                // 文本中出现 NUL 或行长度超过容量时，停止预览并报告当前物理行号。
                view.line_number++;
                preview_failed(LOG_PREVIEW_FORMAT_ERROR, 0U);
                return;
            }
            line[line_length++] = (char)chunk[i];
        }
    }
    if(length == 0U) {
        // 文件末尾可能没有换行符，剩余内容仍按最后一行校验，不能直接丢弃。
        if(line_length && !consume_line()) { preview_failed(LOG_PREVIEW_FORMAT_ERROR, 0U); return; }
        preview_failed(view.frame_count ? LOG_PREVIEW_READY : LOG_PREVIEW_EMPTY, 0U);
    }
}

/**
 * @brief 在日志写入之后推进目录扫描或文件预览。
 * @note 请求接口不执行文件操作；每轮仅推进一个有限批次，避免持续占用主循环。
 */
void LogPlayer_Process(void)
{
    if(close_error != 0U) {
        // 资源关闭失败后不继续覆盖文件对象，需复位重新初始化。
        query_pending = open_pending = 0U;
        return;
    }
    if(!active) {
        // 页面退出后释放资源；已完成的预览保留，未完成的预览取消。
        uint32_t error = close_reader();
        if(error) preview_failed(LOG_PREVIEW_IO_ERROR, error);
        if(view.preview_state == LOG_PREVIEW_LOADING) {
            view.preview_state = LOG_PREVIEW_IDLE;
            view.frame_count = 0U;
            view.revision++;
        }
        error = close_directory();
        if(error) list_failed(error);
        return;
    }
#ifndef CAN_PLAYER_SIMULATOR
    uint32_t revision = CanLogger_GetArchiveRevision();
    // 手动或自动归档生成新文件后，历史页刷新至首页；切页本身不触发归档。
    if(revision != archive_revision) {
        archive_revision = revision;
        LogPlayer_RequestRefresh();
    }
    if(CanLogger_GetState() == CAN_LOG_UNMOUNTED) {
        // 日志未挂载时拒绝文件访问，但不更改日志模块自身的错误状态。
        query_pending = open_pending = 0U;
        if(view.list_state != LOG_LIST_ERROR) list_failed(3U);
        if(reader_open || view.preview_state == LOG_PREVIEW_LOADING) preview_failed(LOG_PREVIEW_IO_ERROR, 3U);
        return;
    }
#endif
    // 新请求优先于旧任务；打开新文件时由 start_preview 统一结束旧读取。
    if(query_pending) { start_query(); return; }
    if(open_pending) { start_preview(); return; }
    // 每轮只执行一个扫描批次或一次读取，读取优先，避免用户选择后等待整个目录重扫。
    if(reader_open) read_preview();
    else if(directory_open) scan_directory();
}

// 模拟器据此缩短空闲等待时间，不代表本模块使用独立线程。
uint8_t LogPlayer_IsBusy(void)
{
    return active && !close_error && (query_pending || open_pending || directory_open || reader_open);
}

// 返回内部数据的只读视图，界面不得修改或释放，后续 Process 可能更新其内容。
const LogPlayerView *LogPlayer_GetView(void) { return &view; }
