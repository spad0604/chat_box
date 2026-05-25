#pragma once

#include <Arduino.h>
#include <lvgl.h>

typedef void (*ui_chat_text_cb_t)(const char *text);
typedef void (*ui_chat_simple_cb_t)();
typedef void (*ui_chat_history_cb_t)(uint8_t index);

void ui_chat_init();
void ui_chat_set_send_callback(ui_chat_text_cb_t cb);
void ui_chat_set_mic_callback(ui_chat_simple_cb_t cb);
void ui_chat_set_new_chat_callback(ui_chat_simple_cb_t cb);
void ui_chat_set_history_callback(ui_chat_history_cb_t cb);

void ui_chat_add_message(const char *role, const char *message);
void ui_chat_clear_messages();
void ui_chat_set_status(const char *status);
