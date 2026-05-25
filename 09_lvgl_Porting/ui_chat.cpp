#include "ui_chat.h"

static lv_obj_t *dashboard_screen;
static lv_obj_t *chat_screen;
static lv_obj_t *root;
static lv_obj_t *chat_list;
static lv_obj_t *input_ta;
static lv_obj_t *status_label;
static lv_obj_t *keyboard;

static ui_chat_text_cb_t on_send;
static ui_chat_simple_cb_t on_mic;
static ui_chat_simple_cb_t on_new_chat;
static ui_chat_history_cb_t on_history;

static lv_style_t style_root;
static lv_style_t style_dashboard;
static lv_style_t style_dashboard_title;
static lv_style_t style_start_button;
static lv_style_t style_sidebar;
static lv_style_t style_panel;
static lv_style_t style_input;
static lv_style_t style_user_bubble;
static lv_style_t style_bot_bubble;
static lv_style_t style_text_light;
static lv_style_t style_text_muted;
static lv_style_t style_button;
static lv_style_t style_button_primary;

static const char *history_items[] = {
    "ESP32 LCD7 chatbot",
    "WiFi setup",
    "Voice command",
    "Sensor monitor",
    "Home control"
};

static void init_styles()
{
    lv_style_init(&style_root);
    lv_style_set_bg_color(&style_root, lv_color_hex(0x0b0b0c));
    lv_style_set_bg_opa(&style_root, LV_OPA_COVER);
    lv_style_set_pad_all(&style_root, 0);
    lv_style_set_border_width(&style_root, 0);

    lv_style_init(&style_dashboard);
    lv_style_set_bg_color(&style_dashboard, lv_color_hex(0x0b4fa8));
    lv_style_set_bg_grad_color(&style_dashboard, lv_color_hex(0x18a7d8));
    lv_style_set_bg_grad_dir(&style_dashboard, LV_GRAD_DIR_VER);
    lv_style_set_bg_opa(&style_dashboard, LV_OPA_COVER);
    lv_style_set_pad_all(&style_dashboard, 0);
    lv_style_set_border_width(&style_dashboard, 0);

    lv_style_init(&style_dashboard_title);
    lv_style_set_text_color(&style_dashboard_title, lv_color_hex(0xffffff));
    lv_style_set_text_font(&style_dashboard_title, &lv_font_montserrat_30);

    lv_style_init(&style_start_button);
    lv_style_set_bg_color(&style_start_button, lv_color_hex(0xffffff));
    lv_style_set_bg_opa(&style_start_button, LV_OPA_COVER);
    lv_style_set_radius(&style_start_button, 22);
    lv_style_set_border_width(&style_start_button, 0);
    lv_style_set_shadow_width(&style_start_button, 24);
    lv_style_set_shadow_opa(&style_start_button, LV_OPA_30);
    lv_style_set_shadow_color(&style_start_button, lv_color_hex(0x06346f));
    lv_style_set_text_color(&style_start_button, lv_color_hex(0x0b3f8c));
    lv_style_set_pad_left(&style_start_button, 26);
    lv_style_set_pad_right(&style_start_button, 26);
    lv_style_set_pad_top(&style_start_button, 12);
    lv_style_set_pad_bottom(&style_start_button, 12);

    lv_style_init(&style_sidebar);
    lv_style_set_bg_color(&style_sidebar, lv_color_hex(0x151516));
    lv_style_set_bg_opa(&style_sidebar, LV_OPA_COVER);
    lv_style_set_pad_all(&style_sidebar, 12);
    lv_style_set_border_width(&style_sidebar, 0);

    lv_style_init(&style_panel);
    lv_style_set_bg_color(&style_panel, lv_color_hex(0x0b0b0c));
    lv_style_set_bg_opa(&style_panel, LV_OPA_COVER);
    lv_style_set_pad_all(&style_panel, 16);
    lv_style_set_border_width(&style_panel, 0);

    lv_style_init(&style_input);
    lv_style_set_bg_color(&style_input, lv_color_hex(0x242424));
    lv_style_set_bg_opa(&style_input, LV_OPA_COVER);
    lv_style_set_radius(&style_input, 18);
    lv_style_set_border_width(&style_input, 1);
    lv_style_set_border_color(&style_input, lv_color_hex(0x383838));
    lv_style_set_text_color(&style_input, lv_color_hex(0xf4f4f4));
    lv_style_set_pad_left(&style_input, 12);
    lv_style_set_pad_right(&style_input, 12);
    lv_style_set_pad_top(&style_input, 8);
    lv_style_set_pad_bottom(&style_input, 8);

    lv_style_init(&style_user_bubble);
    lv_style_set_bg_color(&style_user_bubble, lv_color_hex(0x2f2f31));
    lv_style_set_bg_opa(&style_user_bubble, LV_OPA_COVER);
    lv_style_set_radius(&style_user_bubble, 14);
    lv_style_set_border_width(&style_user_bubble, 0);
    lv_style_set_pad_all(&style_user_bubble, 10);
    lv_style_set_text_color(&style_user_bubble, lv_color_hex(0xffffff));

    lv_style_init(&style_bot_bubble);
    lv_style_set_bg_color(&style_bot_bubble, lv_color_hex(0x171717));
    lv_style_set_bg_opa(&style_bot_bubble, LV_OPA_COVER);
    lv_style_set_radius(&style_bot_bubble, 14);
    lv_style_set_border_width(&style_bot_bubble, 1);
    lv_style_set_border_color(&style_bot_bubble, lv_color_hex(0x2a2a2a));
    lv_style_set_pad_all(&style_bot_bubble, 10);
    lv_style_set_text_color(&style_bot_bubble, lv_color_hex(0xf3f3f3));

    lv_style_init(&style_text_light);
    lv_style_set_text_color(&style_text_light, lv_color_hex(0xf5f5f5));

    lv_style_init(&style_text_muted);
    lv_style_set_text_color(&style_text_muted, lv_color_hex(0xa7a7a7));

    lv_style_init(&style_button);
    lv_style_set_bg_color(&style_button, lv_color_hex(0x1f1f20));
    lv_style_set_bg_opa(&style_button, LV_OPA_COVER);
    lv_style_set_radius(&style_button, 8);
    lv_style_set_border_width(&style_button, 0);
    lv_style_set_pad_all(&style_button, 8);
    lv_style_set_text_color(&style_button, lv_color_hex(0xf1f1f1));

    lv_style_init(&style_button_primary);
    lv_style_set_bg_color(&style_button_primary, lv_color_hex(0xffffff));
    lv_style_set_bg_opa(&style_button_primary, LV_OPA_COVER);
    lv_style_set_radius(&style_button_primary, 18);
    lv_style_set_border_width(&style_button_primary, 0);
    lv_style_set_pad_all(&style_button_primary, 8);
    lv_style_set_text_color(&style_button_primary, lv_color_hex(0x111111));
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, lv_style_t *style)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_add_style(label, style, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return label;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_add_style(btn, &style_button, 0);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_height(btn, 38);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return btn;
}

