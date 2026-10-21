#include "ui.h"
#include "can_stats.h"

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
static lv_obj_t *total_label;       /* 全局接收帧数标签。 */
static lv_obj_t *capacity_label;    /* 统计表容量不足提示。 */
static uint32_t displayed_total;    /* 上次刷新时的累计帧数。 */
static uint16_t receive_offset;     /* 当前页在筛选结果中的起始偏移。 */
#define RECEIVE_VISIBLE_ROWS 8U  /* 单页最大条目数，用于限制控件内存占用。 */

/* 回放页保留示例数据；接收页使用统计模块提供的数据。 */
typedef struct {
    uint32_t id;
    const char *cells[6];
} ui_sample_t;

static const ui_sample_t playback_samples[] = {
    {0x237, {"12.340", "0x237", "标准", "8", "12 27 65 74 83 92 87 33"}},
    {0x123, {"12.352", "0x123", "标准", "4", "01 02 03 04"}},
    {0x321, {"12.360", "0x321", "标准", "2", "A5 5A"}},
    {0x18FEF100, {"12.371", "0x18FEF100", "扩展", "8", "FF FF FF 00 12 34 56 78"}},
    {0x237, {"12.840", "0x237", "标准", "8", "12 27 65 74 83 92 87 34"}}
};

typedef struct {
    bool playback;
    lv_obj_t *input;
    lv_obj_t *table;
    lv_obj_t *matches;
    char text[11]; /* 0x + 8 位十六进制数 + 字符串结束符 */
} ui_filter_t;

static ui_filter_t receive_filter;
static ui_filter_t playback_filter = {.playback = true};
static lv_obj_t *hex_keyboard;

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

/* 根据筛选条件和分页偏移刷新接收表格，不修改底层统计。 */
static void refresh_receive(ui_filter_t *filter)
{
    uint32_t id;
    bool all;
    bool valid = parse_filter(filter->text, &id, &all);  /* 空筛选条件匹配全部条目。 */
    uint16_t selected[CAN_STATS_CAPACITY];  /* 匹配条目的索引列表，不复制报文数据。 */
    uint16_t matches = 0;  /* 匹配条目数。 */
    uint16_t count = CanStats_GetEntryCount();  /* 有效统计条目数。 */
    for(uint16_t i = 0; i < count; i++) {
        const CanStatsEntry *entry = CanStats_GetEntry(i);
        if(valid && (all || entry->latest.id == id)) selected[matches++] = i;
    }
    /* 筛选结果变化或分页偏移越界时，重置为首页。 */
    if(receive_offset >= matches) receive_offset = 0;
    uint16_t visible = matches - receive_offset;
    if(visible > RECEIVE_VISIBLE_ROWS) visible = RECEIVE_VISIBLE_ROWS;
    lv_table_set_row_count(filter->table, visible + 1U);  /* 第 0 行为表头。 */
    for(uint16_t i = 0; i < visible; i++) {
        /* 将页面偏移映射为统计表索引。 */
        const CanStatsEntry *entry = CanStats_GetEntry(selected[receive_offset + i]);
        const CanFrame *frame = &entry->latest;
        uint16_t row = i + 1U;
        char text[32];
        lv_snprintf(text, sizeof(text), "0x%lX", (unsigned long)frame->id);
        lv_table_set_cell_value(filter->table, row, 0, text);
        /* 使用 RTR 后缀区分远程帧与数据帧。 */
        lv_snprintf(text, sizeof(text), "%s%s", frame->ide ? "扩展" : "标准",
                    frame->rtr ? " RTR" : "");
        lv_table_set_cell_value(filter->table, row, 1, text);
        lv_snprintf(text, sizeof(text), "%u", (unsigned)frame->dlc);
        lv_table_set_cell_value(filter->table, row, 2, text);
        /* 8 字节数据格式化后最多为 23 个字符，不含字符串结束符。 */
        unsigned used = 0;  /* 已写入的字符数。 */
        text[0] = '\0';
        if(frame->rtr == 0U) {  /* 远程帧不包含有效数据，数据列保持为空。 */
            for(uint8_t j = 0; j < frame->dlc; j++) {
                used += (unsigned)lv_snprintf(text + used, sizeof(text) - used,
                    j ? " %02X" : "%02X", (unsigned)frame->data[j]);
            }
        }
        lv_table_set_cell_value(filter->table, row, 3, text);
        /* 使用整数运算将微秒转换为毫秒，并保留三位小数。 */
        if(entry->has_period) {
            lv_snprintf(text, sizeof(text), "%lu.%03lu",
                        (unsigned long)(entry->period_us / 1000U),
                        (unsigned long)(entry->period_us % 1000U));
        }
        else lv_snprintf(text, sizeof(text), "--");
        lv_table_set_cell_value(filter->table, row, 4, text);
        lv_snprintf(text, sizeof(text), "%lu", (unsigned long)entry->count);
        lv_table_set_cell_value(filter->table, row, 5, text);
    }
    /* 更新匹配条目数、当前显示范围及筛选输入状态。 */
    if(!valid) lv_label_set_text(filter->matches, "ID 格式错误");
    else lv_label_set_text_fmt(filter->matches, "匹配 %u / 共 %u  [%u-%u]",
        (unsigned)matches, (unsigned)count,
        matches ? (unsigned)receive_offset + 1U : 0U,
        (unsigned)(receive_offset + visible));
    lv_obj_set_style_border_color(filter->input,
        lv_color_hex(valid ? UI_COLOR_LINE : 0xE35B5B), LV_PART_MAIN);
}

