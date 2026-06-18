#include "ui_chat.h"

#include "font_vietnamese_16.h"
#include "logo/logo.c"

#include <string.h>

static lv_obj_t *dashboard_screen;
static lv_obj_t *chat_screen;
static lv_obj_t *chat_list;
static lv_obj_t *input_bar;
static lv_obj_t *input_ta;
static lv_obj_t *status_label;
static lv_obj_t *dashboard_time_label;
static lv_obj_t *dashboard_date_label;
static lv_obj_t *dashboard_wifi_icon;
static lv_obj_t *typing_row;
static lv_obj_t *typing_label;
static lv_obj_t *keyboard;
static lv_obj_t *voice_icon_box;
static lv_obj_t *chat_mic_btn;
static lv_obj_t *dashboard_mic_btn;
static lv_obj_t *dashboard_mic_ring;
static lv_obj_t *dashboard_mic_hint;
static lv_obj_t *dashboard_mic_wave;
static lv_obj_t *dashboard_wave_bars[4];
static lv_timer_t *dashboard_wave_timer;
static bool mic_is_recording;

static ui_chat_text_cb_t on_send;
static ui_chat_simple_cb_t on_mic;
static ui_chat_simple_cb_t on_new_chat;
static ui_chat_simple_cb_t on_open_history = NULL;
static ui_chat_history_cb_t on_history = NULL;
static ui_chat_simple_cb_t on_campus_news = NULL;
static ui_chat_simple_cb_t on_campus_news_close = NULL;
static ui_chat_int_cb_t on_campus_news_swipe = NULL;

static lv_style_t style_screen;
static lv_style_t style_text_dark;
static lv_style_t style_text_muted;
static lv_style_t style_title;
static lv_style_t style_card;
static lv_style_t style_pill;
static lv_style_t style_icon_orange;
static lv_style_t style_icon_blue;
static lv_style_t style_icon_green;
static lv_style_t style_icon_red;
static lv_style_t style_mic_recording;
static lv_style_t style_mic_recording_ring;
static lv_style_t style_user_bubble;
static lv_style_t style_bot_bubble;
static lv_style_t style_input;
static lv_style_t style_send_button;
static lv_style_t style_close_button;

static void append_utf8(char *out, size_t out_size, size_t *pos, uint32_t cp)
{
    if (*pos + 1 >= out_size) return;

    if (cp <= 0x7f) {
        out[(*pos)++] = (char)cp;
    } else if (cp <= 0x7ff && *pos + 2 < out_size) {
        out[(*pos)++] = (char)(0xc0 | (cp >> 6));
        out[(*pos)++] = (char)(0x80 | (cp & 0x3f));
    } else if (cp <= 0xffff && *pos + 3 < out_size) {
        out[(*pos)++] = (char)(0xe0 | (cp >> 12));
        out[(*pos)++] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[(*pos)++] = (char)(0x80 | (cp & 0x3f));
    } else if (cp <= 0x10ffff && *pos + 4 < out_size) {
        out[(*pos)++] = (char)(0xf0 | (cp >> 18));
        out[(*pos)++] = (char)(0x80 | ((cp >> 12) & 0x3f));
        out[(*pos)++] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[(*pos)++] = (char)(0x80 | (cp & 0x3f));
    }
}

static void append_space(char *out, size_t out_size, size_t *pos)
{
    if (*pos == 0 || out[*pos - 1] == ' ') return;
    append_utf8(out, out_size, pos, ' ');
}

static bool decode_utf8_char(const char *s, size_t len, size_t *i, uint32_t *cp)
{
    uint8_t c = (uint8_t)s[*i];
    if (c < 0x80) {
        *cp = c;
        (*i)++;
        return true;
    }

    uint8_t need = 0;
    uint32_t value = 0;
    if ((c & 0xe0) == 0xc0) {
        need = 1;
        value = c & 0x1f;
        if (value == 0) return false;
    } else if ((c & 0xf0) == 0xe0) {
        need = 2;
        value = c & 0x0f;
    } else if ((c & 0xf8) == 0xf0) {
        need = 3;
        value = c & 0x07;
    } else {
        return false;
    }

    if (*i + need >= len) return false;
    for (uint8_t j = 1; j <= need; j++) {
        uint8_t cc = (uint8_t)s[*i + j];
        if ((cc & 0xc0) != 0x80) return false;
        value = (value << 6) | (cc & 0x3f);
    }

    *i += need + 1;
    *cp = value;
    return true;
}

static bool is_supported_text_cp(uint32_t cp)
{
    if (cp >= 0x20 && cp <= 0x7e) return true;
    if (cp >= 0x00c0 && cp <= 0x1ef9) return true; // Vietnamese Latin ranges in font_vietnamese_16.
    return false;
}

static void sanitize_display_text(const char *input, char *out, size_t out_size)
{
    if (out_size == 0) return;
    size_t pos = 0;
    size_t len = input ? strlen(input) : 0;

    for (size_t i = 0; i < len && pos + 1 < out_size;) {
        size_t before = i;
        uint32_t cp = 0;
        if (!decode_utf8_char(input, len, &i, &cp)) {
            i = before + 1;
            append_space(out, out_size, &pos);
            continue;
        }

        if (cp == '\n' || cp == '\r' || cp == '\t' || cp == 0x00a0) {
            append_space(out, out_size, &pos);
        } else if (cp == '`' || cp == '*' || cp == '#' || cp == '_' ||
                   cp == '~' || cp == '>' || cp == '[' || cp == ']') {
            continue;
        } else if (cp == 0x2018 || cp == 0x2019) {
            append_utf8(out, out_size, &pos, '\'');
        } else if (cp == 0x201c || cp == 0x201d) {
            append_utf8(out, out_size, &pos, '"');
        } else if (cp == 0x2010 || cp == 0x2011 || cp == 0x2012 || cp == 0x2013 || cp == 0x2014) {
            append_utf8(out, out_size, &pos, '-');
        } else if (cp == 0x2026) {
            append_utf8(out, out_size, &pos, '.');
            append_utf8(out, out_size, &pos, '.');
            append_utf8(out, out_size, &pos, '.');
        } else if (cp == 0x2022) {
            append_utf8(out, out_size, &pos, '-');
            append_space(out, out_size, &pos);
        } else if (is_supported_text_cp(cp)) {
            append_utf8(out, out_size, &pos, cp);
        } else {
            append_space(out, out_size, &pos);
        }
    }

    while (pos > 0 && out[pos - 1] == ' ') pos--;
    out[pos] = '\0';
}

