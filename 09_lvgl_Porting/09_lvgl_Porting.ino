#include <Arduino.h>
#include <esp_display_panel.hpp>
#include <WiFi.h>
#include <WiFiClient.h>
#include <time.h>

#include <lvgl.h>
#include "lvgl_v8_port.h"
#include "ui_chat.h"
#include <SPI.h>
#include <SD.h>

#if __has_include("esp32/rom/tjpgd.h")
#include "esp32/rom/tjpgd.h"
#elif __has_include("rom/tjpgd.h")
#include "rom/tjpgd.h"
#else
#include "esp_rom_tjpgd.h"
#endif

using namespace esp_panel::drivers;
using namespace esp_panel::board;

static const char *WIFI_SSID = "spad0604";
static const char *WIFI_PASSWORD = "06042004";
static const char *SERVER_HOST = "54.206.118.226";
static const uint16_t SERVER_PORT = 8000;
static const char *SERVER_BASE_URL = "http://54.206.118.226:8000";
static Board *global_board = nullptr;

static const long GMT_OFFSET_SEC = 7 * 3600;
static const int DAYLIGHT_OFFSET_SEC = 0;

static HardwareSerial AudioSerial(1);
static const int AUDIO_UART_RX = 44;
static const int AUDIO_UART_TX = 43;
static const uint32_t AUDIO_UART_BAUD = 460800;

static const uint32_t SAMPLE_RATE = 16000;
static const uint16_t BITS_PER_SAMPLE = 16;
static const uint16_t CHANNELS = 1;
static const size_t MAX_PCM_BYTES = SAMPLE_RATE * 2 * 8;

static String session_id;
static String pending_text;
static volatile bool pending_text_send = false;
static volatile bool pending_voice_toggle = false;
static bool recording = false;
static uint8_t *pcm_buffer = nullptr;
static size_t pcm_len = 0;

static const uint8_t MAX_HISTORY_SESSIONS = 12;
static String history_session_ids[MAX_HISTORY_SESSIONS];
static String history_last_messages[MAX_HISTORY_SESSIONS];
static char history_titles[MAX_HISTORY_SESSIONS][96];
static const char *history_title_ptrs[MAX_HISTORY_SESSIONS];
static uint8_t history_session_count = 0;
static uint32_t session_counter = 0;
static bool time_synced = false;
static uint32_t last_time_sync_ms = 0;
static uint32_t last_clock_update_ms = 0;

static void halt_on_error(const char *message)
{
    Serial.println(message);
    Serial.println("System halted. Check board/display configuration before continuing.");
    while (true) {
        delay(1000);
    }
}

static void ui_status(const char *status)
{
    if (lvgl_port_lock(50)) {
        ui_chat_set_status(status);
        lvgl_port_unlock();
    }
}

static void ui_add(const char *role, const String &message)
{
    if (lvgl_port_lock(200)) {
        ui_chat_add_message(role, message.c_str());
        lvgl_port_unlock();
    }
}

static String newSessionId()
{
    uint64_t mac = ESP.getEfuseMac();
    uint32_t t = millis();
    session_counter++;
    char buf[64];
    // Example: s3-7c9eBD01a2b3-0012-123456
    snprintf(buf, sizeof(buf), "s3-%04x%08x-%04lu-%lu",
             (uint16_t)(mac >> 32),
             (uint32_t)(mac & 0xffffffffULL),
             (unsigned long)session_counter,
             (unsigned long)t);
    return String(buf);
}

static String jsonEscape(const String &input)
{
    String out;
    out.reserve(input.length() + 8);
    for (size_t i = 0; i < input.length(); i++) {
        char c = input[i];
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else {
            out += c;
        }
    }
    return out;
}

static String extractJsonString(const String &json, const char *key)
{
    String marker = String("\"") + key + "\":";
    int start = json.indexOf(marker);
    if (start < 0) {
        return "";
    }
    start = json.indexOf('"', start + marker.length());
    if (start < 0) {
        return "";
    }
    String out;
    bool esc = false;
    for (int i = start + 1; i < (int)json.length(); i++) {
        char c = json[i];
        if (esc) {
            if (c == 'n') {
                out += '\n';
            } else if (c == 'r') {
                out += '\r';
            } else {
                out += c;
            }
            esc = false;
        } else if (c == '\\') {
            esc = true;
        } else if (c == '"') {
            break;
        } else {
            out += c;
        }
    }
    return out;
}

static String extractJsonStringFrom(const String &json, int from, const char *key, int *next_pos)
{
    String marker = String("\"") + key + "\":";
    int start = json.indexOf(marker, from);
    if (start < 0) {
        return "";
    }
    start = json.indexOf('"', start + marker.length());
    if (start < 0) {
        return "";
    }

    String out;
    bool esc = false;
    for (int i = start + 1; i < (int)json.length(); i++) {
        char c = json[i];
        if (esc) {
            if (c == 'n') {
                out += '\n';
            } else if (c == 'r') {
                out += '\r';
            } else if (c == 't') {
                out += '\t';
            } else {
                out += c;
            }
            esc = false;
        } else if (c == '\\') {
            esc = true;
        } else if (c == '"') {
            if (next_pos) {
                *next_pos = i + 1;
            }
            return out;
        } else {
            out += c;
        }
    }
    return out;
}

