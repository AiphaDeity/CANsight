#include "ui.h"
#include "can_stats.h"
#include "log_player.h"
#include "can_time.h"
#include <string.h>


LV_FONT_DECLARE(lv_font_cansight_14);

#define UI_COLOR_BG       0x15181D
#define UI_COLOR_PANEL    0x20252D
#define UI_COLOR_LINE     0x343B45
#define UI_COLOR_TEXT     0xE6E9ED
#define UI_COLOR_MUTED    0x9AA3AF
#define UI_COLOR_BLUE     0x2878E3

static lv_obj_t *receive_page;
static lv_obj_t *playback_page;
static lv_obj_t *receive_tab;
static lv_obj_t *playback_tab;
static lv_obj_t *total_label;       // 全局接收帧数标签。
static lv_obj_t *capacity_label;    // 统计表容量不足提示。
static lv_obj_t *can_status_label;
static lv_obj_t *sd_status_label;
static lv_obj_t *diagnostics_label;
static ui_archive_callback_t archive_callback;
static lv_obj_t *file_buttons[LOG_PLAYER_PAGE_SIZE];
static char file_button_names[LOG_PLAYER_PAGE_SIZE][CAN_LOG_ARCHIVE_NAME_SIZE];
static lv_obj_t *list_status_label;
static lv_obj_t *list_page_label;
static lv_obj_t *list_previous;
static lv_obj_t *list_next;
static lv_obj_t *preview_status_label;
static lv_obj_t *preview_table;
static lv_obj_t *preview_count_label;
static lv_obj_t *storage_label;
static char history_message[100];
static uint32_t history_message_tick;
static bool archive_notice_valid;
static CanArchiveState archive_notice_state;
static char archive_notice_name[CAN_LOG_ARCHIVE_NAME_SIZE];
static LogDeleteState displayed_delete_state;
static lv_obj_t *delete_button;
static lv_obj_t *delete_dialog;
static lv_obj_t *frame_previous;
static lv_obj_t *frame_next;
static lv_obj_t *replay_start_button;
static lv_obj_t *replay_pause_button;
static lv_obj_t *replay_stop_button;
static char delete_target[CAN_LOG_ARCHIVE_NAME_SIZE];  // 弹窗打开时固定目标，确认后不重新取当前选中项。
static uint32_t displayed_player_revision;
static bool player_display_valid;
static CanStatsRuntime displayed_runtime;
static bool runtime_display_valid;
static uint32_t displayed_total;    // 上次刷新时的累计帧数。
static uint16_t receive_offset;     // 当前页在筛选结果中的起始偏移。
#define RECEIVE_VISIBLE_ROWS 8U  // 单页最大条目数，用于限制控件内存占用。

typedef struct {
    lv_obj_t *input;
    lv_obj_t *table;
    lv_obj_t *matches;
    char text[11]; // 0x + 8 位十六进制数 + 字符串结束符
    bool validity_known;
    bool valid;
} ui_filter_t;

static ui_filter_t receive_filter;
static ui_filter_t history_filter;
static lv_obj_t *hex_keyboard;
static ui_filter_t *keyboard_filter;
static bool receive_filter_dirty;

static void refresh_player(void);
static bool parse_filter(const char *text, uint32_t *id, bool *all);

// 历史筛选通过 Apply 或键盘确认提交，输入过程中不反复扫描文件。
static bool history_filter_applied(void)
{
    uint32_t id;
    bool all;
    const LogPlayerView *view = LogPlayer_GetView();
    return parse_filter(history_filter.text, &id, &all) &&
        view->filter_enabled == !all && (all || view->filter_id == id);
}