static int16_t disp_w()
{
    return lv_disp_get_hor_res(NULL);
}

static int16_t disp_h()
{
    return lv_disp_get_ver_res(NULL);
}

static int16_t sx(int16_t value)
{
    int32_t scaled = (int32_t)value * disp_w() / 1280;
    if (scaled < 1) {
        scaled = 1;
    }
    return (int16_t)scaled;
}

static int16_t sy(int16_t value)
{
    int32_t scaled = (int32_t)value * disp_h() / 768;
    if (scaled < 1) {
        scaled = 1;
    }
    return (int16_t)scaled;
}

static int16_t at_least(int16_t value, int16_t minimum)
{
    return value < minimum ? minimum : value;
}

static void init_styles()
{
    lv_style_init(&style_screen);
    lv_style_set_bg_color(&style_screen, lv_color_hex(0xf2fbff));
    lv_style_set_bg_opa(&style_screen, LV_OPA_COVER);
    lv_style_set_pad_all(&style_screen, 0);
    lv_style_set_border_width(&style_screen, 0);

    lv_style_init(&style_text_dark);
    lv_style_set_text_color(&style_text_dark, lv_color_hex(0x07142d));

    lv_style_init(&style_text_muted);
    lv_style_set_text_color(&style_text_muted, lv_color_hex(0x2f6598));
    lv_style_set_text_font(&style_text_muted, &font_vietnamese_16);

    lv_style_init(&style_title);
    lv_style_set_text_color(&style_title, lv_color_hex(0x06142c));
    lv_style_set_text_font(&style_title, &lv_font_montserrat_30);

    lv_style_init(&style_card);
    lv_style_set_bg_color(&style_card, lv_color_hex(0xffffff));
    lv_style_set_bg_opa(&style_card, LV_OPA_COVER);
    lv_style_set_radius(&style_card, 16);
    lv_style_set_border_width(&style_card, 1);
    lv_style_set_border_color(&style_card, lv_color_hex(0xe8eef5));
    lv_style_set_pad_all(&style_card, 8);

    lv_style_init(&style_pill);
    lv_style_set_bg_color(&style_pill, lv_color_hex(0xffffff));
    lv_style_set_bg_opa(&style_pill, LV_OPA_COVER);
    lv_style_set_radius(&style_pill, 18);
    lv_style_set_border_width(&style_pill, 1);
    lv_style_set_border_color(&style_pill, lv_color_hex(0xffc6a8));
    lv_style_set_pad_left(&style_pill, 10);
    lv_style_set_pad_right(&style_pill, 10);
    lv_style_set_pad_top(&style_pill, 6);
    lv_style_set_pad_bottom(&style_pill, 6);
    lv_style_set_text_color(&style_pill, lv_color_hex(0xe95100));

    lv_style_init(&style_icon_orange);
    lv_style_set_bg_color(&style_icon_orange, lv_color_hex(0xffe5c9));
    lv_style_set_bg_opa(&style_icon_orange, LV_OPA_COVER);
    lv_style_set_radius(&style_icon_orange, 18);
    lv_style_set_text_color(&style_icon_orange, lv_color_hex(0xf05c00));

    lv_style_init(&style_icon_blue);
    lv_style_set_bg_color(&style_icon_blue, lv_color_hex(0xcceeff));
    lv_style_set_bg_opa(&style_icon_blue, LV_OPA_COVER);
    lv_style_set_radius(&style_icon_blue, 18);
    lv_style_set_text_color(&style_icon_blue, lv_color_hex(0x0089bf));

    lv_style_init(&style_icon_green);
    lv_style_set_bg_color(&style_icon_green, lv_color_hex(0xd9f5dc));
    lv_style_set_bg_opa(&style_icon_green, LV_OPA_COVER);
    lv_style_set_radius(&style_icon_green, 18);
    lv_style_set_text_color(&style_icon_green, lv_color_hex(0x0a8a25));

    lv_style_init(&style_icon_red);
    lv_style_set_bg_color(&style_icon_red, lv_color_hex(0xffd5d5));
    lv_style_set_bg_opa(&style_icon_red, LV_OPA_COVER);
    lv_style_set_radius(&style_icon_red, 18);
    lv_style_set_text_color(&style_icon_red, lv_color_hex(0xe53935));

    lv_style_init(&style_mic_recording);
    lv_style_set_bg_color(&style_mic_recording, lv_color_hex(0x0aa7cf));
    lv_style_set_bg_opa(&style_mic_recording, LV_OPA_COVER);
    lv_style_set_radius(&style_mic_recording, LV_RADIUS_CIRCLE);
    lv_style_set_border_width(&style_mic_recording, 1);
    lv_style_set_border_color(&style_mic_recording, lv_color_hex(0x0785aa));
    lv_style_set_text_color(&style_mic_recording, lv_color_hex(0xffffff));

    lv_style_init(&style_mic_recording_ring);
    lv_style_set_bg_color(&style_mic_recording_ring, lv_color_hex(0xb8f0ff));
    lv_style_set_bg_opa(&style_mic_recording_ring, LV_OPA_60);
    lv_style_set_radius(&style_mic_recording_ring, LV_RADIUS_CIRCLE);
    lv_style_set_border_width(&style_mic_recording_ring, 2);
    lv_style_set_border_color(&style_mic_recording_ring, lv_color_hex(0x8ce3f7));

    lv_style_init(&style_user_bubble);
    lv_style_set_bg_color(&style_user_bubble, lv_color_hex(0xff7417));
    lv_style_set_bg_opa(&style_user_bubble, LV_OPA_COVER);
    lv_style_set_radius(&style_user_bubble, 16);
    lv_style_set_border_width(&style_user_bubble, 1);
    lv_style_set_border_color(&style_user_bubble, lv_color_hex(0xe95d00));
    lv_style_set_pad_left(&style_user_bubble, 18);
    lv_style_set_pad_right(&style_user_bubble, 18);
    lv_style_set_pad_top(&style_user_bubble, 12);
    lv_style_set_pad_bottom(&style_user_bubble, 12);
    lv_style_set_text_color(&style_user_bubble, lv_color_hex(0xffffff));
    lv_style_set_text_font(&style_user_bubble, &font_vietnamese_16);

    lv_style_init(&style_bot_bubble);
    lv_style_set_bg_color(&style_bot_bubble, lv_color_hex(0xffffff));
    lv_style_set_bg_opa(&style_bot_bubble, LV_OPA_COVER);
    lv_style_set_radius(&style_bot_bubble, 16);
    lv_style_set_border_width(&style_bot_bubble, 1);
    lv_style_set_border_color(&style_bot_bubble, lv_color_hex(0xe8eef5));
    lv_style_set_pad_left(&style_bot_bubble, 18);
    lv_style_set_pad_right(&style_bot_bubble, 18);
    lv_style_set_pad_top(&style_bot_bubble, 12);
    lv_style_set_pad_bottom(&style_bot_bubble, 12);
    lv_style_set_text_color(&style_bot_bubble, lv_color_hex(0x07142d));
    lv_style_set_text_font(&style_bot_bubble, &font_vietnamese_16);

    lv_style_init(&style_input);
    lv_style_set_bg_color(&style_input, lv_color_hex(0xffffff));
    lv_style_set_bg_opa(&style_input, LV_OPA_COVER);
    lv_style_set_radius(&style_input, 18);
    lv_style_set_border_width(&style_input, 1);
    lv_style_set_border_color(&style_input, lv_color_hex(0xe0e5ea));
    lv_style_set_text_color(&style_input, lv_color_hex(0x07142d));
    lv_style_set_text_font(&style_input, &font_vietnamese_16);
    lv_style_set_pad_left(&style_input, 18);
    lv_style_set_pad_right(&style_input, 18);
    lv_style_set_pad_top(&style_input, 11);
    lv_style_set_pad_bottom(&style_input, 8);

    lv_style_init(&style_send_button);
    lv_style_set_bg_color(&style_send_button, lv_color_hex(0xff7417));
    lv_style_set_bg_opa(&style_send_button, LV_OPA_COVER);
    lv_style_set_radius(&style_send_button, 18);
    lv_style_set_border_width(&style_send_button, 1);
    lv_style_set_border_color(&style_send_button, lv_color_hex(0xf36b08));
    lv_style_set_text_color(&style_send_button, lv_color_hex(0xffffff));

    lv_style_init(&style_close_button);
    lv_style_set_bg_color(&style_close_button, lv_color_hex(0xe8edf3));
    lv_style_set_bg_opa(&style_close_button, LV_OPA_COVER);
    lv_style_set_radius(&style_close_button, LV_RADIUS_CIRCLE);
    lv_style_set_border_width(&style_close_button, 0);
    lv_style_set_text_color(&style_close_button, lv_color_hex(0x07142d));
}