static String httpGet(const String &path)
{
    if (!ensureWiFi()) {
        return "";
    }

    WiFiClient client;
    client.setTimeout(30000);
    if (!client.connect(SERVER_HOST, SERVER_PORT)) {
        Serial.println("HTTP connect failed");
        return "";
    }

    client.print("GET ");
    client.print(path);
    client.print(" HTTP/1.1\r\nHost: ");
    client.print(SERVER_HOST);
    client.print(":");
    client.print(SERVER_PORT);
    client.print("\r\nConnection: close\r\n\r\n");

    int status = 0;
    String body = readHttpResponse(client, &status);
    client.stop();

    Serial.printf("GET %s -> %d\n", path.c_str(), status);
    if (status < 200 || status >= 300) {
        return "";
    }
    return body;
}

static String truncateForTitle(const String &s, size_t max_len)
{
    String out = s;
    out.replace("\n", " ");
    out.replace("\r", " ");
    out.trim();
    if (out.length() <= max_len) {
        return out;
    }
    return out.substring(0, (int)max_len - 3) + "...";
}

static uint8_t refreshHistorySessions()
{
    history_session_count = 0;

    String body = httpGet("/api/v1/chat/sessions?limit=12");
    if (body.length() == 0) {
        return 0;
    }

    int pos = 0;
    while (history_session_count < MAX_HISTORY_SESSIONS) {
        String sid = extractJsonStringFrom(body, pos, "session_id", &pos);
        if (sid.length() == 0) {
            break;
        }
        String last = extractJsonStringFrom(body, pos, "last_message", &pos);
        if (last.length() == 0) {
            last = sid;
        }

        history_session_ids[history_session_count] = sid;
        history_last_messages[history_session_count] = last;

        String title = String(history_session_count + 1) + ". " + truncateForTitle(last, 72);
        snprintf(history_titles[history_session_count], sizeof(history_titles[history_session_count]), "%s", title.c_str());
        history_title_ptrs[history_session_count] = history_titles[history_session_count];
        history_session_count++;
    }

    return history_session_count;
}

static void showHistorySessionsInUi()
{
    ui_status("History");
    if (!refreshHistorySessions()) {
        if (lvgl_port_lock(200)) {
            ui_chat_clear_messages();
            ui_chat_add_message("assistant", "Khong tai duoc danh sach lich su. Kiem tra WiFi/API.");
            lvgl_port_unlock();
        }
        return;
    }

    if (lvgl_port_lock(400)) {
        ui_chat_show_sessions(history_title_ptrs, history_session_count);
        lvgl_port_unlock();
    }
}

static String absoluteAudioUrl(String audio_url)
{
    if (audio_url.startsWith("http://") || audio_url.startsWith("https://")) {
        return audio_url;
    }
    if (audio_url.startsWith("/")) {
        return String(SERVER_BASE_URL) + audio_url;
    }
    return audio_url;
}

static bool extractServerPath(const String &url, String &path)
{
    String base = String(SERVER_BASE_URL);
    if (url.startsWith(base)) {
        path = url.substring(base.length());
        if (!path.startsWith("/")) {
            path = "/" + path;
        }
        return true;
    }
    if (url.startsWith("/")) {
        path = url;
        return true;
    }
    return false;
}