static bool parse_filter(const char *text, uint32_t *id, bool *all)
{
    *id = 0;
    *all = text[0] == '\0';
    if(*all) return true;
    if(text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text += 2;
    if(*text == '\0') return false;

    uint32_t digits = 0;
    while(*text != '\0') {
        uint32_t digit;
        if(*text >= '0' && *text <= '9') digit = (uint32_t)(*text - '0');
        else if(*text >= 'A' && *text <= 'F') digit = (uint32_t)(*text - 'A' + 10);
        else if(*text >= 'a' && *text <= 'f') digit = (uint32_t)(*text - 'a' + 10);
        else return false;
        if(++digits > 8) return false;
        *id = (*id << 4) | digit;
        text++;
    }
    return *id <= 0x1FFFFFFFU;
}

// 内容变化时才更新单元格，避免周期刷新反复分配内存及重算相同文本。
static void set_table_cell_if_changed(lv_obj_t *table, uint32_t row,
                                      uint32_t column, const char *text)
{
    const char *previous = lv_table_get_cell_value(table, row, column);
    if(previous == NULL || strcmp(previous, text) != 0) {
        lv_table_set_cell_value(table, row, column, text);
    }
}

// 相同文案不重新设置，避免匹配统计反复触发重绘。
static void set_label_if_changed(lv_obj_t *label, const char *text)
{
    if(strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

// 操作结果仅短暂显示，历史加载和错误状态由统一刷新函数优先呈现。
static void set_history_message(const char *text)
{
    lv_snprintf(history_message, sizeof(history_message), "%s", text);
    history_message_tick = lv_tick_get();
}

// 输入框仅在有效性变化时更新边框，初始化时设置一次默认样式。
static void set_filter_validity(ui_filter_t *filter, bool valid)
{
    if(!filter->validity_known || filter->valid != valid) {
        lv_obj_set_style_border_color(filter->input,
            lv_color_hex(valid ? UI_COLOR_LINE : 0xE35B5B), LV_PART_MAIN);
        filter->validity_known = true;
        filter->valid = valid;
    }
}

// 根据筛选条件和分页偏移刷新接收表格，不修改底层统计。
static void refresh_receive(ui_filter_t *filter)
{
    // 编辑期间仅更新输入框，避免每次退格都重绘报文表格。
    if(hex_keyboard != NULL && keyboard_filter == filter) return;
    receive_filter_dirty = false;
    uint32_t id;
    bool all;
    bool valid = parse_filter(filter->text, &id, &all);  // 空筛选条件匹配全部条目。
    uint16_t selected[CAN_STATS_CAPACITY];  // 匹配条目的索引列表，不复制报文数据。
    uint16_t matches = 0;  // 匹配条目数。
    uint16_t count = CanStats_GetEntryCount();  // 有效统计条目数。
    for(uint16_t i = 0; i < count; i++) {
        const CanStatsEntry *entry = CanStats_GetEntry(i);
        if(valid && (all || entry->latest.id == id)) selected[matches++] = i;
    }
    // 筛选结果变化或分页偏移越界时，重置为首页。
    if(receive_offset >= matches) receive_offset = 0;
    uint16_t visible = matches - receive_offset;
    if(visible > RECEIVE_VISIBLE_ROWS) visible = RECEIVE_VISIBLE_ROWS;
    // 表格行数在创建时固定，新增 ID、筛选及翻页不再改变整体布局。
    for(uint16_t i = 0; i < visible; i++) {
        // 将页面偏移映射为统计表索引。
        const CanStatsEntry *entry = CanStats_GetEntry(selected[receive_offset + i]);
        const CanFrame *frame = &entry->latest;
        uint16_t row = i + 1U;
        char text[32];
        lv_snprintf(text, sizeof(text), "0x%lX", (unsigned long)frame->id);
        set_table_cell_if_changed(filter->table, row, 0, text);
        // 使用 RTR 后缀区分远程帧与数据帧。
        lv_snprintf(text, sizeof(text), "%s%s", frame->ide ? "扩展" : "标准",
                    frame->rtr ? " RTR" : "");
        set_table_cell_if_changed(filter->table, row, 1, text);
        lv_snprintf(text, sizeof(text), "%u", (unsigned)frame->dlc);
        set_table_cell_if_changed(filter->table, row, 2, text);
        // 8 字节数据格式化后最多为 23 个字符，不含字符串结束符。
        unsigned used = 0;  // 已写入的字符数。
        text[0] = '\0';
        if(frame->rtr == 0U) {  // 远程帧不包含有效数据，数据列保持为空。
            for(uint8_t j = 0; j < frame->dlc; j++) {
                used += (unsigned)lv_snprintf(text + used, sizeof(text) - used,
                    j ? " %02X" : "%02X", (unsigned)frame->data[j]);
            }
        }
        set_table_cell_if_changed(filter->table, row, 3, text);
        lv_snprintf(text, sizeof(text), "%lu", (unsigned long)entry->count);
        set_table_cell_if_changed(filter->table, row, 4, text);
    }
    // 筛选或翻页后清空未使用的位置，避免上一页的报文残留。
    for(uint16_t row = visible + 1U; row <= RECEIVE_VISIBLE_ROWS; row++) {
        for(uint32_t col = 0U; col < 5U; col++) {
            set_table_cell_if_changed(filter->table, row, col, "");
        }
    }
    // 匹配范围不随报文计数变化，不重复提交相同文案。
    char summary[80];
    if(!valid) lv_snprintf(summary, sizeof(summary), "ID 格式错误");
    else lv_snprintf(summary, sizeof(summary), "匹配 %u / 共 %u  [%u-%u]",
        (unsigned)matches, (unsigned)count,
        matches ? (unsigned)receive_offset + 1U : 0U,
        (unsigned)(receive_offset + visible));
    set_label_if_changed(filter->matches, summary);
    set_filter_validity(filter, valid);
}

static void refresh_filter(ui_filter_t *filter)
{
    if(filter == &history_filter) {
        uint32_t id;
        bool all;
        bool valid = parse_filter(filter->text, &id, &all);
        set_filter_validity(filter, valid);
        if(valid) LogPlayer_SetFilter(id, !all);
        player_display_valid = false;
        refresh_player();
    }
    else refresh_receive(filter);
}

static void close_hex_keyboard(void)
{
    if(hex_keyboard == NULL) return;
    lv_keyboard_set_textarea(hex_keyboard, NULL);
    lv_obj_set_hidden(hex_keyboard, true);
    // 键盘在启动时创建并复用，关闭时只隐藏和解除输入绑定。
    keyboard_filter = NULL;
}

static void keyboard_done_cb(lv_event_t *event)
{
    if(lv_event_get_code(event) == LV_EVENT_READY && keyboard_filter == &history_filter)
        refresh_filter(&history_filter);
    close_hex_keyboard();
    keyboard_filter = NULL;
}

static void create_hex_keyboard(void)
{
    static const char * const map[] = {
        "1", "2", "3", "A", "B", "\n",
        "4", "5", "6", "C", "D", "\n",
        "7", "8", "9", "E", "F", "\n",
        LV_SYMBOL_CLOSE, "0", "0x", LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, ""
    };
    static const lv_buttonmatrix_ctrl_t controls[] = {
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
        1, 1, 1, 1, 1, 1, 1, 1, 1 | LV_BUTTONMATRIX_CTRL_NO_REPEAT, 1
    };
    if(hex_keyboard == NULL) {
        hex_keyboard = lv_keyboard_create(lv_layer_top());
        lv_obj_set_hidden(hex_keyboard, true);
        lv_obj_set_scrollable(hex_keyboard, false);
        lv_obj_set_size(hex_keyboard, 520, 148);
        lv_obj_align(hex_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_keyboard_set_map(hex_keyboard, LV_KEYBOARD_MODE_USER_1, map, controls);
        lv_keyboard_set_mode(hex_keyboard, LV_KEYBOARD_MODE_USER_1);
        lv_keyboard_set_popovers(hex_keyboard, false);
        lv_obj_set_style_bg_color(hex_keyboard, lv_color_hex(UI_COLOR_PANEL), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(hex_keyboard, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(hex_keyboard, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(hex_keyboard, lv_color_hex(UI_COLOR_LINE), LV_PART_ITEMS);
        lv_obj_set_style_text_color(hex_keyboard, lv_color_hex(UI_COLOR_TEXT), LV_PART_ITEMS);
        lv_obj_set_style_text_font(hex_keyboard, &lv_font_montserrat_14, LV_PART_ITEMS);
        lv_obj_add_event_cb(hex_keyboard, keyboard_done_cb, LV_EVENT_READY, NULL);
        lv_obj_add_event_cb(hex_keyboard, keyboard_done_cb, LV_EVENT_CANCEL, NULL);
    }
}

static void filter_clicked_cb(lv_event_t *event)
{
    ui_filter_t *filter = lv_event_get_user_data(event);
    keyboard_filter = filter;
    lv_keyboard_set_textarea(hex_keyboard, filter->input);
    lv_obj_set_hidden(hex_keyboard, false);
}

static void filter_changed_cb(lv_event_t *event)
{
    ui_filter_t *filter = lv_event_get_user_data(event);
    lv_snprintf(filter->text, sizeof(filter->text), "%s", lv_textarea_get_text(filter->input));
    if(filter == &receive_filter) {
        receive_offset = 0;
        receive_filter_dirty = true;
        if(hex_keyboard != NULL && keyboard_filter == filter) {
            uint32_t id;
            bool all;
            set_filter_validity(filter, parse_filter(filter->text, &id, &all));
            return;
        }
    }
    if(filter == &history_filter) {
        uint32_t id;
        bool all;
        bool valid = parse_filter(filter->text, &id, &all);
        lv_obj_set_style_border_color(filter->input,
            lv_color_hex(valid ? UI_COLOR_LINE : 0xE35B5B), LV_PART_MAIN);
        player_display_valid = false;
        refresh_player();
        return;
    }
    // 筛选条件变更后重置分页，并立即更新显示。
    receive_offset = 0;
    refresh_filter(filter);
}

static void filter_clear_cb(lv_event_t *event)
{
    ui_filter_t *filter = lv_event_get_user_data(event);
    close_hex_keyboard();
    lv_textarea_set_text(filter->input, "");
    // 带字符限制的输入框，设置空字符串不一定触发 VALUE_CHANGED。
    filter->text[0] = '\0';
    receive_offset = 0;
    refresh_filter(filter);
}

// 禁用时仅改变文字和边框，覆盖主题的浅色重着色，不创建半透明图层。
static void style_disabled_control(lv_obj_t *obj)
{
    // 当前 LVGL 直接遍历 transition->props，禁用过渡须提供有效的空列表。
    static const lv_style_prop_t no_transition_props[] = {0};
    static const lv_style_transition_dsc_t no_transition = {.props = no_transition_props};
    lv_obj_set_style_recolor_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_style_text_color(obj, lv_color_hex(0x68717D), LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_style_border_color(obj, lv_color_hex(0x292F38), LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_style_transition(obj, &no_transition, LV_PART_MAIN);
    lv_obj_set_style_transition(obj, &no_transition, LV_PART_MAIN | LV_STATE_DISABLED);
}

static void bind_filter(ui_filter_t *filter, lv_obj_t *input, lv_obj_t *clear,
                        lv_obj_t *table, lv_obj_t *matches)
{
    filter->input = input;
    filter->table = table;
    filter->matches = matches;
    filter->validity_known = false;
    style_disabled_control(input);
    lv_obj_set_style_text_color(input, lv_color_hex(0x68717D),
        LV_PART_TEXTAREA_PLACEHOLDER | LV_STATE_DISABLED);
    // 输入文字和占位提示是不同的绘制部分，两处都显式指定精简中文字体。
    lv_obj_set_style_text_font(input, &lv_font_cansight_14, LV_PART_MAIN);
    lv_obj_set_style_text_font(input, &lv_font_cansight_14, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_text_color(input, lv_color_hex(UI_COLOR_MUTED), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_textarea_set_max_length(input, sizeof(filter->text) - 1);
    lv_textarea_set_accepted_chars(input, "0123456789abcdefABCDEFxX");
    lv_textarea_set_text(input, filter->text);
    lv_obj_add_event_cb(input, filter_clicked_cb, LV_EVENT_CLICKED, filter);
    lv_obj_add_event_cb(input, filter_changed_cb, LV_EVENT_VALUE_CHANGED, filter);
    lv_obj_add_event_cb(clear, filter_clear_cb, LV_EVENT_CLICKED, filter);
    refresh_filter(filter);
}

// 接收和回放表格共用外观，避免只修好其中一页。
static void style_data_table(lv_obj_t *table)
{
    // 表格仅显示数据，不使用单元格选择；默认按下和松开事件会使整表失效。
    lv_obj_set_clickable(table, false);
    lv_obj_set_click_focusable(table, false);
    lv_obj_set_scrollable(table, false);
    lv_obj_set_scroll_chain(table, false);
    lv_obj_set_style_pad_all(table, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(table, lv_color_hex(UI_COLOR_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(table, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(table, 8, LV_PART_ITEMS);
    lv_obj_set_style_pad_ver(table, 6, LV_PART_ITEMS);
    lv_obj_set_style_text_font(table, &lv_font_cansight_14, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(table, lv_color_hex(UI_COLOR_PANEL), LV_PART_ITEMS);
    lv_obj_set_style_text_color(table, lv_color_hex(UI_COLOR_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_border_color(table, lv_color_hex(UI_COLOR_LINE), LV_PART_ITEMS);
    lv_obj_set_style_border_width(table, 1, LV_PART_ITEMS);
}

// 创建了标签控件并设置属性
static lv_obj_t *create_label(lv_obj_t *parent, const char *text,
                              lv_coord_t x, lv_coord_t y, lv_coord_t width,
                              lv_color_t color)
{
    // 创建标签并设置文本内容和位置
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    
    // 设置标签宽度和文本颜色样式
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_cansight_14, LV_PART_MAIN);
    
    return label;
}

//创建按钮控件
static lv_obj_t *create_button(lv_obj_t *parent, const char *text,
                               lv_coord_t x, lv_coord_t y, lv_coord_t width,
                               lv_coord_t height, bool selected)
{
    //创建按钮并设置位置和尺寸
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_scrollable(button, false);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);

    //设置按钮样式：背景色根据选中状态决定，添加边框和圆角 
    lv_obj_set_style_bg_color(button,
                              lv_color_hex(selected ? UI_COLOR_BLUE : UI_COLOR_PANEL),
                              LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lv_color_hex(UI_COLOR_LINE), LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(button, 6, LV_PART_MAIN);
    lv_obj_set_style_text_color(button, lv_color_hex(UI_COLOR_TEXT), LV_PART_MAIN);
    style_disabled_control(button);

    //创建按钮文本标签并居中显示
    lv_obj_t *label = lv_label_create(button);
    lv_obj_set_style_text_font(label, &lv_font_cansight_14, LV_PART_MAIN);
    lv_label_set_text(label, text);
    // 文字继承按钮颜色，随父按钮禁用状态变暗。
    lv_obj_center(label);
    return button;
}

//显示指定的页面（接收页面或回放页面）
static void create_receive_page(lv_obj_t *screen);
static void create_playback_page(lv_obj_t *screen);

void ui_set_archive_callback(ui_archive_callback_t callback)
{
    archive_callback = callback;
}

// 仅由手动归档按钮提交请求，页面切换不改变活动文件。
static void request_archive(void)
{
    if(archive_callback != NULL) {
        if(playback_page != NULL) set_history_message("Archive requested...");
        archive_callback();
    }
    else if(playback_page != NULL) {
        set_history_message("Simulator: no SD card");
    }
}

static void archive_event_cb(lv_event_t *event)
{
    (void)event;
    request_archive();
}

// 弹窗可能从自身事件关闭，先隐藏并延后释放，避免事件处理期间访问已释放对象。
static void close_delete_dialog(void)
{
    if(delete_dialog == NULL) return;
    lv_obj_set_hidden(delete_dialog, true);
    lv_obj_delete_async(delete_dialog);
    delete_dialog = NULL;
}

static void show_page(bool show_playback)
{
    close_hex_keyboard();
    close_delete_dialog();
    // 两页不同时驻留，避免同时占用两张表格的 LVGL 动态内存。
    lv_obj_t *screen = lv_screen_active();
    if(show_playback) {
        if(receive_page != NULL) {
            lv_obj_delete(receive_page);
            receive_page = NULL;
            receive_filter.input = receive_filter.table = receive_filter.matches = NULL;
        }
        if(playback_page == NULL) {
            LogPlayer_RequestRefresh();
            create_playback_page(screen);
        }
    }
    else {
        if(playback_page != NULL) {
            lv_obj_delete(playback_page);
            playback_page = NULL;
            preview_status_label = NULL;
            history_filter.input = history_filter.table = history_filter.matches = NULL;
            LogPlayer_SetActive(0U);
        }
        if(receive_page == NULL) {
            create_receive_page(screen);
        }
    }

    //更新标签页背景颜色，选中的标签页显示蓝色高亮
    lv_obj_set_style_bg_color(receive_tab,
                              lv_color_hex(show_playback ? UI_COLOR_PANEL : UI_COLOR_BLUE),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(playback_tab,
                              lv_color_hex(show_playback ? UI_COLOR_BLUE : UI_COLOR_PANEL),
                              LV_PART_MAIN);
}

//标签页切换事件回调函数
static void tab_event_cb(lv_event_t *event)
{
    //判断当前触发事件的标签是否为回放标签页
    bool show_playback = lv_event_get_current_target(event) == playback_tab;
    //根据判断结果切换显示对应的页面和标签样式
    show_page(show_playback);
}

/**
 * @brief 将 FatFs 错误码映射为名称，供历史操作错误提示使用。
 */
static const char *fatfs_error_name(uint32_t error)
{
    // 错误值对应当前 FatFs ff.h 的 FRESULT 定义。
    static const char * const names[] = {
        "FR_OK", "FR_DISK_ERR", "FR_INT_ERR", "FR_NOT_READY", "FR_NO_FILE",
        "FR_NO_PATH", "FR_INVALID_NAME", "FR_DENIED", "FR_EXIST", "FR_INVALID_OBJECT",
        "FR_WRITE_PROTECTED", "FR_INVALID_DRIVE", "FR_NOT_ENABLED", "FR_NO_FILESYSTEM",
        "FR_MKFS_ABORTED", "FR_TIMEOUT", "FR_LOCKED", "FR_NOT_ENOUGH_CORE",
        "FR_TOO_MANY_OPEN_FILES", "FR_INVALID_PARAMETER"
    };
    return error < sizeof(names) / sizeof(names[0]) ? names[error] : "FR_UNKNOWN";
}

// 归档仅在状态或文件名变化时生成临时提示，不常驻展示最近归档文件。
static void refresh_archive(const CanStatsRuntime *runtime)
{
    if(playback_page == NULL) return;
    bool changed = archive_notice_valid &&
        (archive_notice_state != runtime->archive_state ||
         strcmp(archive_notice_name, runtime->archive_name) != 0);
    archive_notice_state = runtime->archive_state;
    lv_snprintf(archive_notice_name, sizeof(archive_notice_name), "%s", runtime->archive_name);
    archive_notice_valid = true;
    if(!changed) return;
    switch(runtime->archive_state) {
        case CAN_ARCHIVE_DONE: set_history_message("Archive complete"); break;
        case CAN_ARCHIVE_EMPTY: set_history_message("No frames; archive skipped"); break;
        default: break;
    }
}

// 进度和错误共用一行；正常就绪时不重复解释记录状态或时间戳格式。
static void refresh_history_status(const LogPlayerView *view)
{
    char text[120] = "";
    uint32_t color = UI_COLOR_MUTED;
    const LogReplayView *replay = LogPlayer_GetReplay();
    if(view->delete_state != displayed_delete_state) {
        displayed_delete_state = view->delete_state;
        if(view->delete_state == LOG_DELETE_DONE) set_history_message("File deleted");
    }
    const CanStatsRuntime *runtime = CanStats_GetRuntime();
    if(replay->state == LOG_REPLAY_STOP_FAILED) {
        lv_snprintf(text, sizeof(text), "Stop unconfirmed; recording paused; reset required");
        color = 0xE35B5B;
    }
    else if(view->delete_state == LOG_DELETE_ERROR) {
        lv_snprintf(text, sizeof(text), "Delete failed: %lu (%s)",
            (unsigned long)view->delete_error, fatfs_error_name(view->delete_error));
        color = 0xE35B5B;
    }
    else if(view->list_state == LOG_LIST_ERROR) {
        lv_snprintf(text, sizeof(text), "List error: %lu (%s)",
            (unsigned long)view->list_error, fatfs_error_name(view->list_error));
        color = 0xE35B5B;
    }
    else if(view->preview_state == LOG_PREVIEW_FORMAT_ERROR) {
        lv_snprintf(text, sizeof(text), "CSV format error at line %lu", (unsigned long)view->line_number);
        color = 0xE35B5B;
    }
    else if(view->preview_state == LOG_PREVIEW_IO_ERROR) {
        lv_snprintf(text, sizeof(text), "Read error: %lu (%s)",
            (unsigned long)view->preview_error, fatfs_error_name(view->preview_error));
        color = 0xE35B5B;
    }
    else if(archive_callback != NULL && runtime->archive_state == CAN_ARCHIVE_FAILED) {
        lv_snprintf(text, sizeof(text), "Archive failed; see SD error");
        color = 0xE35B5B;
    }
    else if(view->storage_state == LOG_STORAGE_ERROR) {
        lv_snprintf(text, sizeof(text), "Capacity error: %lu (%s)",
            (unsigned long)view->storage_error, fatfs_error_name(view->storage_error));
        color = 0xE35B5B;
    }
    else if(view->delete_state == LOG_DELETE_PENDING) lv_snprintf(text, sizeof(text), "Deleting...");
    else if(archive_callback != NULL && runtime->archive_state == CAN_ARCHIVE_PENDING)
        lv_snprintf(text, sizeof(text), "Archiving...");
    else if(view->preview_state == LOG_PREVIEW_INDEXING) {
        lv_snprintf(text, sizeof(text), "Indexing %lu%%",
            (unsigned long)(view->selected_size ? (uint64_t)view->scanned_bytes * 100U / view->selected_size : 0U));
    }
    else if(view->preview_state == LOG_PREVIEW_LOADING) lv_snprintf(text, sizeof(text), "Locating frame...");
    else if(view->list_state == LOG_LIST_SCANNING) lv_snprintf(text, sizeof(text), "Scanning...");
    else if(history_message[0] && lv_tick_get() - history_message_tick < 3000U)
        lv_snprintf(text, sizeof(text), "%s", history_message);
    else if(replay->state != LOG_REPLAY_IDLE) {
        static const char * const states[] = {
            "", "Preparing replay", "Replaying", "Paused", "Stopping", "Replay complete", "Replay error",
            "Stop unconfirmed", "Stopped"
        };
        lv_snprintf(text, sizeof(text), "%s %lu/%lu | late %lu ms%s",
            states[replay->state], (unsigned long)replay->sent_frames,
            (unsigned long)replay->total_frames, (unsigned long)(replay->max_late_us / 1000U),
            LogPlayer_ReplayBusy() ? " | recording paused" : "");
        if(replay->state == LOG_REPLAY_ERROR || replay->state == LOG_REPLAY_STOP_FAILED) {
            lv_snprintf(text, sizeof(text), "Error %lu; sent %lu | cancel %lu | fail %lu | unknown %lu",
                (unsigned long)replay->error, (unsigned long)replay->sent_frames,
                (unsigned long)replay->cancelled_frames, (unsigned long)replay->failed_frames,
                (unsigned long)replay->unconfirmed_frames);
            if(replay->state == LOG_REPLAY_STOP_FAILED)
                lv_snprintf(text, sizeof(text), "Stop unconfirmed; recording paused; reset required");
            color = 0xE35B5B;
        }
        else if(replay->state == LOG_REPLAY_STOPPED) {
            lv_snprintf(text, sizeof(text), "Stopped; sent %lu | cancel %lu | fail %lu | unknown %lu",
                (unsigned long)replay->sent_frames, (unsigned long)replay->cancelled_frames,
                (unsigned long)replay->failed_frames, (unsigned long)replay->unconfirmed_frames);
        }
    }
    else if(view->preview_state == LOG_PREVIEW_EMPTY)
        lv_snprintf(text, sizeof(text), "%s", view->total_frames ? "No matching frames" : "Empty log");
    else if(view->total_files == 0U) lv_snprintf(text, sizeof(text), "No history logs");
    set_label_if_changed(preview_status_label, text);
    if(!lv_color_eq(lv_obj_get_style_text_color(preview_status_label, LV_PART_MAIN), lv_color_hex(color))) {
        lv_obj_set_style_text_color(preview_status_label, lv_color_hex(color), LV_PART_MAIN);
    }
}

// 按数据版本更新文件列表与预览，文件读取由主循环完成。
static void refresh_player(void)
{
    if(playback_page == NULL) return;
    const LogPlayerView *view = LogPlayer_GetView();
    refresh_history_status(view);
    const LogReplayView *replay = LogPlayer_GetReplay();
    bool replay_busy = LogPlayer_ReplayBusy() != 0U;
    lv_obj_set_disabled(replay_start_button, replay_busy || !view->index_ready || !view->total_frames);
    lv_obj_set_disabled(replay_pause_button,
        replay->state != LOG_REPLAY_RUNNING && replay->state != LOG_REPLAY_PAUSED);
    lv_obj_set_disabled(replay_stop_button, !replay_busy);
    set_label_if_changed(lv_obj_get_child(replay_pause_button, 0),
        replay->state == LOG_REPLAY_PAUSED ? "Resume" : "Pause");
    for(unsigned i = 0; i < LOG_PLAYER_PAGE_SIZE; i++) lv_obj_set_disabled(file_buttons[i], replay_busy);
    lv_obj_set_disabled(history_filter.input, replay_busy);
    if(replay_busy) {
        lv_obj_set_disabled(delete_button, true);
        lv_obj_set_disabled(frame_previous, true);
        lv_obj_set_disabled(frame_next, true);
        player_display_valid = false;
        return;
    }
    if(player_display_valid && displayed_player_revision == view->revision) return;
    player_display_valid = true;
    displayed_player_revision = view->revision;
    if(view->list_state == LOG_LIST_SCANNING) lv_label_set_text(list_status_label, "Scanning...");
    else if(view->list_state == LOG_LIST_ERROR) {
        lv_label_set_text_fmt(list_status_label, "List error: %lu", (unsigned long)view->list_error);
    }
    else lv_label_set_text_fmt(list_status_label, "%lu files", (unsigned long)view->total_files);

    for(unsigned i = 0; i < LOG_PLAYER_PAGE_SIZE; i++) {
        bool visible = view->list_state == LOG_LIST_READY && i < view->file_count;
        lv_obj_set_hidden(file_buttons[i], !visible);
        if(!visible) continue;
        lv_snprintf(file_button_names[i], sizeof(file_button_names[i]), "%s", view->files[i].name);
        lv_label_set_text(lv_obj_get_child(file_buttons[i], 0), file_button_names[i]);
        lv_obj_set_style_bg_color(file_buttons[i], lv_color_hex(
            strcmp(view->selected_name, file_button_names[i]) == 0 ? UI_COLOR_BLUE : UI_COLOR_PANEL),
            LV_PART_MAIN);
    }
    uint32_t pages = (view->total_files + LOG_PLAYER_PAGE_SIZE - 1U) / LOG_PLAYER_PAGE_SIZE;
    lv_label_set_text_fmt(list_page_label, "%lu / %lu",
        (unsigned long)(view->file_count ? view->page + 1U : 0U), (unsigned long)pages);
    lv_obj_set_disabled(list_previous, view->list_state != LOG_LIST_READY || view->page == 0U);
    lv_obj_set_disabled(list_next, view->list_state != LOG_LIST_READY ||
        (view->page + 1U) * LOG_PLAYER_PAGE_SIZE >= view->total_files);

    if(view->filter_enabled) {
        lv_label_set_text_fmt(preview_count_label, "Match %lu/%lu | frame %lu/%lu",
            (unsigned long)(view->frame_count ? view->first_match + 1U : 0U),
            (unsigned long)view->matched_frames,
            (unsigned long)(view->frame_count ? view->frame_numbers[0] : 0U),
            (unsigned long)view->total_frames);
    }
    else {
        lv_label_set_text_fmt(preview_count_label, "Frame %lu / %lu",
            (unsigned long)(view->frame_count ? view->frame_numbers[0] : 0U),
            (unsigned long)view->total_frames);
    }
    bool applied = history_filter_applied();
    if(!applied) lv_label_set_text(preview_count_label, "Edit ID, then press Apply");
    bool navigating = applied && view->index_ready && view->preview_state == LOG_PREVIEW_READY &&
        view->delete_state != LOG_DELETE_PENDING;
    lv_obj_set_disabled(frame_previous, !navigating || view->first_match == 0U);
    lv_obj_set_disabled(frame_next, !navigating || view->first_match + 1U >= view->matched_frames);
    lv_obj_set_style_text_color(preview_count_label, lv_color_hex(UI_COLOR_MUTED), LV_PART_MAIN);
    lv_obj_set_disabled(delete_button, view->selected_name[0] == '\0' ||
        view->delete_state == LOG_DELETE_PENDING || view->list_state != LOG_LIST_READY);
    lv_obj_set_style_text_color(storage_label, lv_color_hex(UI_COLOR_MUTED), LV_PART_MAIN);
    if(view->storage_state == LOG_STORAGE_READY) {
        // 使用二进制容量单位和整数格式化，避免引入浮点与 64 位 printf 支持。
        uint64_t unit = 1024ULL * 1024ULL;
        bool low = view->free_bytes < CAN_LOG_MAX_FILE_SIZE;
        lv_label_set_text_fmt(storage_label, "%s%s free: %lu.%lu MiB",
            low ? "LOW SPACE | " : "", archive_callback == NULL ? "Disk" : "SD",
            (unsigned long)(view->free_bytes / unit),
            (unsigned long)((view->free_bytes % unit) * 10U / unit));
        if(low) lv_obj_set_style_text_color(storage_label, lv_color_hex(0xE3B34E), LV_PART_MAIN);
    }
    else if(view->storage_state == LOG_STORAGE_ERROR) {
        lv_label_set_text_fmt(storage_label, "Capacity error: %lu (%s)",
            (unsigned long)view->storage_error, fatfs_error_name(view->storage_error));
        lv_obj_set_style_text_color(storage_label, lv_color_hex(0xE35B5B), LV_PART_MAIN);
    }
    else lv_label_set_text(storage_label, "Reading capacity...");
    lv_table_set_row_count(preview_table, view->frame_count + 1U);
    for(unsigned i = 0; i < view->frame_count; i++) {
        const CanFrame *frame = &view->frames[i];
        char text[32];
        CanTime_FormatFixed(text, frame->timestamp_us, 1000000U, 6U);
        set_table_cell_if_changed(preview_table, i + 1U, 0, text);
        lv_snprintf(text, sizeof(text), "0x%lX", (unsigned long)frame->id);
        set_table_cell_if_changed(preview_table, i + 1U, 1, text);
        lv_snprintf(text, sizeof(text), "%s%s", frame->ide ? "扩展" : "标准", frame->rtr ? " RTR" : "");
        set_table_cell_if_changed(preview_table, i + 1U, 2, text);
        lv_snprintf(text, sizeof(text), "%u", (unsigned)frame->dlc);
        set_table_cell_if_changed(preview_table, i + 1U, 3, text);
        unsigned used = 0U;
        text[0] = '\0';
        if(frame->rtr == 0U) {
            for(unsigned j = 0; j < frame->dlc; j++) {
                used += (unsigned)lv_snprintf(text + used, sizeof(text) - used,
                    j ? " %02X" : "%02X", (unsigned)frame->data[j]);
            }
        }
        set_table_cell_if_changed(preview_table, i + 1U, 4, text);
    }
}

static void file_event_cb(lv_event_t *event)
{
    // 使用按钮实际显示的文件名，避免目录更新与界面刷新之间选择错误条目。
    LogPlayer_RequestOpen(lv_event_get_user_data(event));
    refresh_player();
}

static void history_page_cb(lv_event_t *event)
{
    LogPlayer_RequestPage((int)(intptr_t)lv_event_get_user_data(event));
    refresh_player();
}

static void history_refresh_cb(lv_event_t *event)
{
    (void)event;
    LogPlayer_RequestRefresh();
    refresh_player();
}

static void history_filter_apply_cb(lv_event_t *event)
{
    (void)event;
    close_hex_keyboard();
    refresh_filter(&history_filter);
}

static void history_step_cb(lv_event_t *event)
{
    if(history_filter_applied()) LogPlayer_RequestStep((int)(intptr_t)lv_event_get_user_data(event));
    refresh_player();
}

// 使用本地 LVGL 的绘制任务接口标出当前帧，不为每一行创建独立控件。
static void history_table_draw_cb(lv_event_t *event)
{
    lv_draw_fill_dsc_t *fill = lv_draw_task_get_fill_dsc(lv_event_get_draw_task(event));
    if(fill != NULL && fill->base.part == LV_PART_ITEMS && fill->base.id1 == 1U &&
       LogPlayer_GetView()->preview_state == LOG_PREVIEW_READY) {
        fill->color = lv_color_hex(0x293B52);
    }
}

// 确认按钮仅提交固定目标的删除请求，文件系统操作由主循环处理。
static void delete_confirm_cb(lv_event_t *event)
{
    bool confirm = lv_event_get_user_data(event) != NULL;
    close_delete_dialog();
    if(confirm && !LogPlayer_RequestDelete(delete_target)) {
        set_history_message("Delete cancelled: selection unavailable");
        return;
    }
    refresh_player();
}

static void history_delete_cb(lv_event_t *event)
{
    (void)event;
    const LogPlayerView *view = LogPlayer_GetView();
    if(delete_dialog != NULL || view->selected_name[0] == '\0' ||
       view->delete_state == LOG_DELETE_PENDING) return;
    close_hex_keyboard();
    lv_snprintf(delete_target, sizeof(delete_target), "%s", view->selected_name);
    // 顶层遮罩拦截背景操作，防止确认期间切换文件或重复归档。
    delete_dialog = lv_obj_create(lv_layer_top());
    lv_obj_set_size(delete_dialog, 800, 480);
    lv_obj_set_pos(delete_dialog, 0, 0);
    lv_obj_set_style_pad_all(delete_dialog, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(delete_dialog, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(delete_dialog, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(delete_dialog, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(delete_dialog, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_scroll_dir(delete_dialog, LV_DIR_NONE);
    lv_obj_t *panel = lv_obj_create(delete_dialog);
    lv_obj_set_size(panel, 440, 210);
    lv_obj_center(panel);
    lv_obj_set_style_pad_all(panel, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(panel, lv_color_hex(UI_COLOR_PANEL), LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, lv_color_hex(UI_COLOR_LINE), LV_PART_MAIN);
    lv_obj_set_scroll_dir(panel, LV_DIR_NONE);
    create_label(panel, "Delete history file?", 20, 18, 400, lv_color_hex(UI_COLOR_TEXT));
    create_label(panel, delete_target, 20, 52, 400, lv_color_hex(UI_COLOR_TEXT));
    create_label(panel, "This cannot be undone.", 20, 86, 400, lv_color_hex(0xE3B34E));
    lv_obj_t *cancel = create_button(panel, "Cancel", 20, 142, 185, 42, false);
    lv_obj_t *confirm = create_button(panel, "Delete", 230, 142, 185, 42, false);
    lv_obj_set_style_bg_color(confirm, lv_color_hex(0xA83F3F), LV_PART_MAIN);
    lv_obj_add_event_cb(cancel, delete_confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(confirm, delete_confirm_cb, LV_EVENT_CLICKED, delete_target);
}

static void refresh_runtime(void)
{
    const CanStatsRuntime *runtime = CanStats_GetRuntime();
    if(!runtime_display_valid || runtime->can_state != displayed_runtime.can_state) {
        const char *text = "● CAN1 未启动";
        uint32_t color = UI_COLOR_MUTED;
        if(runtime->can_state == CAN_STATS_RUNNING) {
            text = "● CAN1 运行中";
            color = 0x43D17A;
        }
        else if(runtime->can_state == CAN_STATS_ERROR) {
            text = "● CAN1 错误";
            color = 0xE35B5B;
        }
        lv_label_set_text(can_status_label, text);
        lv_obj_set_style_text_color(can_status_label, lv_color_hex(color), LV_PART_MAIN);
    }

    if(!runtime_display_valid || runtime->sd_state != displayed_runtime.sd_state) {
        const char *text = "● SD 未挂载";
        uint32_t color = UI_COLOR_MUTED;
        switch(runtime->sd_state) {
            case CAN_LOG_READY: text = "● SD 就绪"; color = UI_COLOR_BLUE; break;
            case CAN_LOG_RECORDING: text = "● SD 记录中"; color = 0x43D17A; break;
            case CAN_LOG_PAUSED: text = "● SD 暂停"; break;
            case CAN_LOG_ERROR: text = "● SD 错误"; color = 0xE35B5B; break;
            default: break;
        }
        lv_label_set_text(sd_status_label, text);
        lv_obj_set_style_text_color(sd_status_label, lv_color_hex(color), LV_PART_MAIN);
    }

    // 屏幕只提示异常类别；累计计数、错误码和耗时由串口诊断提供。
    if(!runtime_display_valid || runtime->queue_lost != displayed_runtime.queue_lost ||
       runtime->fifo_overruns != displayed_runtime.fifo_overruns ||
       runtime->error_flags != displayed_runtime.error_flags ||
       runtime->sd_error != displayed_runtime.sd_error ||
       runtime->logger.rejected_frames != displayed_runtime.logger.rejected_frames) {
        char text[128];
        lv_snprintf(text, sizeof(text), "%s%s%s%s",
            runtime->queue_lost || runtime->fifo_overruns ? "接收错误  " : "",
            runtime->error_flags ? "CAN 错误  " : "",
            runtime->sd_error ? "SD 日志错误  " : "",
            runtime->logger.rejected_frames ? "SD 记录错误" : "");
        set_label_if_changed(diagnostics_label, text);
        lv_obj_set_hidden(diagnostics_label, text[0] == '\0');
    }
    // 归档结果与接收帧数无关，未收到新报文时仍须更新。
    refresh_archive(runtime);
    displayed_runtime = *runtime;
    runtime_display_valid = true;
}

// 统计刷新主体；仅修改控件，不改变 CAN 接收统计或日志记录。
static void refresh_stats_widgets(void)
{
    refresh_runtime();
    refresh_player();
    uint32_t total = CanStats_GetTotalCount();
    if(total == displayed_total) {
        // 即使没有新报文，关闭键盘后也须应用尚未显示的筛选条件。
        if(receive_filter_dirty && receive_filter.table != NULL) refresh_receive(&receive_filter);
        return;
    }
    displayed_total = total;
    lv_label_set_text_fmt(total_label, "总帧数 %lu", (unsigned long)total);
    if(CanStats_GetUntrackedCount() != 0U) {
        lv_label_set_text_fmt(capacity_label, "ID table full; not listed: %lu frames",
                             (unsigned long)CanStats_GetUntrackedCount());
    }
    // 页面切换可能已释放接收页控件，仅在控件有效时更新。
    if(receive_filter.table != NULL) refresh_receive(&receive_filter);
}

// 周期更新页面数据。
static void stats_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    refresh_stats_widgets();
}

//创建状态栏
static void create_status_bar(lv_obj_t *screen)
{
    //创建状态栏容器并设置位置和尺寸 
    lv_obj_t *bar = lv_obj_create(screen);
    lv_obj_set_clickable(bar, false);
    lv_obj_set_scrollable(bar, false);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, 800, 44);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COLOR_PANEL), LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);

    //添加状态栏各状态标签
    create_label(bar, "CanSight", 16, 12, 100, lv_color_hex(UI_COLOR_TEXT));
    can_status_label = create_label(bar, "● CAN1 未启动", 150, 13, 150,
                                   lv_color_hex(UI_COLOR_MUTED));
    create_label(bar, "1 Mbit/s", 325, 13, 110, lv_color_hex(UI_COLOR_TEXT));
    sd_status_label = create_label(bar, "● SD 未挂载", 475, 13, 150,
                                  lv_color_hex(UI_COLOR_MUTED));
    total_label = create_label(bar, "总帧数 0", 630, 13, 165, lv_color_hex(UI_COLOR_TEXT));
}

// 处理接收页分页事件。
static void receive_page_cb(lv_event_t *event)
{
    // 事件用户数据非空表示下一页，为 NULL 表示上一页。
    if(lv_event_get_user_data(event) != NULL) {
        receive_offset += RECEIVE_VISIBLE_ROWS;
    }
    else if(receive_offset >= RECEIVE_VISIBLE_ROWS) {
        receive_offset -= RECEIVE_VISIBLE_ROWS;
    }
    // 统一在刷新函数中处理分页越界。
    refresh_receive(&receive_filter);
}

//创建 CAN 接收页面的 UI 组件
static void create_receive_page(lv_obj_t *screen)
{
    receive_page = lv_obj_create(screen);
    lv_obj_set_clickable(receive_page, false);
    lv_obj_set_scrollable(receive_page, false);
    lv_obj_set_pos(receive_page, 0, 140);
    lv_obj_set_size(receive_page, 800, 340);
    lv_obj_set_style_bg_opa(receive_page, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(receive_page, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(receive_page, 0, LV_PART_MAIN);

    //筛选区域：ID 输入框、清除按钮、匹配统计
    create_label(receive_page, "ID (hex)", 16, 9, 70, lv_color_hex(UI_COLOR_MUTED));
    lv_obj_t *filter = lv_textarea_create(receive_page);
    lv_obj_set_pos(filter, 92, 0);
    lv_obj_set_size(filter, 170, 36);
    lv_textarea_set_one_line(filter, true);
    lv_textarea_set_placeholder_text(filter, "例如 0x237");
    lv_obj_set_style_bg_color(filter, lv_color_hex(0x101318), LV_PART_MAIN);
    lv_obj_set_style_text_color(filter, lv_color_hex(UI_COLOR_TEXT), LV_PART_MAIN);
    lv_obj_t *clear = create_button(receive_page, "清除", 272, 0, 68, 36, false);
    lv_obj_t *previous = create_button(receive_page, "<", 352, 0, 36, 36, false);
    lv_obj_t *next = create_button(receive_page, ">", 394, 0, 36, 36, false);
    lv_obj_add_event_cb(previous, receive_page_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(next, receive_page_cb, LV_EVENT_CLICKED, &receive_offset);
    lv_obj_t *matches = create_label(receive_page, "", 442, 9, 345,
                 lv_color_hex(UI_COLOR_MUTED));

    // 启动时预分配表头和八个显示位置，接收期间仅更新现有单元格。
    lv_obj_t *table = lv_table_create(receive_page);
    style_data_table(table);
    lv_obj_set_pos(table, 12, 48);
    lv_obj_set_size(table, 776, 280);
    lv_table_set_column_count(table, 5);
    lv_table_set_row_count(table, RECEIVE_VISIBLE_ROWS + 1U);
    lv_table_set_column_width(table, 0, 125);
    lv_table_set_column_width(table, 1, 100);
    lv_table_set_column_width(table, 2, 48);
    lv_table_set_column_width(table, 3, 380);
    lv_table_set_column_width(table, 4, 100);

    // 接收表格采用单行裁剪，文本变化不触发行高调整及整表失效。
    for(uint32_t row = 0U; row <= RECEIVE_VISIBLE_ROWS; row++) {
        for(uint32_t col = 0U; col < 5U; col++) {
            lv_table_set_cell_ctrl(table, row, col, LV_TABLE_CELL_CTRL_TEXT_CROP);
        }
    }

    const char *headers[] = {"CAN-ID", "类型", "DLC", "最新数据", "计数"};
    // 初始化列标题，数据单元格由统计刷新函数填充。
    for(uint32_t col = 0; col < 5; col++) {
        set_table_cell_if_changed(table, 0, col, headers[col]);
    }
    bind_filter(&receive_filter, filter, clear, table, matches);
}

static void replay_event_cb(lv_event_t *event)
{
    intptr_t action = (intptr_t)lv_event_get_user_data(event);
    if(action == 0) {
        close_hex_keyboard();
        if(!LogPlayer_ReplayStart()) set_history_message("Replay unavailable; wait for indexing and SD ready");
    }
    else if(action == 1) LogPlayer_ReplayPause();
    else LogPlayer_ReplayStop();
    refresh_player();
}

// 创建历史文件管理、逐帧浏览及 CAN 原速回放页面。
static void create_playback_page(lv_obj_t *screen)
{
    playback_page = lv_obj_create(screen);
    lv_obj_set_clickable(playback_page, false);
    lv_obj_set_scrollable(playback_page, false);
    lv_obj_set_pos(playback_page, 0, 140);
    lv_obj_set_size(playback_page, 800, 340);
    lv_obj_set_style_bg_opa(playback_page, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(playback_page, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(playback_page, 0, LV_PART_MAIN);
    create_label(playback_page, "SD 日志文件", 16, 0, 180, lv_color_hex(UI_COLOR_MUTED));

    lv_obj_t *panel = lv_obj_create(playback_page);
    lv_obj_set_pos(panel, 8, 26);
    lv_obj_set_size(panel, 190, 306);
    lv_obj_set_style_bg_color(panel, lv_color_hex(UI_COLOR_PANEL), LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, lv_color_hex(UI_COLOR_LINE), LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 6, LV_PART_MAIN);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    list_status_label = create_label(panel, "", 4, 0, 168, lv_color_hex(UI_COLOR_MUTED));
    lv_label_set_long_mode(list_status_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(list_status_label, 18);
    for(unsigned i = 0; i < LOG_PLAYER_PAGE_SIZE; i++) {
        file_buttons[i] = create_button(panel, "", 4, 24 + (lv_coord_t)i * 34, 168, 30, false);
        lv_obj_set_style_pad_hor(file_buttons[i], 4, LV_PART_MAIN);
        lv_obj_t *label = lv_obj_get_child(file_buttons[i], 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(label, 158);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
        lv_obj_add_event_cb(file_buttons[i], file_event_cb, LV_EVENT_CLICKED, file_button_names[i]);
    }
    list_previous = create_button(panel, "<", 4, 165, 36, 28, false);
    list_next = create_button(panel, ">", 132, 165, 36, 28, false);
    list_page_label = create_label(panel, "", 48, 171, 80, lv_color_hex(UI_COLOR_MUTED));
    lv_obj_add_event_cb(list_previous, history_page_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    lv_obj_add_event_cb(list_next, history_page_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    lv_obj_t *refresh = create_button(panel, "Refresh", 4, 201, 168, 32, false);
    lv_obj_add_event_cb(refresh, history_refresh_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *archive = create_button(panel, "Archive log", 4, 243, 168, 34, false);
    lv_obj_add_event_cb(archive, archive_event_cb, LV_EVENT_CLICKED, NULL);

    history_message[0] = '\0';
    archive_notice_valid = false;
    displayed_delete_state = LogPlayer_GetView()->delete_state;
    replay_start_button = create_button(playback_page, "Replay", 216, 0, 100, 26, false);
    replay_pause_button = create_button(playback_page, "Pause", 324, 0, 100, 26, false);
    replay_stop_button = create_button(playback_page, "Stop", 432, 0, 100, 26, false);
    lv_obj_add_event_cb(replay_start_button, replay_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)0);
    lv_obj_add_event_cb(replay_pause_button, replay_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    lv_obj_add_event_cb(replay_stop_button, replay_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)2);
    preview_status_label = create_label(playback_page, "", 216, 50, 570, lv_color_hex(UI_COLOR_MUTED));
    storage_label = create_label(playback_page, "", 216, 27, 570, lv_color_hex(UI_COLOR_MUTED));
    create_label(playback_page, "ID", 216, 87, 26, lv_color_hex(UI_COLOR_MUTED));
    lv_obj_t *filter = lv_textarea_create(playback_page);
    lv_obj_set_pos(filter, 246, 80);
    lv_obj_set_size(filter, 156, 32);
    lv_textarea_set_one_line(filter, true);
    lv_textarea_set_placeholder_text(filter, "例如 0x237");
    lv_obj_set_style_pad_ver(filter, 5, LV_PART_MAIN);
    lv_obj_set_style_bg_color(filter, lv_color_hex(0x101318), LV_PART_MAIN);
    lv_obj_set_style_text_color(filter, lv_color_hex(UI_COLOR_TEXT), LV_PART_MAIN);
    lv_obj_t *apply = create_button(playback_page, "Apply", 410, 80, 70, 32, false);
    lv_obj_t *clear = create_button(playback_page, "Clear", 488, 80, 70, 32, false);
    lv_obj_add_event_cb(apply, history_filter_apply_cb, LV_EVENT_CLICKED, NULL);
    delete_button = create_button(playback_page, "Delete file", 670, 80, 116, 32, false);
    lv_obj_add_event_cb(delete_button, history_delete_cb, LV_EVENT_CLICKED, NULL);
    frame_previous = create_button(playback_page, "Prev frame", 216, 116, 110, 30, false);
    frame_next = create_button(playback_page, "Next frame", 334, 116, 110, 30, false);
    lv_obj_add_event_cb(frame_previous, history_step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    lv_obj_add_event_cb(frame_next, history_step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    preview_count_label = create_label(playback_page, "", 454, 123, 332, lv_color_hex(UI_COLOR_MUTED));
    lv_label_set_long_mode(preview_count_label, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_long_mode(preview_status_label, LV_LABEL_LONG_MODE_DOTS);

    preview_table = lv_table_create(playback_page);
    style_data_table(preview_table);
    lv_obj_set_send_draw_task_events(preview_table, true);
    lv_obj_add_event_cb(preview_table, history_table_draw_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
    lv_obj_set_style_pad_ver(preview_table, 4, LV_PART_ITEMS);
    lv_obj_set_pos(preview_table, 204, 152);
    lv_obj_set_size(preview_table, 584, 180);
    lv_table_set_column_count(preview_table, 5);
    lv_table_set_row_count(preview_table, 1);
    // DLC 列包含左右内边距，保留足够宽度，防止表头再次折成多行。
    const uint16_t widths[] = {110, 110, 95, 48, 217};
    const char *headers[] = {"时间(s)", "CAN-ID", "类型", "DLC", "Data"};
    for(unsigned i = 0; i < 5U; i++) {
        lv_table_set_column_width(preview_table, i, widths[i]);
        set_table_cell_if_changed(preview_table, 0, i, headers[i]);
    }
    player_display_valid = false;
    bind_filter(&history_filter, filter, clear, preview_table, preview_count_label);
    refresh_archive(CanStats_GetRuntime());
    refresh_player();
}

void ui_init(void)
{
    //获取当前活动屏幕并设置背景样式 
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_clickable(screen, false);
    lv_obj_set_scrollable(screen, false);
    lv_obj_set_style_bg_color(screen, lv_color_hex(UI_COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);

    //创建顶部状态栏，显示应用名称和CAN总线状态信息 
    create_status_bar(screen);
    capacity_label = create_label(screen, "", 16, 100, 770,
                                  lv_color_hex(0xE3B34E));
    diagnostics_label = create_label(screen, "", 342, 64, 444,
                                     lv_color_hex(0xE3B34E));
    runtime_display_valid = false;
    displayed_total = UINT32_MAX;  // 保证启动时累计帧数为 0 仍执行首次刷新。

    // 创建实时接收和 SD 日志两个页面的切换按钮。
    receive_tab = create_button(screen, "接收", 12, 54, 150, 42, true);
    playback_tab = create_button(screen, "SD 日志", 172, 54, 150, 42, false);
    lv_obj_add_event_cb(receive_tab, tab_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(playback_tab, tab_event_cb, LV_EVENT_CLICKED, NULL);

    //先只创建当前显示的接收页，回放页在点击页签时再创建
    create_receive_page(screen);
    create_hex_keyboard();  // 在 CAN 启动前完成键盘分配和定位。

    //默认显示接收页面
    show_page(false);
    stats_timer_cb(NULL);  // 初始化统计显示。
    lv_timer_create(stats_timer_cb, 500, NULL);  // 注册周期刷新任务，仅在界面初始化时创建。
}