static void refresh_filter(ui_filter_t *filter)
{
    if(!filter->playback) {
        refresh_receive(filter);
        return;
    }
    const ui_sample_t *samples = playback_samples;
    uint32_t count = sizeof(playback_samples) / sizeof(playback_samples[0]);
    uint32_t columns = 5;
    uint32_t id;
    bool all;
    bool valid = parse_filter(filter->text, &id, &all);
    uint32_t matches = 0;
    for(uint32_t i = 0; i < count; i++) {
        if(valid && (all || samples[i].id == id)) matches++;
    }

    lv_table_set_row_count(filter->table, matches + 1);
    uint32_t row = 1;
    for(uint32_t i = 0; i < count; i++) {
        if(!valid || (!all && samples[i].id != id)) continue;
        for(uint32_t col = 0; col < columns; col++) {
            lv_table_set_cell_value(filter->table, row, col, samples[i].cells[col]);
        }
        row++;
    }
    lv_obj_scroll_to_y(filter->table, 0, LV_ANIM_OFF);
    if(!valid) lv_label_set_text(filter->matches, "ID 格式错误");
    else {
        lv_label_set_text_fmt(filter->matches, "匹配 %u / 共 %u 帧",
                             (unsigned)matches, (unsigned)count);
    }
    lv_obj_set_style_border_color(filter->input,
                                  lv_color_hex(valid ? UI_COLOR_LINE : 0xE35B5B), LV_PART_MAIN);
}

static void close_hex_keyboard(void)
{
    if(hex_keyboard == NULL) return;
    lv_keyboard_set_textarea(hex_keyboard, NULL);
    lv_obj_set_hidden(hex_keyboard, true);
    /* 可能从键盘自身事件关闭，延后删除以免访问已释放对象。 */
    lv_obj_delete_async(hex_keyboard);
    hex_keyboard = NULL;
}

static void keyboard_done_cb(lv_event_t *event)
{
    (void)event;
    close_hex_keyboard();
}