static uint16_t readLe16FromBytes(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t readLe32FromBytes(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void sendSpeakerFrame(const uint8_t *data, uint16_t len)
{
    static const uint8_t header[] = {'S', 'P', 'K', '0'};
    uint8_t len_bytes[2] = {
        (uint8_t)(len & 0xff),
        (uint8_t)(len >> 8),
    };
    AudioSerial.write(header, sizeof(header));
    AudioSerial.write(len_bytes, sizeof(len_bytes));
    AudioSerial.write(data, len);
}

static bool waitForAudioBoardAck(const char *expected, uint32_t timeout_ms)
{
    String line;
    uint32_t start = millis();
    while (millis() - start < timeout_ms) {
        while (AudioSerial.available()) {
            char c = (char)AudioSerial.read();
            if (c == '\n') {
                line.trim();
                if (line == expected) {
                    Serial.print("Audio board ack: ");
                    Serial.println(line);
                    return true;
                }
                if (line.length() > 0) {
                    Serial.print("Audio board event: ");
                    Serial.println(line);
                }
                line = "";
            } else if (c != '\r') {
                if (line.length() < 80) {
                    line += c;
                }
            }
        }
        delay(1);
    }

    Serial.print("Timeout waiting for audio board ack: ");
    Serial.println(expected);
    return false;
}

static bool readClientBytes(WiFiClient &client, uint8_t *buffer, size_t len, uint32_t timeout_ms)
{
    size_t pos = 0;
    uint32_t start = millis();
    while (pos < len && millis() - start < timeout_ms) {
        while (client.available() && pos < len) {
            buffer[pos++] = (uint8_t)client.read();
            start = millis();
        }
        delay(1);
    }
    return pos == len;
}

static bool streamWavUrlToAudioBoard(const String &audio_url)
{
    String path;
    if (!extractServerPath(audio_url, path)) {
        Serial.print("Unsupported audio URL: ");
        Serial.println(audio_url);
        return false;
    }
    if (!ensureWiFi()) {
        return false;
    }

    WiFiClient client;
    client.setTimeout(30000);
    if (!client.connect(SERVER_HOST, SERVER_PORT)) {
        Serial.println("Audio HTTP connect failed");
        return false;
    }

    client.print("GET ");
    client.print(path);
    client.print(" HTTP/1.1\r\nHost: ");
    client.print(SERVER_HOST);
    client.print(":");
    client.print(SERVER_PORT);
    client.print("\r\nConnection: close\r\n\r\n");

    String status_line = client.readStringUntil('\n');
    status_line.trim();
    int status = 0;
    int first_space = status_line.indexOf(' ');
    if (first_space >= 0 && first_space + 4 <= (int)status_line.length()) {
        status = status_line.substring(first_space + 1, first_space + 4).toInt();
    }

    while (client.connected()) {
        String line = client.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) {
            break;
        }
    }

    if (status < 200 || status >= 300) {
        Serial.printf("Audio GET %s -> %d\n", path.c_str(), status);
        client.stop();
        return false;
    }

    uint8_t wav_header[44];
    if (!readClientBytes(client, wav_header, sizeof(wav_header), 5000)) {
        Serial.println("Audio WAV header read failed");
        client.stop();
        return false;
    }

    if (memcmp(wav_header, "RIFF", 4) != 0 || memcmp(wav_header + 8, "WAVE", 4) != 0 ||
        memcmp(wav_header + 12, "fmt ", 4) != 0 || memcmp(wav_header + 36, "data", 4) != 0) {
        Serial.println("Audio file is not canonical WAV");
        client.stop();
        return false;
    }

    uint16_t audio_format = readLe16FromBytes(wav_header + 20);
    uint16_t channels = readLe16FromBytes(wav_header + 22);
    uint32_t sample_rate = readLe32FromBytes(wav_header + 24);
    uint16_t bits = readLe16FromBytes(wav_header + 34);
    uint32_t data_len = readLe32FromBytes(wav_header + 40);
    if (audio_format != 1 || channels != 1 || sample_rate != 16000 || bits != 16) {
        Serial.printf("Unsupported WAV fmt format=%u ch=%u rate=%lu bits=%u\n",
                      audio_format, channels, (unsigned long)sample_rate, bits);
        client.stop();
        return false;
    }

    Serial.println("Sending PLAY_PCM_START to audio board");
    AudioSerial.print("PLAY_PCM_START\n");
    if (!waitForAudioBoardAck("OK PLAY_PCM_START", 2000)) {
        client.stop();
        return false;
    }

    uint8_t chunk[512];
    uint32_t remaining = data_len;
    while ((client.connected() || client.available()) && remaining > 0) {
        size_t want = remaining > sizeof(chunk) ? sizeof(chunk) : remaining;
        if (!readClientBytes(client, chunk, want, 5000)) {
            break;
        }
        sendSpeakerFrame(chunk, (uint16_t)want);
        remaining -= want;
        delay(1);
    }
    Serial.println("Sending PLAY_PCM_STOP to audio board");
    AudioSerial.print("PLAY_PCM_STOP\n");
    waitForAudioBoardAck("OK PLAY_PCM_STOP", 2000);
    client.stop();
    Serial.printf("Audio streamed, requested=%lu remaining=%lu\n", (unsigned long)data_len, (unsigned long)remaining);
    return remaining == 0;
}

static void handleUsbCommand(String line)
{
    line.trim();
    if (line.length() == 0) {
        return;
    }

    Serial.print("[USB CMD] ");
    Serial.println(line);

    if (line == "PING") {
        Serial.println("OK PONG");
    } else if (line == "REC_START") {
        AudioSerial.print("REC_START\n");
        Serial.println("[UART->AUDIO] REC_START");
    } else if (line == "REC_STOP") {
        AudioSerial.print("REC_STOP\n");
        Serial.println("[UART->AUDIO] REC_STOP");
    } else if (line == "PLAY_PCM_START") {
        AudioSerial.print("PLAY_PCM_START\n");
        Serial.println("[UART->AUDIO] PLAY_PCM_START");
    } else if (line == "PLAY_PCM_STOP") {
        AudioSerial.print("PLAY_PCM_STOP\n");
        Serial.println("[UART->AUDIO] PLAY_PCM_STOP");
    } else if (line.startsWith("PLAY_URL ")) {
        String url = line.substring(9);
        playAudioUrl(url);
    } else {
        Serial.println("Unknown USB command");
    }
}

static void playAudioUrl(const String &audio_url)
{
    if (audio_url.length() == 0) {
        return;
    }
    String url = absoluteAudioUrl(audio_url);
    ui_status("Speaking...");
    AudioSerial.print("PLAY_URL ");
    AudioSerial.print(url);
    AudioSerial.print("\n");
    Serial.print("[UART->AUDIO] PLAY_URL ");
    Serial.println(url);
}

static bool ensureWiFi()
{
    if (WiFi.status() == WL_CONNECTED) {
        return true;
    }
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        delay(250);
        Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("WiFi IP: ");
        Serial.println(WiFi.localIP());
        return true;
    }
    Serial.println("WiFi connect failed");
    return false;
}