static void show_chat_screen()
{
    lv_scr_load_anim(chat_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
}

static void show_dashboard_screen()
{
    if (keyboard) {
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    lv_scr_load_anim(dashboard_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
}

static void start_chat_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    show_chat_screen();
}

static void back_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    show_dashboard_screen();
}

static void keyboard_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static void input_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(keyboard, input_ta);
        lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static void send_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);

    const char *text = lv_textarea_get_text(input_ta);
    if (!text || strlen(text) == 0) {
        return;
    }

    ui_chat_add_message("user", text);
    if (on_send) {
        on_send(text);
    }
    lv_textarea_set_text(input_ta, "");
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void mic_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    ui_chat_set_status("Listening...");
    if (on_mic) {
        on_mic();
    }
}

static void new_chat_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    ui_chat_clear_messages();
    ui_chat_add_message("assistant", "Xin chao, toi co the giup gi?");
    ui_chat_set_status("New chat");
    if (on_new_chat) {
        on_new_chat();
    }
}

static void history_event_cb(lv_event_t *e)
{
    uintptr_t index = (uintptr_t)lv_event_get_user_data(e);
    ui_chat_clear_messages();
    ui_chat_add_message("assistant", "Da mo lich su chat.");
    ui_chat_set_status(history_items[index]);
    if (on_history) {
        on_history((uint8_t)index);
    }
}