static void filter_clicked_cb(lv_event_t *event)
{
    ui_filter_t *filter = lv_event_get_user_data(event);
    static const char * const map[] = {
        "1", "2", "3", "A", "B", "\n",
        "4", "5", "6", "C", "D", "\n",
        "7", "8", "9", "E", "F", "\n",
        LV_SYMBOL_CLOSE, "0", "0x", LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, ""
    };
    static const lv_buttonmatrix_ctrl_t controls[] = {
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1
    };
    if(hex_keyboard == NULL) {
        hex_keyboard = lv_keyboard_create(lv_layer_top());
        lv_obj_set_size(hex_keyboard, 800, 180);
        lv_obj_align(hex_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_keyboard_set_map(hex_keyboard, LV_KEYBOARD_MODE_USER_1, map, controls);
        lv_keyboard_set_mode(hex_keyboard, LV_KEYBOARD_MODE_USER_1);
        lv_keyboard_set_popovers(hex_keyboard, false);
        lv_obj_set_style_bg_color(hex_keyboard, lv_color_hex(UI_COLOR_PANEL), LV_PART_MAIN);
        lv_obj_set_style_bg_color(hex_keyboard, lv_color_hex(UI_COLOR_LINE), LV_PART_ITEMS);
        lv_obj_set_style_text_color(hex_keyboard, lv_color_hex(UI_COLOR_TEXT), LV_PART_ITEMS);
        lv_obj_set_style_text_font(hex_keyboard, &lv_font_montserrat_14, LV_PART_ITEMS);
        lv_obj_add_event_cb(hex_keyboard, keyboard_done_cb, LV_EVENT_READY, NULL);
        lv_obj_add_event_cb(hex_keyboard, keyboard_done_cb, LV_EVENT_CANCEL, NULL);
    }
    lv_keyboard_set_textarea(hex_keyboard, filter->input);
}

static void filter_changed_cb(lv_event_t *event)
{
    ui_filter_t *filter = lv_event_get_user_data(event);
    lv_snprintf(filter->text, sizeof(filter->text), "%s", lv_textarea_get_text(filter->input));
    /* 筛选条件变更后重置分页，并立即更新显示。 */
    if(!filter->playback) receive_offset = 0;
    refresh_filter(filter);
}

static void filter_clear_cb(lv_event_t *event)
{
    ui_filter_t *filter = lv_event_get_user_data(event);
    close_hex_keyboard();
    lv_textarea_set_text(filter->input, "");
    /* 带字符限制的输入框，设置空字符串不一定触发 VALUE_CHANGED。 */
    filter->text[0] = '\0';
    if(!filter->playback) receive_offset = 0;
    refresh_filter(filter);
}

static void bind_filter(ui_filter_t *filter, lv_obj_t *input, lv_obj_t *clear,
                        lv_obj_t *table, lv_obj_t *matches)
{
    filter->input = input;
    filter->table = table;
    filter->matches = matches;
    /* 输入文字和占位提示是不同的绘制部分，两处都显式指定精简中文字体。 */
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

/* 接收和回放表格共用外观，避免只修好其中一页。 */
static void style_data_table(lv_obj_t *table)
{
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
                              lv_color_t color, uint32_t font_size)
{
    /* 消除未使用参数警告 */
    (void)font_size;
    
    /* 创建标签并设置文本内容和位置 */
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    
    /* 设置标签宽度和文本颜色样式 */
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
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);

    //设置按钮样式：背景色根据选中状态决定，添加边框和圆角 
    lv_obj_set_style_bg_color(button,
                              lv_color_hex(selected ? UI_COLOR_BLUE : UI_COLOR_PANEL),
                              LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lv_color_hex(UI_COLOR_LINE), LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(button, 6, LV_PART_MAIN);

    //创建按钮文本标签并居中显示
    lv_obj_t *label = lv_label_create(button);
    lv_obj_set_style_text_font(label, &lv_font_cansight_14, LV_PART_MAIN);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_TEXT), LV_PART_MAIN);
    lv_obj_center(label);
    return button;
}

//显示指定的页面（接收页面或回放页面）
static void create_receive_page(lv_obj_t *screen);
static void create_playback_page(lv_obj_t *screen);

static void show_page(bool show_playback)
{
    close_hex_keyboard();
    /* 两页不同时驻留，避免同时占用两张表格的 LVGL 动态内存。 */
    lv_obj_t *screen = lv_screen_active();
    if(show_playback) {
        if(receive_page != NULL) {
            lv_obj_delete(receive_page);
            receive_page = NULL;
            receive_filter.input = receive_filter.table = receive_filter.matches = NULL;
        }
        if(playback_page == NULL) {
            create_playback_page(screen);
        }
    }
    else {
        if(playback_page != NULL) {
            lv_obj_delete(playback_page);
            playback_page = NULL;
            playback_filter.input = playback_filter.table = playback_filter.matches = NULL;
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
 * @brief 周期更新统计显示。
 * @note 周期为 100 ms，由主循环中的 lv_timer_handler() 调度，不在中断中执行。
 */
static void stats_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    uint32_t total = CanStats_GetTotalCount();
    if(total == displayed_total) return;  /* 统计未变化时跳过控件更新。 */
    displayed_total = total;
    lv_label_set_text_fmt(total_label, "总帧数 %lu", (unsigned long)total);
    if(CanStats_GetUntrackedCount() != 0U) {
        lv_label_set_text_fmt(capacity_label, "ID table full; not listed: %lu frames",
                             (unsigned long)CanStats_GetUntrackedCount());
    }
    /* 页面切换可能已释放接收页控件，仅在控件有效时更新。 */
    if(receive_filter.table != NULL) refresh_receive(&receive_filter);
}

//创建状态栏
static void create_status_bar(lv_obj_t *screen)
{
    //创建状态栏容器并设置位置和尺寸 
    lv_obj_t *bar = lv_obj_create(screen);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, 800, 44);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COLOR_PANEL), LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);

    //添加状态栏各状态标签
    create_label(bar, "CanSight", 16, 12, 100, lv_color_hex(UI_COLOR_TEXT), 18);
    create_label(bar, "● CAN1 运行中", 150, 13, 150, lv_color_hex(0x43D17A), 0);
    create_label(bar, "500 kbit/s", 325, 13, 110, lv_color_hex(UI_COLOR_TEXT), 0);
    create_label(bar, "● SD 记录中", 475, 13, 130, lv_color_hex(0x43D17A), 0);
    total_label = create_label(bar, "总帧数 0", 630, 13, 165, lv_color_hex(UI_COLOR_TEXT), 0);
}