static bool syncClockFromNtp()
{
    if (!ensureWiFi()) {
        return false;
    }

    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, "pool.ntp.org", "time.nist.gov", "time.google.com");
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 5000)) {
        Serial.println("NTP sync failed");
        return false;
    }

    time_synced = true;
    return true;
}

static void updateDashboardClock()
{
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 100)) {
        return;
    }

    char time_buf[8];
    char date_buf[24];
    if (strftime(time_buf, sizeof(time_buf), "%H:%M", &timeinfo) == 0) {
        return;
    }
    if (strftime(date_buf, sizeof(date_buf), "%a, %b %d", &timeinfo) == 0) {
        return;
    }

    if (lvgl_port_lock(50)) {
        ui_chat_set_datetime(time_buf, date_buf);
        lvgl_port_unlock();
    }
}

static String readHttpResponse(WiFiClient &client, int *status_out)
{
    String status_line = client.readStringUntil('\n');
    status_line.trim();
    int status = 0;
    int first_space = status_line.indexOf(' ');
    if (first_space >= 0 && first_space + 4 <= (int)status_line.length()) {
        status = status_line.substring(first_space + 1, first_space + 4).toInt();
    }
    if (status_out) {
        *status_out = status;
    }

    int content_length = -1;
    while (client.connected()) {
        String line = client.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) {
            break;
        }
        if (line.startsWith("Content-Length:")) {
            content_length = line.substring(15).toInt();
        }
    }

    String body;
    uint32_t start = millis();
    while (client.connected() || client.available()) {
        while (client.available()) {
            body += (char)client.read();
            if (content_length > 0 && body.length() >= (size_t)content_length) {
                return body;
            }
        }
        if (millis() - start > 30000) {
            break;
        }
        delay(1);
    }
    return body;
}

static String httpPost(const char *path, const String &content_type, const uint8_t *body, size_t body_len)
{
    WiFiClient client;
    client.setTimeout(30000);
    if (!client.connect(SERVER_HOST, SERVER_PORT)) {
        Serial.println("HTTP connect failed");
        return "";
    }

    client.print("POST ");
    client.print(path);
    client.print(" HTTP/1.1\r\nHost: ");
    client.print(SERVER_HOST);
    client.print(":");
    client.print(SERVER_PORT);
    client.print("\r\nConnection: close\r\nContent-Type: ");
    client.print(content_type);
    client.print("\r\nContent-Length: ");
    client.print(body_len);
    client.print("\r\n\r\n");
    client.write(body, body_len);

    int status = 0;
    String response = readHttpResponse(client, &status);
    client.stop();

    Serial.printf("POST %s -> %d\n", path, status);
    Serial.println(response);
    if (status < 200 || status >= 300) {
        return "";
    }
    return response;
}

static String postJson(const char *path, const String &payload)
{
    if (!ensureWiFi()) {
        return "";
    }

    return httpPost(path, "application/json", (const uint8_t *)payload.c_str(), payload.length());
}

static void writeLe16(uint8_t *p, uint16_t value)
{
    p[0] = value & 0xff;
    p[1] = value >> 8;
}

static void writeLe32(uint8_t *p, uint32_t value)
{
    p[0] = value & 0xff;
    p[1] = (value >> 8) & 0xff;
    p[2] = (value >> 16) & 0xff;
    p[3] = (value >> 24) & 0xff;
}

static void makeWavHeader(uint8_t *header, uint32_t pcm_bytes)
{
    memcpy(header, "RIFF", 4);
    writeLe32(header + 4, 36 + pcm_bytes);
    memcpy(header + 8, "WAVEfmt ", 8);
    writeLe32(header + 16, 16);
    writeLe16(header + 20, 1);
    writeLe16(header + 22, CHANNELS);
    writeLe32(header + 24, SAMPLE_RATE);
    writeLe32(header + 28, SAMPLE_RATE * CHANNELS * BITS_PER_SAMPLE / 8);
    writeLe16(header + 32, CHANNELS * BITS_PER_SAMPLE / 8);
    writeLe16(header + 34, BITS_PER_SAMPLE);
    memcpy(header + 36, "data", 4);
    writeLe32(header + 40, pcm_bytes);
}

