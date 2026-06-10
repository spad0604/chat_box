#pragma once

#include <Arduino.h>
#include <lvgl.h>

typedef void (*ui_chat_text_cb_t)(const char *text);
typedef void (*ui_chat_simple_cb_t)();
typedef void (*ui_chat_history_cb_t)(uint8_t index);
typedef void (*ui_chat_int_cb_t)(int val);

void ui_chat_init();
void ui_chat_set_send_callback(ui_chat_text_cb_t cb);
void ui_chat_set_mic_callback(ui_chat_simple_cb_t cb);
void ui_chat_set_new_chat_callback(ui_chat_simple_cb_t cb);
void ui_chat_set_open_history_callback(ui_chat_simple_cb_t cb);
void ui_chat_set_history_callback(ui_chat_history_cb_t cb);

void ui_chat_add_message(const char *role, const char *message);
void ui_chat_clear_messages();
void ui_chat_set_status(const char *status);
bool ui_chat_is_mic_recording();
void ui_chat_set_datetime(const char *time_text, const char *date_text);

// Render a list of chat sessions in the chat area. Each item is clickable and will
// call the history callback with the provided index.
void ui_chat_show_sessions(const char *const *titles, uint8_t count);


void ui_chat_show_wifi_config_screen(const char *ssid, const char *ip);

void ui_chat_set_campus_news_callback(ui_chat_simple_cb_t cb);
void ui_chat_set_campus_news_close_callback(ui_chat_simple_cb_t cb);
void ui_chat_set_campus_news_swipe_callback(ui_chat_int_cb_t cb);
void ui_chat_show_campus_news();
void ui_chat_hide_campus_news();
void ui_chat_set_campus_news_status(const char *status);
lv_color_t* ui_chat_get_news_canvas_buffer();
void ui_chat_refresh_news_canvas();