static lv_obj_t *create_message_row(const char *role)
{
    lv_obj_t *row = lv_obj_create(chat_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_top(row, 4, 0);
    lv_obj_set_style_pad_bottom(row, 4, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);

    if (strcmp(role, "user") == 0) {
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    } else {
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    }

    return row;
}

void ui_chat_init()
{
    init_styles();

    dashboard_screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(dashboard_screen);
    lv_obj_add_style(dashboard_screen, &style_dashboard, 0);
    lv_obj_set_size(dashboard_screen, LV_PCT(100), LV_PCT(100));

    lv_obj_t *dashboard_title = lv_label_create(dashboard_screen);
    lv_label_set_text(dashboard_title, "ChatBot");
    lv_obj_add_style(dashboard_title, &style_dashboard_title, 0);
    lv_obj_align(dashboard_title, LV_ALIGN_TOP_MID, 0, 56);

    lv_obj_t *start_btn = lv_btn_create(dashboard_screen);
    lv_obj_remove_style_all(start_btn);
    lv_obj_add_style(start_btn, &style_start_button, 0);
    lv_obj_set_size(start_btn, 210, 58);
    lv_obj_align(start_btn, LV_ALIGN_BOTTOM_MID, 0, -42);
    lv_obj_add_event_cb(start_btn, start_chat_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *start_label = lv_label_create(start_btn);
    lv_label_set_text(start_label, "Start chat");
    lv_obj_center(start_label);

    chat_screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(chat_screen);
    lv_obj_add_style(chat_screen, &style_root, 0);
    lv_obj_set_size(chat_screen, LV_PCT(100), LV_PCT(100));

    root = lv_obj_create(chat_screen);
    lv_obj_remove_style_all(root);
    lv_obj_add_style(root, &style_root, 0);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);

    lv_obj_t *main_area = lv_obj_create(root);
    lv_obj_remove_style_all(main_area);
    lv_obj_add_style(main_area, &style_panel, 0);
    lv_obj_set_height(main_area, LV_PCT(100));
    lv_obj_set_flex_grow(main_area, 1);
    lv_obj_set_flex_flow(main_area, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *top_bar = lv_obj_create(main_area);
    lv_obj_remove_style_all(top_bar);
    lv_obj_set_width(top_bar, LV_PCT(100));
    lv_obj_set_height(top_bar, 34);
    lv_obj_set_flex_flow(top_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *back_btn = lv_btn_create(top_bar);
    lv_obj_remove_style_all(back_btn);
    lv_obj_add_style(back_btn, &style_button, 0);
    lv_obj_set_size(back_btn, 74, 32);
    lv_obj_add_event_cb(back_btn, back_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "Back");
    lv_obj_center(back_label);

    make_label(top_bar, "ChatBot", &style_text_light);
    status_label = make_label(top_bar, "Ready", &style_text_muted);

    chat_list = lv_obj_create(main_area);
    lv_obj_remove_style_all(chat_list);
    lv_obj_set_width(chat_list, LV_PCT(100));
    lv_obj_set_flex_grow(chat_list, 1);
    lv_obj_set_flex_flow(chat_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_right(chat_list, 4, 0);
    lv_obj_set_scrollbar_mode(chat_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(chat_list, LV_DIR_VER);

    lv_obj_t *input_row = lv_obj_create(main_area);
    lv_obj_remove_style_all(input_row);
    lv_obj_set_width(input_row, LV_PCT(100));
    lv_obj_set_height(input_row, 54);
    lv_obj_set_flex_flow(input_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(input_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(input_row, 8, 0);

    input_ta = lv_textarea_create(input_row);
    lv_obj_remove_style_all(input_ta);
    lv_obj_add_style(input_ta, &style_input, 0);
    lv_obj_set_height(input_ta, 42);
    lv_obj_set_flex_grow(input_ta, 1);
    lv_textarea_set_one_line(input_ta, true);
    lv_textarea_set_placeholder_text(input_ta, "Ask anything");
    lv_obj_add_event_cb(input_ta, input_event_cb, LV_EVENT_FOCUSED, NULL);

    lv_obj_t *mic_btn = lv_btn_create(input_row);
    lv_obj_remove_style_all(mic_btn);
    lv_obj_add_style(mic_btn, &style_button, 0);
    lv_obj_set_size(mic_btn, 54, 42);
    lv_obj_add_event_cb(mic_btn, mic_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *mic_label = lv_label_create(mic_btn);
    lv_label_set_text(mic_label, "Mic");
    lv_obj_center(mic_label);

    lv_obj_t *send_btn = lv_btn_create(input_row);
    lv_obj_remove_style_all(send_btn);
    lv_obj_add_style(send_btn, &style_button_primary, 0);
    lv_obj_set_size(send_btn, 62, 42);
    lv_obj_add_event_cb(send_btn, send_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *send_label = lv_label_create(send_btn);
    lv_label_set_text(send_label, "Send");
    lv_obj_center(send_label);

    lv_obj_t *sidebar = lv_obj_create(root);
    lv_obj_remove_style_all(sidebar);
    lv_obj_add_style(sidebar, &style_sidebar, 0);
    lv_obj_set_size(sidebar, 220, LV_PCT(100));
    lv_obj_set_flex_flow(sidebar, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(sidebar, 8, 0);

    make_label(sidebar, "Chats", &style_text_light);
    make_button(sidebar, "+ New chat", new_chat_event_cb, NULL);

    lv_obj_t *history_title = make_label(sidebar, "History", &style_text_muted);
    lv_obj_set_style_pad_top(history_title, 10, 0);

    for (uint8_t i = 0; i < sizeof(history_items) / sizeof(history_items[0]); i++) {
        make_button(sidebar, history_items[i], history_event_cb, (void *)(uintptr_t)i);
    }

    keyboard = lv_keyboard_create(chat_screen);
    lv_obj_set_size(keyboard, LV_PCT(100), 170);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(keyboard, input_ta);
    lv_obj_add_event_cb(keyboard, keyboard_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);

    ui_chat_add_message("assistant", "Xin chao, toi co the giup gi?");
    lv_scr_load(dashboard_screen);
}

void ui_chat_set_send_callback(ui_chat_text_cb_t cb)
{
    on_send = cb;
}

void ui_chat_set_mic_callback(ui_chat_simple_cb_t cb)
{
    on_mic = cb;
}

void ui_chat_set_new_chat_callback(ui_chat_simple_cb_t cb)
{
    on_new_chat = cb;
}

void ui_chat_set_history_callback(ui_chat_history_cb_t cb)
{
    on_history = cb;
}

void ui_chat_add_message(const char *role, const char *message)
{
    if (!chat_list || !message) {
        return;
    }

    lv_obj_t *row = create_message_row(role);
    lv_obj_t *bubble = lv_obj_create(row);
    lv_obj_remove_style_all(bubble);
    lv_obj_add_style(bubble, strcmp(role, "user") == 0 ? &style_user_bubble : &style_bot_bubble, 0);
    lv_obj_set_width(bubble, LV_PCT(72));
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);

    lv_obj_t *label = lv_label_create(bubble);
    lv_label_set_text(label, message);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));

    lv_obj_scroll_to_view(row, LV_ANIM_ON);
}

void ui_chat_clear_messages()
{
    if (chat_list) {
        lv_obj_clean(chat_list);
    }
}

void ui_chat_set_status(const char *status)
{
    if (status_label && status) {
        lv_label_set_text(status_label, status);
    }
}