static String postVoiceWav()
{
    if (!ensureWiFi() || pcm_len == 0) {
        return "";
    }

    String boundary = "----UniMateESP32Boundary";
    String head = "--" + boundary + "\r\n"
                  "Content-Disposition: form-data; name=\"file\"; filename=\"recording.wav\"\r\n"
                  "Content-Type: audio/wav\r\n\r\n";
    String tail = "\r\n--" + boundary + "--\r\n";
    const size_t wav_header_len = 44;
    size_t total_len = head.length() + wav_header_len + pcm_len + tail.length();
    uint8_t *body = (uint8_t *)ps_malloc(total_len);
    if (!body) {
        Serial.println("No memory for voice upload body");
        return "";
    }

    size_t offset = 0;
    memcpy(body + offset, head.c_str(), head.length());
    offset += head.length();
    makeWavHeader(body + offset, pcm_len);
    offset += wav_header_len;
    memcpy(body + offset, pcm_buffer, pcm_len);
    offset += pcm_len;
    memcpy(body + offset, tail.c_str(), tail.length());

    String response = httpPost(
        "/api/v1/chat/voice",
        "multipart/form-data; boundary=" + boundary,
        body,
        total_len);
    free(body);
    return response;
}

static void handleChatResponse(const String &body)
{
    if (body.length() == 0) {
        ui_add("assistant", "Khong goi duoc server. Kiem tra WiFi/API.");
        ui_status("Error");
        return;
    }

    String returned_session = extractJsonString(body, "session_id");
    if (returned_session.length() > 0) {
        session_id = returned_session;
    }

    String reply = extractJsonString(body, "reply_text");
    String transcript = extractJsonString(body, "transcript");
    String audio_url = extractJsonString(body, "audio_url");

    if (transcript.length() > 0) {
        ui_add("user", transcript);
    }
    if (reply.length() == 0) {
        reply = "Server khong tra ve reply_text.";
    }
    ui_add("assistant", reply);
    playAudioUrl(audio_url);
    ui_status("Online");
}

static void sendTextToServer(const String &text)
{
    ui_status("Thinking...");
    String payload = "{\"message\":\"" + jsonEscape(text) + "\",\"session_id\":\"" + jsonEscape(session_id) + "\"}";
    handleChatResponse(postJson("/api/v1/chat/text", payload));
}

static void handle_send(const char *text)
{
    Serial.print("User: ");
    Serial.println(text);
    pending_text = text;
    pending_text_send = true;
}

static void handle_mic()
{
    pending_voice_toggle = true;
}

static void handle_new_chat()
{
    Serial.println("New chat");
    session_id = newSessionId();
    Serial.print("Session: ");
    Serial.println(session_id);
}

static void handle_history(uint8_t index)
{
    Serial.print("Open history index: ");
    Serial.println(index);

    if (index >= history_session_count) {
        return;
    }

    String selected_session = history_session_ids[index];
    if (selected_session.length() == 0) {
        return;
    }

    session_id = selected_session;
    ui_status("Loading...");

    String path = "/api/v1/chat/history/" + selected_session + "?limit=50";
    String body = httpGet(path);
    if (body.length() == 0) {
        ui_add("assistant", "Khong tai duoc lich su. Kiem tra WiFi/API.");
        ui_status("Error");
        return;
    }

    if (lvgl_port_lock(600)) {
        ui_chat_clear_messages();
        lvgl_port_unlock();
    }

    int pos = 0;
    uint16_t added = 0;
    while (added < 60) {
        String role = extractJsonStringFrom(body, pos, "role", &pos);
        if (role.length() == 0) {
            break;
        }
        String content = extractJsonStringFrom(body, pos, "content", &pos);
        if (content.length() == 0) {
            content = "(empty)";
        }

        if (lvgl_port_lock(300)) {
            ui_chat_add_message(role.c_str(), content.c_str());
            lvgl_port_unlock();
        }
        added++;
    }
    ui_status("History");
}

static void handle_open_history()
{
    Serial.println("Open history");
    showHistorySessionsInUi();
}

static bool sd_initialized = false;
static bool is_playing_video = false;
static File video_file;
static uint8_t *video_frame_buf = nullptr;
static size_t video_frame_buf_sz = 128 * 1024;
static uint32_t last_frame_time_ms = 0;
static const uint32_t frame_interval_ms = 66; // ~15 FPS

static bool init_sd_card()
{
    if (sd_initialized) {
        return true;
    }

    Serial.println("Initializing SD card...");

    // Initialize SPI: SCK = 12, MISO = 13, MOSI = 11, SS = 15
    SPI.begin(12, 13, 11, 15);

    // Select SD CS (EXIO4 = 4) on CH422G I/O Expander
    if (global_board && global_board->getIO_Expander()) {
        auto expander = static_cast<esp_expander::CH422G*>(global_board->getIO_Expander()->getBase());
        if (expander) {
            expander->digitalWrite(4, 0); // Pull EXIO4 LOW to select SD card
            Serial.println("SD CS (EXIO4) set to LOW");
        } else {
            Serial.println("Failed to get CH422G expander base!");
        }
    } else {
        Serial.println("Global board or IO expander is null!");
    }

    // Call SD.begin with dummy CS pin (15)
    if (SD.begin(15, SPI, 4000000)) {
        Serial.println("SD card initialized successfully.");
        sd_initialized = true;
        return true;
    } else {
        Serial.println("SD card initialization failed!");
        return false;
    }
}