/* 处理接收页分页事件。 */
static void receive_page_cb(lv_event_t *event)
{
    /* 事件用户数据非空表示下一页，为 NULL 表示上一页。 */
    if(lv_event_get_user_data(event) != NULL) {
        receive_offset += RECEIVE_VISIBLE_ROWS;
    }
    else if(receive_offset >= RECEIVE_VISIBLE_ROWS) {
        receive_offset -= RECEIVE_VISIBLE_ROWS;
    }
    /* 统一在刷新函数中处理分页越界。 */
    refresh_receive(&receive_filter);
}

//创建 CAN 接收页面的 UI 组件
static void create_receive_page(lv_obj_t *screen)
{
    receive_page = lv_obj_create(screen);
    lv_obj_set_pos(receive_page, 0, 140);
    lv_obj_set_size(receive_page, 800, 340);
    lv_obj_set_style_bg_opa(receive_page, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(receive_page, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(receive_page, 0, LV_PART_MAIN);

    //筛选区域：ID 输入框、清除按钮、匹配统计
    create_label(receive_page, "ID (hex)", 16, 9, 70, lv_color_hex(UI_COLOR_MUTED), 0);
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
                 lv_color_hex(UI_COLOR_MUTED), 0);

    /* 仅为当前页分配表格行，单页最多显示 8 条统计记录。 */
    lv_obj_t *table = lv_table_create(receive_page);
    style_data_table(table);
    lv_obj_set_pos(table, 12, 48);
    lv_obj_set_size(table, 776, 280);
    lv_table_set_column_count(table, 6);
    lv_table_set_row_count(table, 1);
    lv_table_set_column_width(table, 0, 125);
    lv_table_set_column_width(table, 1, 100);
    lv_table_set_column_width(table, 2, 48);
    lv_table_set_column_width(table, 3, 265);
    lv_table_set_column_width(table, 4, 115);
    lv_table_set_column_width(table, 5, 100);

    const char *headers[] = {"CAN-ID", "类型", "DLC", "最新数据", "周期(ms)", "计数"};
    /* 初始化列标题，数据单元格由统计刷新函数填充。 */
    for(uint32_t col = 0; col < 6; col++) {
        lv_table_set_cell_value(table, 0, col, headers[col]);
    }
    bind_filter(&receive_filter, filter, clear, table, matches);
}

//建回放页面界面
static void create_playback_page(lv_obj_t *screen)
{
    //创建回放页面主容器并设置位置和尺寸
    playback_page = lv_obj_create(screen);
    lv_obj_set_pos(playback_page, 0, 140);
    lv_obj_set_size(playback_page, 800, 340);
    lv_obj_set_style_bg_opa(playback_page, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(playback_page, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(playback_page, 0, LV_PART_MAIN);
    //左侧文件列表面板 - 显示SD卡中的CAN日志文件
    create_label(playback_page, "SD 日志文件", 16, 4, 180, lv_color_hex(UI_COLOR_MUTED), 0);
    lv_obj_t *file_panel = lv_obj_create(playback_page);
    lv_obj_set_pos(file_panel, 8, 32);
    lv_obj_set_size(file_panel, 190, 300);
    lv_obj_set_style_bg_color(file_panel, lv_color_hex(UI_COLOR_PANEL), LV_PART_MAIN);
    lv_obj_set_style_border_color(file_panel, lv_color_hex(UI_COLOR_LINE), LV_PART_MAIN);
    lv_obj_set_style_pad_all(file_panel, 6, LV_PART_MAIN);
    lv_obj_set_scroll_dir(file_panel, LV_DIR_VER);
    create_button(file_panel, "CANLOG_001.CSV", 4, 4, 168, 38, true);
    create_label(file_panel, "CANLOG_002.CSV", 12, 58, 160, lv_color_hex(UI_COLOR_TEXT), 0);
    create_label(file_panel, "CANLOG_003.CSV", 12, 98, 160, lv_color_hex(UI_COLOR_TEXT), 0);
    create_label(file_panel, "……", 12, 142, 160, lv_color_hex(UI_COLOR_MUTED), 0);
    create_label(file_panel, "CANLOG.CSV\n正在记录，暂不可回放", 8, 238, 158,
                 lv_color_hex(UI_COLOR_MUTED), 0);

    //右侧内容区域 - 文件名和帧统计信息 
    create_label(playback_page, "CANLOG_001.CSV", 216, 4, 300, lv_color_hex(UI_COLOR_TEXT), 0);
    create_label(playback_page, "第 128 帧 / 共 5230 帧", 565, 4, 220,
                 lv_color_hex(UI_COLOR_MUTED), 0);


    //进度条 - 显示当前回放位置 
    lv_obj_t *progress = lv_bar_create(playback_page);
    lv_obj_set_pos(progress, 210, 30);
    lv_obj_set_size(progress, 578, 8);
    lv_bar_set_range(progress, 0, 5230);
    lv_bar_set_value(progress, 128, LV_ANIM_OFF);

    //播放控制按钮组
    create_button(playback_page, "上一帧", 210, 49, 95, 40, false);
    create_button(playback_page, "播放/暂停", 315, 49, 120, 40, true);
    create_button(playback_page, "停止", 445, 49, 85, 40, false);
    create_button(playback_page, "下一帧", 540, 49, 95, 40, false);

    //ID过滤器输入区域
    create_label(playback_page, "ID (hex)", 210, 105, 70, lv_color_hex(UI_COLOR_MUTED), 0);
    lv_obj_t *filter = lv_textarea_create(playback_page);
    lv_obj_set_pos(filter, 282, 96);
    lv_obj_set_size(filter, 155, 36);
    lv_textarea_set_one_line(filter, true);
    lv_textarea_set_placeholder_text(filter, "例如 0x237");
    lv_obj_set_style_bg_color(filter, lv_color_hex(0x101318), LV_PART_MAIN);
    lv_obj_set_style_text_color(filter, lv_color_hex(UI_COLOR_TEXT), LV_PART_MAIN);
    lv_obj_t *clear = create_button(playback_page, "清除", 447, 96, 68, 36, false);
    lv_obj_t *matches = create_label(playback_page, "", 535, 105, 250,
                                    lv_color_hex(UI_COLOR_MUTED), 0);

    //CAN数据表格 - 显示回放帧的详细信息
    lv_obj_t *table = lv_table_create(playback_page);
    style_data_table(table);
    lv_obj_set_pos(table, 204, 142);
    lv_obj_set_size(table, 584, 190);
    lv_table_set_column_count(table, 5);
    lv_table_set_row_count(table, 6);
    lv_table_set_column_width(table, 0, 95);
    lv_table_set_column_width(table, 1, 125);
    lv_table_set_column_width(table, 2, 75);
    lv_table_set_column_width(table, 3, 55);
    lv_table_set_column_width(table, 4, 230);
    const char *headers[] = {"时间(s)", "CAN-ID", "类型", "DLC", "Data"};
    for(uint32_t col = 0; col < 5; col++) {
        lv_table_set_cell_value(table, 0, col, headers[col]);
    }
    bind_filter(&playback_filter, filter, clear, table, matches);
}

void ui_init(void)
{
    //获取当前活动屏幕并设置背景样式 
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(UI_COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);

    //创建顶部状态栏，显示应用名称和CAN总线状态信息 
    create_status_bar(screen);
    capacity_label = create_label(screen, "", 16, 110, 770,
                                  lv_color_hex(0xE3B34E), 0);
    displayed_total = UINT32_MAX;  /* 保证启动时累计帧数为 0 仍执行首次刷新。 */

    //创建"接收"和"SD 回放"两个标签页切换按钮
    receive_tab = create_button(screen, "接收", 12, 54, 150, 42, true);
    playback_tab = create_button(screen, "SD 回放", 172, 54, 150, 42, false);
    lv_obj_add_event_cb(receive_tab, tab_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(playback_tab, tab_event_cb, LV_EVENT_CLICKED, NULL);

    //先只创建当前显示的接收页，回放页在点击页签时再创建
    create_receive_page(screen);

    //默认显示接收页面
    show_page(false);
    stats_timer_cb(NULL);  /* 初始化统计显示。 */
    lv_timer_create(stats_timer_cb, 100, NULL);  /* 注册周期刷新任务，仅在界面初始化时创建。 */
}