static lv_obj_t *plain_obj(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    return obj;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, lv_style_t *style)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    if (style) {
        lv_obj_add_style(label, style, 0);
    }
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return label;
}

static lv_obj_t *create_message_row(const char *role);

static void scroll_chat_to_bottom(lv_anim_enable_t anim)
{
    if (!chat_list) {
        return;
    }

    uint32_t child_count = lv_obj_get_child_cnt(chat_list);
    if (child_count == 0) {
        return;
    }

    lv_obj_t *last_child = lv_obj_get_child(chat_list, child_count - 1);
    if (last_child) {
        lv_obj_scroll_to_view(last_child, anim);
    }
}

static void set_typing_indicator(bool show, const char *text)
{
    if (!typing_row || !chat_list) {
        return;
    }

    if (show) {
        if (typing_label && text) {
            lv_label_set_text(typing_label, text);
        }
        uint32_t count = lv_obj_get_child_cnt(chat_list);
        lv_obj_move_to_index(typing_row, count == 0 ? 0 : count - 1);
        lv_obj_clear_flag(typing_row, LV_OBJ_FLAG_HIDDEN);
        scroll_chat_to_bottom(LV_ANIM_OFF);
    } else {
        lv_obj_add_flag(typing_row, LV_OBJ_FLAG_HIDDEN);
    }
}