static String find_video_file()
{
    if (SD.exists("/video.mjpeg")) {
        return "/video.mjpeg";
    }
    if (SD.exists("/video.mjpg")) {
        return "/video.mjpg";
    }

    File root = SD.open("/");
    if (!root) {
        return "";
    }

    File file = root.openNextFile();
    while (file) {
        if (!file.isDirectory()) {
            String name = file.name();
            if (name.endsWith(".mjpeg") || name.endsWith(".mjpg") || name.endsWith(".MJPEG") || name.endsWith(".MJPG")) {
                String path = "/" + name;
                file.close();
                root.close();
                return path;
            }
        }
        file.close();
        file = root.openNextFile();
    }
    root.close();
    return "";
}

struct JpgDecContext {
    const uint8_t *data;
    size_t size;
    size_t index;
    uint16_t *canvas_buf;
    int canvas_width;
    int canvas_height;
};

static UINT jpg_infunc(JDEC *jd, BYTE *buff, UINT nbyte)
{
    JpgDecContext *ctx = (JpgDecContext *)jd->device;
    if (ctx->index + nbyte > ctx->size) {
        nbyte = ctx->size - ctx->index;
    }
    if (nbyte > 0) {
        memcpy(buff, ctx->data + ctx->index, nbyte);
        ctx->index += nbyte;
    }
    return nbyte;
}

static UINT jpg_outfunc(JDEC *jd, void *bitmap, JRECT *rect)
{
    JpgDecContext *ctx = (JpgDecContext *)jd->device;
    int w = rect->right - rect->left + 1;
    int h = rect->bottom - rect->top + 1;

    int offset_x = (ctx->canvas_width - jd->width) / 2;
    int offset_y = (ctx->canvas_height - jd->height) / 2;
    if (offset_x < 0) offset_x = 0;
    if (offset_y < 0) offset_y = 0;

    for (int y = 0; y < h; y++) {
        int cy = rect->top + y + offset_y;
        if (cy >= ctx->canvas_height) break;

        for (int x = 0; x < w; x++) {
            int cx = rect->left + x + offset_x;
            if (cx >= ctx->canvas_width) break;

            uint16_t color;
            #if JD_FORMAT == 1
            color = ((uint16_t *)bitmap)[y * w + x];
            #else
            uint8_t *p = &((uint8_t *)bitmap)[(y * w + x) * 3];
            color = ((p[0] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[2] >> 3);
            #endif

            ctx->canvas_buf[cy * ctx->canvas_width + cx] = color;
        }
    }
    return 1;
}

static size_t find_next_soi(File &f)
{
    static uint8_t buffer[2048];
    size_t pos = f.position();
    f.seek(pos + 2); // Skip current SOI

    size_t scan_pos = pos + 2;
    while (f.available()) {
        int bytes_read = f.read(buffer, sizeof(buffer));
        if (bytes_read <= 0) break;

        for (int i = 0; i < bytes_read - 1; i++) {
            if (buffer[i] == 0xFF && buffer[i+1] == 0xD8) {
                size_t found_pos = scan_pos + i;
                f.seek(found_pos);
                return found_pos;
            }
        }
        if (buffer[bytes_read - 1] == 0xFF) {
            int next_byte = f.peek();
            if (next_byte == 0xD8) {
                size_t found_pos = scan_pos + bytes_read - 1;
                f.seek(found_pos);
                return found_pos;
            }
        }
        scan_pos += bytes_read;
    }
    return f.size();
}

static void play_next_video_frame()
{
    if (!video_file || !video_file.available()) {
        Serial.println("Video end reached, looping...");
        video_file.seek(0);
    }

    size_t start_pos = video_file.position();
    size_t next_soi_pos = find_next_soi(video_file);
    size_t frame_sz = next_soi_pos - start_pos;

    if (frame_sz < 4) {
        video_file.seek(0);
        return;
    }

    if (frame_sz > video_frame_buf_sz) {
        size_t new_sz = frame_sz + 16 * 1024;
        uint8_t *new_buf = (uint8_t *)ps_realloc(video_frame_buf, new_sz);
        if (new_buf) {
            video_frame_buf = new_buf;
            video_frame_buf_sz = new_sz;
        } else {
            Serial.println("Failed to realloc video frame buffer!");
            return;
        }
    }

    video_file.seek(start_pos);
    if (video_file.read(video_frame_buf, frame_sz) != frame_sz) {
        Serial.println("Failed to read video frame data!");
        return;
    }
    video_file.seek(next_soi_pos);

    JDEC jd;
    uint8_t *pool = (uint8_t *)malloc(4096);
    if (!pool) {
        Serial.println("Failed to allocate JPEG pool!");
        return;
    }

    lv_color_t *canvas_buf = ui_chat_get_video_canvas_buffer();
    if (!canvas_buf) {
        free(pool);
        return;
    }

    JpgDecContext ctx;
    ctx.data = video_frame_buf;
    ctx.size = frame_sz;
    ctx.index = 0;
    ctx.canvas_buf = (uint16_t *)canvas_buf;
    ctx.canvas_width = 800;
    ctx.canvas_height = 480;

    JRESULT res = jd_prepare(&jd, jpg_infunc, pool, 4096, &ctx);
    if (res == JDR_OK) {
        res = jd_decomp(&jd, jpg_outfunc, 0);
        if (res != JDR_OK) {
            Serial.printf("JPEG decompression failed: %d\n", res);
        }
    } else {
        Serial.printf("JPEG prepare failed: %d\n", res);
    }
    free(pool);

    if (lvgl_port_lock(50)) {
        ui_chat_refresh_video_canvas();
        lvgl_port_unlock();
    }
}

static void handle_video_start()
{
    Serial.println("Video start request");

    if (lvgl_port_lock(200)) {
        ui_chat_set_video_status("Dang khoi tao the nho...");
        ui_chat_show_video_player();
        lvgl_port_unlock();
    }

    if (!init_sd_card()) {
        if (lvgl_port_lock(200)) {
            ui_chat_set_video_status("Loi: Khong the khoi tao the nho!\nVui long cam the nho.");
            lvgl_port_unlock();
        }
        return;
    }

    if (lvgl_port_lock(200)) {
        ui_chat_set_video_status("Dang quet tim file video...");
        lvgl_port_unlock();
    }

    String video_path = find_video_file();
    if (video_path.length() == 0) {
        if (lvgl_port_lock(200)) {
            ui_chat_set_video_status("Khong tim thay file video (.mjpeg)!\nVui long copy video vao the nho.");
            lvgl_port_unlock();
        }
        return;
    }

    Serial.print("Opening video file: ");
    Serial.println(video_path);

    video_file = SD.open(video_path, FILE_READ);
    if (!video_file) {
        if (lvgl_port_lock(200)) {
            ui_chat_set_video_status("Loi: Khong the mo file video!");
            lvgl_port_unlock();
        }
        return;
    }

    uint8_t header[2];
    if (video_file.read(header, 2) != 2 || header[0] != 0xFF || header[1] != 0xD8) {
        video_file.close();
        if (lvgl_port_lock(200)) {
            ui_chat_set_video_status("Loi: Dinh dang video khong hop le!\nYeu cau file MJPEG (Motion JPEG).");
            lvgl_port_unlock();
        }
        return;
    }
    video_file.seek(0);

    lv_color_t *canvas_buf = ui_chat_get_video_canvas_buffer();
    if (canvas_buf) {
        memset(canvas_buf, 0, 800 * 480 * sizeof(lv_color_t));
    }

    if (lvgl_port_lock(200)) {
        ui_chat_set_video_status("");
        ui_chat_refresh_video_canvas();
        lvgl_port_unlock();
    }

    if (!video_frame_buf) {
        video_frame_buf = (uint8_t *)ps_malloc(video_frame_buf_sz);
    }

    is_playing_video = true;
    last_frame_time_ms = millis();
}

static void handle_video_close()
{
    Serial.println("Video close request");
    is_playing_video = false;
    if (video_file) {
        video_file.close();
    }

    if (lvgl_port_lock(200)) {
        ui_chat_hide_video_player();
        lvgl_port_unlock();
    }
}



void setup()
{
    String title = "LVGL porting example";

    Serial.begin(115200);
    AudioSerial.begin(AUDIO_UART_BAUD, SERIAL_8N1, AUDIO_UART_RX, AUDIO_UART_TX);
    AudioSerial.print("HMI_UART_TEST\n");
    Serial.printf("[HMI UART] begin RX=%d TX=%d baud=%lu\n", AUDIO_UART_RX, AUDIO_UART_TX, (unsigned long)AUDIO_UART_BAUD);
    Serial.println("[HMI UART TX] HMI_UART_TEST");
    delay(100);
    Serial.printf("PSRAM size: %u bytes\n", ESP.getPsramSize());
    if (ESP.getPsramSize() == 0) {
        halt_on_error("PSRAM is not enabled. Enable OPI PSRAM in Arduino IDE for ESP32-S3 LCD 7.");
    }
    pcm_buffer = (uint8_t *)ps_malloc(MAX_PCM_BYTES);
    if (!pcm_buffer) {
        halt_on_error("Cannot allocate PSRAM audio buffer");
    }
    ensureWiFi();

    Serial.println("Initializing board");
    Board *board = new Board();
    global_board = board;
    if (!board->init()) {
        halt_on_error("Board init failed");
    }

    #if LVGL_PORT_AVOID_TEARING_MODE
    auto lcd = board->getLCD();
    if (lcd == nullptr) {
        halt_on_error("LCD device was not created");
    }
    // When avoid tearing function is enabled, the frame buffer number should be set in the board driver
    lcd->configFrameBufferNumber(LVGL_PORT_DISP_BUFFER_NUM);
#if ESP_PANEL_DRIVERS_BUS_ENABLE_RGB && CONFIG_IDF_TARGET_ESP32S3
    auto lcd_bus = lcd->getBus();
    /**
     * As the anti-tearing feature typically consumes more PSRAM bandwidth, for the ESP32-S3, we need to utilize the
     * "bounce buffer" functionality to enhance the RGB data bandwidth.
     * This feature will consume `bounce_buffer_size * bytes_per_pixel * 2` of SRAM memory.
     */
    if (lcd_bus->getBasicAttributes().type == ESP_PANEL_BUS_TYPE_RGB) {
        static_cast<BusRGB *>(lcd_bus)->configRGB_BounceBufferSize(lcd->getFrameWidth() * 10);
    }
#endif
#endif
    if (!board->begin()) {
        halt_on_error("Board begin failed");
    }

    if (board->getLCD() == nullptr || board->getLCD()->getRefreshPanelHandle() == nullptr) {
        halt_on_error("LCD device is not initialized after board begin");
    }

    Serial.println("Initializing LVGL");
    if (!lvgl_port_init(board->getLCD(), board->getTouch())) {
        halt_on_error("LVGL port init failed");
    }

    Serial.println("Creating UI");
    /* Lock the mutex due to the LVGL APIs are not thread-safe */
    if (!lvgl_port_lock(-1)) {
        halt_on_error("LVGL lock failed");
    }

    ui_chat_set_send_callback(handle_send);
    ui_chat_set_mic_callback(handle_mic);
    ui_chat_set_new_chat_callback(handle_new_chat);
    ui_chat_set_open_history_callback(handle_open_history);
    ui_chat_set_history_callback(handle_history);
    ui_chat_set_video_callback(handle_video_start);
    ui_chat_set_video_close_callback(handle_video_close);
    ui_chat_init();

    syncClockFromNtp();
    updateDashboardClock();

    // Initialize a default session id at boot; a new one will be created on each "New chat".
    if (session_id.length() == 0) {
        session_id = newSessionId();
    }

    /* Release the mutex */
    lvgl_port_unlock();
}

void loop()
{
    static enum { UART_LINE, UART_LEN0, UART_LEN1, UART_PAYLOAD } uart_state = UART_LINE;
    static String uart_line;
    static uint16_t frame_len = 0;
    static uint16_t frame_pos = 0;
    static uint8_t magic_pos = 0;
    static const uint8_t magic[] = {'A', 'U', 'D', '0'};

    while (AudioSerial.available()) {
        uint8_t b = AudioSerial.read();
        if (uart_state == UART_LINE) {
            if (b == magic[magic_pos]) {
                magic_pos++;
                if (magic_pos == sizeof(magic)) {
                    uart_line = "";
                    frame_len = 0;
                    uart_state = UART_LEN0;
                    magic_pos = 0;
                }
            } else {
                magic_pos = 0;
                if (b == '\n') {
                    uart_line.trim();
                    if (uart_line.length() > 0) {
                        Serial.print("Audio board: ");
                        Serial.println(uart_line);
                        if (uart_line == "OK PLAY_DONE" || uart_line == "OK PLAY_URL_DONE") {
                            ui_status("Online");
                        }
                    }
                    uart_line = "";
                } else if (b != '\r' && uart_line.length() < 80) {
                    uart_line += (char)b;
                }
            }
        } else if (uart_state == UART_LEN0) {
            frame_len = b;
            uart_state = UART_LEN1;
        } else if (uart_state == UART_LEN1) {
            frame_len |= ((uint16_t)b << 8);
            frame_pos = 0;
            if (frame_len == 0 || frame_len > 1024) {
                uart_state = UART_LINE;
            } else {
                uart_state = UART_PAYLOAD;
            }
        } else if (uart_state == UART_PAYLOAD) {
            if (recording && pcm_len < MAX_PCM_BYTES) {
                pcm_buffer[pcm_len++] = b;
            }
            frame_pos++;
            if (frame_pos >= frame_len) {
                uart_state = UART_LINE;
            }
        }
    }

    if (Serial.available()) {
        String line = Serial.readStringUntil('\n');
        handleUsbCommand(line);
    }

    if (pending_text_send) {
        pending_text_send = false;
        sendTextToServer(pending_text);
    }

    uint32_t now = millis();
    if (!time_synced && now - last_time_sync_ms > 30000) {
        last_time_sync_ms = now;
        syncClockFromNtp();
    }
    if (now - last_clock_update_ms > 60000) {
        last_clock_update_ms = now;
        updateDashboardClock();
    }

    if (pending_voice_toggle) {
        pending_voice_toggle = false;
        bool should_record = ui_chat_is_mic_recording();
        if (should_record && !recording) {
            pcm_len = 0;
            recording = true;
            AudioSerial.print("REC_START\n");
            Serial.println("[HMI UART TX] REC_START");
            ui_status("Recording...");
            Serial.println("REC_START sent");
        } else if (!should_record && recording) {
            recording = false;
            AudioSerial.print("REC_STOP\n");
            Serial.println("[HMI UART TX] REC_STOP");
            ui_status("Uploading...");
            Serial.printf("REC_STOP sent, pcm=%u\n", (unsigned)pcm_len);
            handleChatResponse(postVoiceWav());
        }
    }

    if (is_playing_video) {
        uint32_t now_ms = millis();
        if (now_ms - last_frame_time_ms >= frame_interval_ms) {
            last_frame_time_ms = now_ms;
            play_next_video_frame();
        }
    }

    delay(2);
}