static void create_typing_indicator()
{
    typing_row = create_message_row("assistant");
    typing_label = NULL;
    if (!typing_row) {
        return;
    }

    lv_obj_t *bubble = plain_obj(typing_row);
    lv_obj_add_style(bubble, &style_bot_bubble, 0);
    lv_obj_set_width(bubble, LV_PCT(28));
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);

    typing_label = lv_label_create(bubble);
    lv_label_set_text(typing_label, "Thinking...");
    lv_label_set_long_mode(typing_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(typing_label, LV_PCT(100));
    lv_obj_set_style_text_font(typing_label, &font_vietnamese_16, 0);
    lv_obj_add_flag(typing_row, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *make_circle(lv_obj_t *parent, int16_t size, lv_color_t color, lv_opa_t opa)
{
    lv_obj_t *circle = plain_obj(parent);
    lv_obj_set_size(circle, size, size);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(circle, color, 0);
    lv_obj_set_style_bg_opa(circle, opa, 0);
    lv_obj_set_style_border_width(circle, 0, 0);
    return circle;
}

static lv_obj_t *make_unimate_logo(lv_obj_t *parent, int16_t size, bool framed)
{
    lv_obj_t *box = plain_obj(parent);
    lv_obj_set_size(box, size, size);
    lv_obj_set_style_bg_opa(box, framed ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_radius(box, framed ? LV_RADIUS_CIRCLE : 0, 0);
    lv_obj_set_style_border_width(box, framed ? 1 : 0, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0xd6e8f8), 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *img = lv_img_create(box);
    lv_img_set_src(img, &unimate_logo);
    int32_t zoom = (int32_t)size * 256 / 545;
    if (zoom < 1) {
        zoom = 1;
    }
    lv_img_set_zoom(img, (uint16_t)zoom);
    lv_obj_center(img);
    return box;
}

static void make_mic_icon(lv_obj_t *parent, lv_color_t color)
{
    lv_obj_t *capsule = plain_obj(parent);
    lv_obj_set_size(capsule, LV_PCT(28), LV_PCT(46));
    lv_obj_align(capsule, LV_ALIGN_CENTER, 0, -4);
    lv_obj_set_style_radius(capsule, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(capsule, color, 0);
    lv_obj_set_style_bg_opa(capsule, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(capsule, 0, 0);

    lv_obj_t *slot = plain_obj(parent);
    lv_obj_set_size(slot, LV_PCT(48), LV_PCT(42));
    lv_obj_align(slot, LV_ALIGN_CENTER, 0, 2);
    lv_obj_set_style_bg_opa(slot, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(slot, 3, 0);
    lv_obj_set_style_border_color(slot, color, 0);
    lv_obj_set_style_border_side(slot, LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_RIGHT | LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(slot, LV_RADIUS_CIRCLE, 0);

    lv_obj_t *stem = plain_obj(parent);
    lv_obj_set_size(stem, LV_PCT(8), LV_PCT(24));
    lv_obj_align(stem, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_color(stem, color, 0);
    lv_obj_set_style_bg_opa(stem, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(stem, LV_RADIUS_CIRCLE, 0);

    lv_obj_t *base = plain_obj(parent);
    lv_obj_set_size(base, LV_PCT(38), LV_PCT(8));
    lv_obj_align(base, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_bg_color(base, color, 0);
    lv_obj_set_style_bg_opa(base, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(base, LV_RADIUS_CIRCLE, 0);
}

static void make_muted_mic_icon(lv_obj_t *parent, lv_color_t color)
{
    make_mic_icon(parent, color);

    lv_obj_t *slash = plain_obj(parent);
    lv_obj_set_size(slash, LV_PCT(58), LV_PCT(8));
    lv_obj_align(slash, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(slash, color, 0);
    lv_obj_set_style_bg_opa(slash, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(slash, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_transform_angle(slash, 450, 0);
}

static lv_obj_t *make_action_card(lv_obj_t *parent, const char *title, const char *icon_text, lv_style_t *icon_style, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *card = lv_btn_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_add_style(card, &style_card, 0);
    lv_obj_set_size(card, sx(470), sy(168));
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, 14, 0);
    lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *icon_box = plain_obj(card);
    lv_obj_add_style(icon_box, icon_style, 0);
    lv_obj_set_size(icon_box, at_least(sx(76), 54), at_least(sy(76), 54));

    if (icon_text) {
        lv_obj_t *icon = lv_label_create(icon_box);
        lv_label_set_text(icon, icon_text);
        lv_obj_center(icon);
    } else {
        voice_icon_box = icon_box;
        make_mic_icon(icon_box, lv_color_hex(0x0089bf));
    }

    lv_obj_t *label = make_label(card, title, &style_text_dark);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    return card;
}

static void draw_dashboard_background(lv_obj_t *screen)
{
    lv_obj_t *left = make_circle(screen, sx(470), lv_color_hex(0xd7ecfb), LV_OPA_70);
    lv_obj_align(left, LV_ALIGN_TOP_LEFT, -sx(250), -sy(88));

    lv_obj_t *right_top = make_circle(screen, sx(375), lv_color_hex(0xe8ecef), LV_OPA_COVER);
    lv_obj_align(right_top, LV_ALIGN_TOP_RIGHT, sx(110), -sy(185));

    lv_obj_t *right_bottom = make_circle(screen, sx(430), lv_color_hex(0xe8f6e8), LV_OPA_COVER);
    lv_obj_align(right_bottom, LV_ALIGN_BOTTOM_RIGHT, sx(160), sy(95));

    lv_obj_t *left_bottom = make_circle(screen, sx(360), lv_color_hex(0xd7ecfb), LV_OPA_40);
    lv_obj_align(left_bottom, LV_ALIGN_BOTTOM_LEFT, -sx(120), sy(110));

    for (uint8_t i = 0; i < 8; i++) {
        static const int16_t xs[] = {88, 190, 602, 680, 704, 1102, 1178, 1028};
        static const int16_t ys[] = {381, 131, 58, 41, 75, 169, 381, 618};
        static const uint32_t colors[] = {0x85dcff, 0xe8c49d, 0xe8c49d, 0x97d7b0, 0x8fd8f5, 0x84c99b, 0xe8c49d, 0x84c99b};
        lv_obj_t *dot = make_circle(screen, sx(i == 0 ? 11 : 7), lv_color_hex(colors[i]), LV_OPA_COVER);
        lv_obj_align(dot, LV_ALIGN_TOP_LEFT, sx(xs[i]), sy(ys[i]));
    }

    lv_obj_t *horizon = plain_obj(screen);
    lv_obj_set_size(horizon, LV_PCT(100), 2);
    lv_obj_align(horizon, LV_ALIGN_BOTTOM_MID, 0, -sy(168));
    lv_obj_set_style_bg_color(horizon, lv_color_hex(0xcfe5f4), 0);
    lv_obj_set_style_bg_opa(horizon, LV_OPA_50, 0);
}

static void dashboard_wave_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    static uint8_t phase = 0;
    static const int16_t frames[][4] = {
        {16, 8, 12, 6},
        {9, 16, 7, 13},
        {6, 11, 16, 8},
        {13, 7, 10, 16},
    };

    for (uint8_t i = 0; i < 4; i++) {
        if (dashboard_wave_bars[i]) {
            lv_obj_set_height(dashboard_wave_bars[i], sy(frames[phase][i]));
        }
    }
    phase = (phase + 1) % 4;
}

static void show_chat_screen()
{
    if (chat_screen) {
        lv_scr_load_anim(chat_screen, LV_SCR_LOAD_ANIM_FADE_ON, 140, 0, false);
    }
}

static void show_dashboard_screen()
{
    if (keyboard) {
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    if (input_bar) {
        lv_obj_align(input_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
    if (chat_list) {
        lv_obj_set_height(chat_list, disp_h() - sy(182));
    }
    if (dashboard_screen) {
        lv_scr_load_anim(dashboard_screen, LV_SCR_LOAD_ANIM_FADE_ON, 140, 0, false);
    }
}

static void update_mic_recording_ui()
{
    if (voice_icon_box) {
        lv_obj_clean(voice_icon_box);
        lv_obj_set_style_bg_color(voice_icon_box, lv_color_hex(mic_is_recording ? 0xb8f0ff : 0xcceeff), 0);
        make_mic_icon(voice_icon_box, lv_color_hex(mic_is_recording ? 0x0089bf : 0x0089bf));
    }

    if (dashboard_mic_ring) {
        if (mic_is_recording) {
            lv_obj_add_style(dashboard_mic_ring, &style_mic_recording_ring, 0);
        } else {
            lv_obj_remove_style(dashboard_mic_ring, &style_mic_recording_ring, 0);
        }
    }

    if (dashboard_mic_btn) {
        lv_obj_clean(dashboard_mic_btn);
        if (mic_is_recording) {
            lv_obj_remove_style(dashboard_mic_btn, &style_send_button, 0);
            lv_obj_add_style(dashboard_mic_btn, &style_mic_recording, 0);
            make_muted_mic_icon(dashboard_mic_btn, lv_color_hex(0xffffff));
        } else {
            lv_obj_remove_style(dashboard_mic_btn, &style_mic_recording, 0);
            lv_obj_add_style(dashboard_mic_btn, &style_send_button, 0);
            make_mic_icon(dashboard_mic_btn, lv_color_hex(0xffffff));
        }
    }

    if (dashboard_mic_hint) {
        lv_label_set_text(dashboard_mic_hint, mic_is_recording ? "Đang nghe..." : "Nhấn để nói");
    }

    if (dashboard_mic_wave) {
        if (mic_is_recording) {
            lv_obj_clear_flag(dashboard_mic_wave, LV_OBJ_FLAG_HIDDEN);
            if (dashboard_wave_timer) {
                lv_timer_resume(dashboard_wave_timer);
            }
        } else {
            lv_obj_add_flag(dashboard_mic_wave, LV_OBJ_FLAG_HIDDEN);
            if (dashboard_wave_timer) {
                lv_timer_pause(dashboard_wave_timer);
            }
        }
    }

    if (chat_mic_btn) {
        lv_obj_clean(chat_mic_btn);
        if (mic_is_recording) {
            lv_obj_remove_style(chat_mic_btn, &style_send_button, 0);
            lv_obj_add_style(chat_mic_btn, &style_mic_recording, 0);
        } else {
            lv_obj_remove_style(chat_mic_btn, &style_mic_recording, 0);
            lv_obj_add_style(chat_mic_btn, &style_send_button, 0);
        }
        make_mic_icon(chat_mic_btn, lv_color_hex(0xffffff));
    }
}

static void back_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (mic_is_recording) {
        mic_is_recording = false;
        update_mic_recording_ui();
        ui_chat_set_status("Online");
        if (on_mic) {
            on_mic();
        }
    }
    show_dashboard_screen();
}

static void keyboard_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        if (input_bar) {
            lv_obj_align(input_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
        }
        if (chat_list) {
            lv_obj_set_height(chat_list, disp_h() - sy(182));
            scroll_chat_to_bottom(LV_ANIM_OFF);
        }
    }
}

static void input_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(keyboard, input_ta);
        lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_coord_t keyboard_h = lv_obj_get_height(keyboard);
        lv_coord_t input_h = input_bar ? lv_obj_get_height(input_bar) : sy(92);
        if (input_bar) {
            lv_obj_align(input_bar, LV_ALIGN_BOTTOM_MID, 0, -keyboard_h);
        }
        if (chat_list) {
            lv_obj_set_height(chat_list, disp_h() - sy(90) - input_h - keyboard_h);
            scroll_chat_to_bottom(LV_ANIM_OFF);
        }
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
    if (input_bar) {
        lv_obj_align(input_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
    if (chat_list) {
        lv_obj_set_height(chat_list, disp_h() - sy(182));
        scroll_chat_to_bottom(LV_ANIM_OFF);
    }
}

static void mic_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    mic_is_recording = !mic_is_recording;
    update_mic_recording_ui();
    if (on_mic) {
        on_mic();
    }
    ui_chat_set_status(mic_is_recording ? "Listening..." : "Online");
}

static void new_chat_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    ui_chat_clear_messages();
    ui_chat_add_message("assistant", "Hi! I'm UniMate. How can I help you today?");
    ui_chat_set_status("Online");
    show_chat_screen();
    if (on_new_chat) {
        on_new_chat();
    }
}

static void campus_news_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (on_campus_news) {
        on_campus_news();
    }
}

static void history_open_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    ui_chat_clear_messages();
    ui_chat_add_message("assistant", "Loading chat history...");
    ui_chat_set_status("History");
    show_chat_screen();
    if (on_open_history) {
        on_open_history();
    }
}

static void session_item_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    uintptr_t index = (uintptr_t)lv_event_get_user_data(e);
    if (on_history) {
        on_history((uint8_t)index);
    }
}

static lv_obj_t *create_message_row(const char *role)
{
    lv_obj_t *row = plain_obj(chat_list);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_top(row, 7, 0);
    lv_obj_set_style_pad_bottom(row, 7, 0);

    if (strcmp(role, "user") == 0) {
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    } else {
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    }

    return row;
}

static void create_dashboard()
{
    dashboard_screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(dashboard_screen);
    lv_obj_add_style(dashboard_screen, &style_screen, 0);
    lv_obj_set_size(dashboard_screen, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(dashboard_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(dashboard_screen, LV_SCROLLBAR_MODE_OFF);
    draw_dashboard_background(dashboard_screen);

    lv_obj_t *logo = make_unimate_logo(dashboard_screen, at_least(sx(40), 36), true);
    lv_obj_align(logo, LV_ALIGN_TOP_LEFT, sx(42), sy(28));

    lv_obj_t *brand = make_label(dashboard_screen, "UniMate", &style_text_dark);
    lv_obj_set_style_text_font(brand, &lv_font_montserrat_14, 0);
    lv_obj_align_to(brand, logo, LV_ALIGN_OUT_RIGHT_MID, sx(12), 0);

    lv_obj_t *school = plain_obj(dashboard_screen);
    lv_obj_add_style(school, &style_pill, 0);
    lv_obj_set_size(school, sx(104), sy(29));
    lv_obj_align_to(school, brand, LV_ALIGN_OUT_RIGHT_MID, sx(10), 0);
    lv_obj_t *school_label = lv_label_create(school);
    lv_label_set_text(school_label, "FPT University");
    lv_obj_center(school_label);

    lv_obj_t *time_box = plain_obj(dashboard_screen);
    lv_obj_set_size(time_box, sx(180), sy(48));
    lv_obj_align(time_box, LV_ALIGN_TOP_MID, 0, sy(22));
    lv_obj_set_flex_flow(time_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(time_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(time_box, LV_OPA_TRANSP, 0);

    dashboard_time_label = make_label(time_box, "--:--", &style_text_dark);
    lv_obj_set_style_text_font(dashboard_time_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(dashboard_time_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(dashboard_time_label, LV_PCT(100));

    dashboard_date_label = make_label(time_box, "--", &style_text_muted);
    lv_obj_set_style_text_align(dashboard_date_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(dashboard_date_label, LV_PCT(100));

    dashboard_wifi_icon = plain_obj(dashboard_screen);
    lv_obj_set_size(dashboard_wifi_icon, at_least(sx(30), 24), at_least(sy(30), 24));
    lv_obj_align(dashboard_wifi_icon, LV_ALIGN_TOP_RIGHT, -sx(112), sy(28));
    lv_obj_set_style_text_color(dashboard_wifi_icon, lv_color_hex(0x2f6598), 0);
    lv_obj_add_flag(dashboard_wifi_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *wifi_label = lv_label_create(dashboard_wifi_icon);
#ifdef LV_SYMBOL_WIFI
    lv_label_set_text(wifi_label, LV_SYMBOL_WIFI);
#else
    lv_label_set_text(wifi_label, "WiFi");
#endif
    lv_obj_center(wifi_label);

    lv_obj_t *bell = lv_btn_create(dashboard_screen);
    lv_obj_remove_style_all(bell);
    lv_obj_add_style(bell, &style_close_button, 0);
    lv_obj_set_size(bell, at_least(sx(48), 42), at_least(sx(48), 42));
    lv_obj_align(bell, LV_ALIGN_TOP_RIGHT, -sx(34), sy(20));
    lv_obj_add_event_cb(bell, history_open_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bell_label = lv_label_create(bell);
    lv_label_set_text(bell_label, LV_SYMBOL_LOOP);
    lv_obj_center(bell_label);

    (void)time_box;

    lv_obj_t *badge = make_circle(bell, at_least(sx(19), 17), lv_color_hex(0xff6b13), LV_OPA_COVER);
    lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, 1, -4);
    lv_obj_t *badge_label = lv_label_create(badge);
    lv_label_set_text(badge_label, "3");
    lv_obj_set_style_text_color(badge_label, lv_color_hex(0xffffff), 0);
    lv_obj_center(badge_label);

    lv_obj_t *mascot = make_unimate_logo(dashboard_screen, sx(132), false);
    lv_obj_align(mascot, LV_ALIGN_TOP_MID, 0, sy(112));

    lv_obj_t *title = make_label(dashboard_screen, "UNIMATE", &style_title);
    lv_obj_set_style_text_color(title, lv_color_hex(0x126fbd), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, sy(276));

    lv_obj_t *subtitle = make_label(dashboard_screen, "Trợ lý AI thông minh tại FPT University", &style_text_muted);
    lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, sy(327));

    lv_obj_t *cards = plain_obj(dashboard_screen);
    lv_obj_set_size(cards, LV_PCT(93), sy(168));
    lv_obj_align(cards, LV_ALIGN_TOP_MID, 0, sy(391));
    lv_obj_set_flex_flow(cards, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cards, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(cards, LV_OPA_TRANSP, 0);
    make_action_card(cards, "AI Chat", "AI", &style_icon_orange, new_chat_event_cb, NULL);
    make_action_card(cards, "Campus News", LV_SYMBOL_FILE, &style_icon_green, campus_news_event_cb, NULL);

    dashboard_mic_ring = make_circle(dashboard_screen, at_least(sx(132), 92), lv_color_hex(0xffd9bd), LV_OPA_60);
    lv_obj_align(dashboard_mic_ring, LV_ALIGN_TOP_MID, 0, sy(374));

    dashboard_mic_btn = lv_btn_create(dashboard_screen);
    lv_obj_remove_style_all(dashboard_mic_btn);
    lv_obj_add_style(dashboard_mic_btn, &style_send_button, 0);
    lv_obj_set_size(dashboard_mic_btn, at_least(sx(108), 76), at_least(sx(108), 76));
    lv_obj_set_style_radius(dashboard_mic_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(dashboard_mic_btn, LV_ALIGN_TOP_MID, 0, sy(386));
    lv_obj_add_event_cb(dashboard_mic_btn, mic_event_cb, LV_EVENT_CLICKED, NULL);

    make_mic_icon(dashboard_mic_btn, lv_color_hex(0xffffff));

    dashboard_mic_hint = make_label(dashboard_screen, "Nhấn để nói", &style_text_muted);
    lv_obj_set_style_text_align(dashboard_mic_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(dashboard_mic_hint, LV_ALIGN_TOP_MID, 0, sy(514));

    dashboard_mic_wave = plain_obj(dashboard_screen);
    lv_obj_set_size(dashboard_mic_wave, sx(52), sy(18));
    lv_obj_align(dashboard_mic_wave, LV_ALIGN_TOP_MID, 0, sy(542));
    lv_obj_set_flex_flow(dashboard_mic_wave, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dashboard_mic_wave, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dashboard_mic_wave, sx(4), 0);
    lv_obj_set_style_bg_opa(dashboard_mic_wave, LV_OPA_TRANSP, 0);
    for (uint8_t i = 0; i < 4; i++) {
        static const int16_t heights[] = {16, 10, 14, 6};
        dashboard_wave_bars[i] = plain_obj(dashboard_mic_wave);
        lv_obj_set_size(dashboard_wave_bars[i], sx(5), sy(heights[i]));
        lv_obj_set_style_radius(dashboard_wave_bars[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dashboard_wave_bars[i], lv_color_hex(0x16bdd7), 0);
        lv_obj_set_style_bg_opa(dashboard_wave_bars[i], LV_OPA_COVER, 0);
    }
    lv_obj_add_flag(dashboard_mic_wave, LV_OBJ_FLAG_HIDDEN);
    dashboard_wave_timer = lv_timer_create(dashboard_wave_timer_cb, 150, NULL);
    lv_timer_pause(dashboard_wave_timer);
}

static void create_chat()
{
    chat_screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(chat_screen);
    lv_obj_add_style(chat_screen, &style_screen, 0);
    lv_obj_set_size(chat_screen, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(chat_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(chat_screen, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *header = plain_obj(chat_screen);
    lv_obj_set_size(header, LV_PCT(100), sy(82));
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_color(header, lv_color_hex(0xe0e9f0), 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);

    lv_obj_t *small_mascot = make_unimate_logo(header, at_least(sx(40), 38), true);
    lv_obj_align(small_mascot, LV_ALIGN_LEFT_MID, sx(28), 0);

    lv_obj_t *chat_title = make_label(header, "UniMate AI Chat", &style_text_dark);
    lv_obj_set_style_text_font(chat_title, &lv_font_montserrat_14, 0);
    lv_obj_align_to(chat_title, small_mascot, LV_ALIGN_OUT_RIGHT_TOP, sx(14), 0);

    status_label = make_label(header, "Online", NULL);
    lv_obj_set_style_text_color(status_label, lv_color_hex(0x00a75a), 0);
    lv_obj_set_style_text_font(status_label, &font_vietnamese_16, 0);
    lv_obj_align_to(status_label, chat_title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, sy(6));

    lv_obj_t *close_btn = lv_btn_create(header);
    lv_obj_remove_style_all(close_btn);
    lv_obj_add_style(close_btn, &style_close_button, 0);
    lv_obj_set_size(close_btn, at_least(sx(40), 40), at_least(sx(40), 40));
    lv_obj_align(close_btn, LV_ALIGN_RIGHT_MID, -sx(28), 0);
    lv_obj_add_event_cb(close_btn, back_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_label = lv_label_create(close_btn);
    lv_label_set_text(close_label, LV_SYMBOL_CLOSE);
    lv_obj_center(close_label);

    chat_list = plain_obj(chat_screen);
    lv_obj_set_size(chat_list, LV_PCT(100), disp_h() - sy(182));
    lv_obj_align(chat_list, LV_ALIGN_TOP_MID, 0, sy(90));
    lv_obj_add_flag(chat_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_left(chat_list, sx(27), 0);
    lv_obj_set_style_pad_right(chat_list, sx(27), 0);
    lv_obj_set_style_pad_top(chat_list, sy(10), 0);
    lv_obj_set_flex_flow(chat_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(chat_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(chat_list, LV_DIR_VER);

    create_typing_indicator();

    input_bar = plain_obj(chat_screen);
    lv_obj_set_size(input_bar, LV_PCT(100), sy(92));
    lv_obj_align(input_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(input_bar, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(input_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(input_bar, 1, 0);
    lv_obj_set_style_border_color(input_bar, lv_color_hex(0xe0e5ea), 0);
    lv_obj_set_style_border_side(input_bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_flex_flow(input_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(input_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(input_bar, sx(27), 0);
    lv_obj_set_style_pad_right(input_bar, sx(27), 0);
    lv_obj_set_style_pad_column(input_bar, sx(14), 0);

    chat_mic_btn = lv_btn_create(input_bar);
    lv_obj_remove_style_all(chat_mic_btn);
    lv_obj_add_style(chat_mic_btn, &style_send_button, 0);
    lv_obj_set_size(chat_mic_btn, at_least(sx(64), 54), at_least(sy(64), 54));
    lv_obj_set_style_radius(chat_mic_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(chat_mic_btn, mic_event_cb, LV_EVENT_CLICKED, NULL);
    make_mic_icon(chat_mic_btn, lv_color_hex(0xffffff));

    input_ta = lv_textarea_create(input_bar);
    lv_obj_remove_style_all(input_ta);
    lv_obj_add_style(input_ta, &style_input, 0);
    lv_obj_clear_flag(input_ta, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(input_ta, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_height(input_ta, at_least(sy(62), 54));
    lv_obj_set_flex_grow(input_ta, 1);
    lv_obj_set_style_text_font(input_ta, &font_vietnamese_16, 0);
    lv_textarea_set_one_line(input_ta, true);
    lv_textarea_set_placeholder_text(input_ta, "Nhập tin nhắn...");
    lv_obj_add_event_cb(input_ta, input_event_cb, LV_EVENT_FOCUSED, NULL);

    lv_obj_t *send_btn = lv_btn_create(input_bar);
    lv_obj_remove_style_all(send_btn);
    lv_obj_add_style(send_btn, &style_send_button, 0);
    lv_obj_set_size(send_btn, at_least(sx(64), 54), at_least(sy(64), 54));
    lv_obj_set_style_radius(send_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(send_btn, send_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *send_label = lv_label_create(send_btn);
    lv_label_set_text(send_label, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(send_label, &lv_font_montserrat_30, 0);
    lv_obj_center(send_label);

    keyboard = lv_keyboard_create(chat_screen);
    lv_obj_set_size(keyboard, LV_PCT(100), at_least(sy(190), 150));
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(keyboard, input_ta);
    lv_obj_add_event_cb(keyboard, keyboard_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(keyboard);
}

void ui_chat_init()
{
    init_styles();
    create_dashboard();
    create_chat();

    ui_chat_add_message("assistant", "Hi! I'm UniMate. How can I help you today?");

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

void ui_chat_set_open_history_callback(ui_chat_simple_cb_t cb)
{
    on_open_history = cb;
}

void ui_chat_set_history_callback(ui_chat_history_cb_t cb)
{
    on_history = cb;
}

void ui_chat_show_sessions(const char *const *titles, uint8_t count)
{
    if (!chat_list) {
        return;
    }

    lv_obj_clean(chat_list);
    typing_row = NULL;
    typing_label = NULL;

    if (!titles || count == 0) {
        ui_chat_add_message("assistant", "No chat sessions yet.");
        return;
    }

    for (uint8_t i = 0; i < count; i++) {
        static char clean_title[256];
        sanitize_display_text(titles[i] ? titles[i] : "(unknown)", clean_title, sizeof(clean_title));
        const char *title = clean_title[0] ? clean_title : "(unknown)";

        lv_obj_t *btn = lv_btn_create(chat_list);
        lv_obj_remove_style_all(btn);
        lv_obj_add_style(btn, &style_card, 0);
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_set_height(btn, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(btn, 14, 0);
        lv_obj_add_event_cb(btn, session_item_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, title);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(label, LV_PCT(100));
        lv_obj_set_style_text_color(label, lv_color_hex(0x07142d), 0);
        lv_obj_set_style_text_font(label, &font_vietnamese_16, 0);
    }
}

void ui_chat_add_message(const char *role, const char *message)
{
    if (!chat_list || !message) {
        return;
    }

    static char clean_message[2048];
    sanitize_display_text(message, clean_message, sizeof(clean_message));
    if (clean_message[0] == '\0') {
        return;
    }

    set_typing_indicator(false, NULL);

    lv_obj_t *row = create_message_row(role);
    lv_obj_t *bubble = plain_obj(row);
    lv_obj_add_style(bubble, strcmp(role, "user") == 0 ? &style_user_bubble : &style_bot_bubble, 0);
    lv_obj_set_width(bubble, strcmp(role, "user") == 0 ? LV_PCT(36) : LV_PCT(66));
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);

    lv_obj_t *label = lv_label_create(bubble);
    lv_label_set_text(label, clean_message);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_font(label, &font_vietnamese_16, 0);

    scroll_chat_to_bottom(LV_ANIM_OFF);
}

void ui_chat_clear_messages()
{
    if (chat_list) {
        lv_obj_clean(chat_list);
    }
    typing_row = NULL;
    typing_label = NULL;

    if (chat_list) {
        create_typing_indicator();
    }
}

void ui_chat_set_status(const char *status)
{
    if (!status_label || !status) {
        return;
    }

    if (strcmp(status, "Thinking...") == 0) {
        lv_label_set_text(status_label, "Online");
        set_typing_indicator(true, status);
        return;
    }

    set_typing_indicator(false, NULL);
    lv_label_set_text(status_label, status);
}

void ui_chat_set_datetime(const char *time_text, const char *date_text)
{
    if (dashboard_time_label && time_text) {
        lv_label_set_text(dashboard_time_label, time_text);
    }
    if (dashboard_date_label && date_text) {
        lv_label_set_text(dashboard_date_label, date_text);
    }
}

void ui_chat_set_wifi_connected(bool connected)
{
    if (!dashboard_wifi_icon) {
        return;
    }

    if (connected) {
        lv_obj_clear_flag(dashboard_wifi_icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(dashboard_wifi_icon, LV_OBJ_FLAG_HIDDEN);
    }
}

bool ui_chat_is_mic_recording()
{
    return mic_is_recording;
}

void ui_chat_set_mic_recording(bool recording)
{
    mic_is_recording = recording;
    update_mic_recording_ui();
}



static lv_obj_t *wifi_screen = NULL;
static lv_obj_t *wifi_ssid_label = NULL;
static lv_obj_t *wifi_ip_label = NULL;

void ui_chat_show_wifi_config_screen(const char *ssid, const char *ip)
{
    if (!wifi_screen) {
        wifi_screen = lv_obj_create(NULL);
        lv_obj_remove_style_all(wifi_screen);
        lv_obj_add_style(wifi_screen, &style_screen, 0);
        
        lv_obj_t *title = make_label(wifi_screen, "WiFi Config", &style_title);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, sy(150));
        
        lv_obj_t *desc = make_label(wifi_screen, "Please connect your phone to the following WiFi\nand go to the IP address to configure.", &style_text_muted);
        lv_obj_set_style_text_align(desc, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(desc, LV_ALIGN_TOP_MID, 0, sy(220));
        
        wifi_ssid_label = make_label(wifi_screen, "", &style_text_dark);
        lv_obj_set_style_text_font(wifi_ssid_label, &lv_font_montserrat_30, 0);
        lv_obj_set_style_text_color(wifi_ssid_label, lv_color_hex(0xff7417), 0);
        lv_obj_align(wifi_ssid_label, LV_ALIGN_TOP_MID, 0, sy(300));
        
        wifi_ip_label = make_label(wifi_screen, "", &style_text_dark);
        lv_obj_set_style_text_font(wifi_ip_label, &lv_font_montserrat_30, 0);
        lv_obj_set_style_text_color(wifi_ip_label, lv_color_hex(0xff7417), 0);
        lv_obj_align(wifi_ip_label, LV_ALIGN_TOP_MID, 0, sy(360));
    }
    
    if (ssid) lv_label_set_text_fmt(wifi_ssid_label, "SSID: %s", ssid);
    if (ip) lv_label_set_text_fmt(wifi_ip_label, "IP: %s", ip);
    
    lv_scr_load(wifi_screen);
}

static lv_obj_t *news_screen = NULL;
static lv_obj_t *news_canvas = NULL;
static lv_color_t *news_canvas_buf = NULL;
static lv_obj_t *news_status_label = NULL;

static void news_close_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (on_campus_news_close) {
        on_campus_news_close();
    }
}

static void news_left_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (on_campus_news_swipe) {
        on_campus_news_swipe(-1);
    }
}

static void news_right_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (on_campus_news_swipe) {
        on_campus_news_swipe(1);
    }
}

static void news_gesture_event_cb(lv_event_t *e)
{
    if (on_campus_news_swipe) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
        if (dir == LV_DIR_LEFT) {
            on_campus_news_swipe(1);
        } else if (dir == LV_DIR_RIGHT) {
            on_campus_news_swipe(-1);
        }
    }
}

void ui_chat_set_campus_news_callback(ui_chat_simple_cb_t cb)
{
    on_campus_news = cb;
}

void ui_chat_set_campus_news_close_callback(ui_chat_simple_cb_t cb)
{
    on_campus_news_close = cb;
}

void ui_chat_set_campus_news_swipe_callback(ui_chat_int_cb_t cb)
{
    on_campus_news_swipe = cb;
}

void ui_chat_show_campus_news()
{
    if (!news_screen) {
        news_screen = lv_obj_create(NULL);
        lv_obj_remove_style_all(news_screen);
        lv_obj_add_style(news_screen, &style_screen, 0);
        lv_obj_set_size(news_screen, LV_PCT(100), LV_PCT(100));
        lv_obj_clear_flag(news_screen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(news_screen, LV_SCROLLBAR_MODE_OFF);

        news_canvas_buf = (lv_color_t *)ps_malloc(800 * 480 * sizeof(lv_color_t));
        if (news_canvas_buf) {
            memset(news_canvas_buf, 0, 800 * 480 * sizeof(lv_color_t));
        }

        if (news_canvas_buf) {
            news_canvas = lv_canvas_create(news_screen);
            lv_canvas_set_buffer(news_canvas, news_canvas_buf, 800, 480, LV_IMG_CF_TRUE_COLOR);
            lv_obj_align(news_canvas, LV_ALIGN_CENTER, 0, 0);
        }

        news_status_label = lv_label_create(news_screen);
        lv_obj_add_style(news_status_label, &style_text_dark, 0);
        lv_obj_set_style_text_font(news_status_label, &font_vietnamese_16, 0);
        lv_obj_set_style_text_color(news_status_label, lv_color_hex(0x333333), 0);
        lv_label_set_text(news_status_label, "Loading...");
        lv_obj_align(news_status_label, LV_ALIGN_CENTER, 0, 0);

        lv_obj_t *close_btn = lv_btn_create(news_screen);
        lv_obj_remove_style_all(close_btn);
        lv_obj_add_style(close_btn, &style_close_button, 0);
        lv_obj_set_size(close_btn, at_least(sx(48), 44), at_least(sx(48), 44));
        lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, -sx(20), sy(20));
        lv_obj_add_event_cb(close_btn, news_close_event_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *close_label = lv_label_create(close_btn);
        lv_label_set_text(close_label, LV_SYMBOL_CLOSE);
        lv_obj_center(close_label);

        lv_obj_add_event_cb(news_screen, news_gesture_event_cb, LV_EVENT_GESTURE, NULL);
    }
    
    lv_scr_load_anim(news_screen, LV_SCR_LOAD_ANIM_FADE_ON, 140, 0, false);
}

void ui_chat_hide_campus_news()
{
    show_dashboard_screen();
}

void ui_chat_set_campus_news_status(const char *status)
{
    if (news_status_label) {
        lv_obj_add_flag(news_status_label, LV_OBJ_FLAG_HIDDEN);
    }
}

lv_color_t* ui_chat_get_news_canvas_buffer()
{
    return news_canvas_buf;
}

void ui_chat_refresh_news_canvas()
{
    if (news_canvas) {
        lv_obj_invalidate(news_canvas);
    }
}

