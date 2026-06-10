#include <Arduino.h>
#include "esp_task_wdt.h"
#include <esp_display_panel.hpp>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
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

#include <Preferences.h>
#include <WebServer.h>
#include <DNSServer.h>
static WebServer *captive_server = nullptr;
static DNSServer *dns_server = nullptr;
static bool in_ap_mode = false;
static const char *DEFAULT_WIFI_SSID = "spad0604";
static const char *DEFAULT_WIFI_PASSWORD = "06042004";
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

static bool ensureWiFi();
static void playAudioUrl(const String &audio_url);
static String readHttpResponse(WiFiClient &client, int *status_out);

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
    if (status < 200 || status >= 300) {
        Serial.printf("HTTP GET %s failed: %d\n", path.c_str(), status);
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
    if (in_ap_mode) {
        return false;
    }

    Preferences prefs;
    prefs.begin("wifi", true);
    String ssid = prefs.getString("ssid", "");
    String pass = prefs.getString("pass", "");
    prefs.end();

    if (ssid.length() == 0 && strlen(DEFAULT_WIFI_SSID) > 0) {
        ssid = DEFAULT_WIFI_SSID;
        pass = DEFAULT_WIFI_PASSWORD;
        Serial.println("No saved WiFi. Using default WiFi credentials from sketch.");
    }

    if (ssid.length() > 0) {
        Serial.print("Connecting to WiFi: ");
        Serial.println(ssid);
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid.c_str(), pass.c_str());
        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
            delay(250);
            Serial.print(".");
        }
        Serial.println();
        if (WiFi.status() == WL_CONNECTED) {
            Serial.print("WiFi IP: ");
            Serial.println(WiFi.localIP());
            return true;
        }
        Serial.println("WiFi connect failed. Starting AP.");
    }

    in_ap_mode = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP("UniMate-AP");

    dns_server = new DNSServer();
    dns_server->start(53, "*", WiFi.softAPIP());

    captive_server = new WebServer(80);
    captive_server->on("/", HTTP_GET, []() {
        String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'></head>"
                      "<body style='font-family:sans-serif; padding:20px;'>"
                      "<h2>Wifi Config</h2>"
                      "<form action='/save' method='POST'>"
                      "SSID:<br><input type='text' name='ssid' style='width:100%; padding:10px; margin:5px 0 15px;'><br>"
                      "Password:<br><input type='password' name='pass' style='width:100%; padding:10px; margin:5px 0 15px;'><br>"
                      "<input type='submit' value='Save' style='width:100%; padding:10px; background:#ff7417; color:#fff; border:none; border-radius:18px;'>"
                      "</form></body></html>";
        captive_server->send(200, "text/html", html);
    });

    captive_server->on("/save", HTTP_POST, []() {
        String nssid = captive_server->arg("ssid");
        String npass = captive_server->arg("pass");

        Preferences p;
        p.begin("wifi", false);
        p.putString("ssid", nssid);
        p.putString("pass", npass);
        p.end();

        String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'></head>"
                      "<body style='font-family:sans-serif; padding:20px;'><h2>Saved. Rebooting...</h2></body></html>";
        captive_server->send(200, "text/html", html);

        AudioSerial.printf("WIFI_CREDS %s %s\n", nssid.c_str(), npass.c_str());
        Serial.printf("[HMI UART TX] WIFI_CREDS %s %s\n", nssid.c_str(), npass.c_str());

        delay(1000);
        ESP.restart();
    });

    captive_server->onNotFound([]() {
        captive_server->sendHeader("Location", "http://192.168.4.1/", true);
        captive_server->send(302, "text/plain", "");
    });

    captive_server->begin();
    ui_status("AP: UniMate-AP");
    Serial.println("AP started. Connect to 'UniMate-AP' and go to http://192.168.4.1");

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

    size_t sent = 0;
    while (sent < body_len) {
        size_t chunk = body_len - sent;
        if (chunk > 4096) {
            chunk = 4096;
        }
        size_t written = client.write(body + sent, chunk);
        if (written == 0) {
            Serial.printf("HTTP body write stalled at %u/%u\n", (unsigned)sent, (unsigned)body_len);
            client.stop();
            return "";
        }
        sent += written;
        delay(1);
    }

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
    if (!ensureWiFi()) {
        Serial.println("Voice upload skipped: WiFi is not connected");
        return "";
    }
    if (pcm_len == 0) {
        Serial.println("Voice upload skipped: pcm_len is 0");
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

static void handleVoiceChatResponse(const String &body)
{
    if (body.length() == 0) {
        ui_add("assistant", "Khong upload/xu ly duoc audio. Text chat van co the hoat dong.");
        ui_status("Error");
        return;
    }
    handleChatResponse(body);
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

static volatile bool pending_history_load = false;
static uint8_t pending_history_idx = 0;

static void loadHistoryData(uint8_t index)
{
    String selected_session = history_session_ids[index];
    session_id = selected_session;

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

    ui_chat_clear_messages();
    ui_chat_add_message("assistant", "Loading...");

    pending_history_idx = index;
    pending_history_load = true;
}

static volatile bool pending_open_history = false;

static void handle_open_history()
{
    Serial.println("Open history requested");
    pending_open_history = true;
}

static bool sd_initialized = false;

static bool init_sd_card()
{
    constexpr int SD_CS_EXIO = 4;
    constexpr int SD_DUMMY_CS = 6;
    constexpr int SD_CLK = 12;
    constexpr int SD_MISO = 13;
    constexpr int SD_MOSI = 11;

    if (sd_initialized) {
        return true;
    }

    Serial.println("Initializing SD card...");

    SPI.setHwCs(false);
    pinMode(SD_MISO, INPUT_PULLUP);
    pinMode(SD_MOSI, INPUT_PULLUP);
    pinMode(SD_CLK, INPUT_PULLUP);
    pinMode(SD_DUMMY_CS, OUTPUT);
    digitalWrite(SD_DUMMY_CS, HIGH);
    SPI.begin(SD_CLK, SD_MISO, SD_MOSI, SD_DUMMY_CS);

    // Select SD CS (EXIO4 = 4) on CH422G I/O Expander
    esp_task_wdt_reset();
    if (global_board && global_board->getIO_Expander()) {
        auto expander = static_cast<esp_expander::CH422G*>(global_board->getIO_Expander()->getBase());
        if (expander) {
            expander->enableAllIO_Output();
            expander->pinMode(SD_CS_EXIO, OUTPUT);
            expander->digitalWrite(SD_CS_EXIO, 1);
            delay(5);
            for (int i = 0; i < 16; i++) {
                SPI.transfer(0xFF);
            }
            expander->digitalWrite(SD_CS_EXIO, 0); // Pull EXIO4 LOW to select SD card
            Serial.println("SD CS (EXIO4) set to LOW");
        } else {
            Serial.println("Failed to get CH422G expander base!");
        }
    } else {
        Serial.println("Global board or IO expander is null!");
    }

    // Try multiple SPI speeds: 40MHz down to 4MHz
    esp_task_wdt_reset();
    uint32_t speeds[] = {40000000, 20000000, 10000000, 4000000};
    for (int i = 0; i < 4; i++) {
        esp_task_wdt_reset();
        if (SD.begin(15, SPI, speeds[i])) {
            Serial.printf("SD card OK at %d Hz\n", speeds[i]);
            sd_initialized = true;
            uint8_t card_type = SD.cardType();
            if (card_type == CARD_NONE) {
                Serial.printf("SD mounted at %d Hz but card type is NONE\n", speeds[i]);
                continue;
            }
            Serial.printf("SD card OK at %d Hz\n", speeds[i]);
            Serial.printf("SD card type: %u, size: %llu MB\n", card_type, SD.cardSize() / (1024 * 1024));
            sd_initialized = true;
            return true;
        }
        Serial.printf("SD.begin failed at %d Hz\n", speeds[i]);
    }

    return false;
}



struct JpgDecContext {
    const uint8_t *data;
    File *file;
    size_t size;
    size_t index;
    uint16_t *canvas_buf;
    int canvas_width;
    int canvas_height;
    uint32_t blocks;
    uint8_t scale;
    uint16_t *temp_buf = nullptr;
    int image_width = 0;
    int image_height = 0;
};

static UINT jpg_infunc(JDEC *jd, BYTE *buff, UINT nbyte)
{
    JpgDecContext *ctx = (JpgDecContext *)jd->device;
    if (!ctx) {
        return 0;
    }

    if (ctx->file) {
        if (ctx->index + nbyte > ctx->size) {
            nbyte = ctx->size - ctx->index;
        }
        if (nbyte == 0) {
            return 0;
        }
        if (!buff) {
            ctx->file->seek(ctx->file->position() + nbyte);
            ctx->index += nbyte;
            return nbyte;
        }
        int read_len = ctx->file->read(buff, nbyte);
        if (read_len > 0) {
            ctx->index += read_len;
        }
        return read_len > 0 ? read_len : 0;
    }

    if (ctx->index + nbyte > ctx->size) {
        nbyte = ctx->size - ctx->index;
    }
    if (!buff) {
        ctx->index += nbyte;
        return nbyte;
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
    if (!ctx || (!ctx->canvas_buf && !ctx->temp_buf) || !bitmap) {
        return 0;
    }

    int w = rect->right - rect->left + 1;
    int h = rect->bottom - rect->top + 1;

    if (ctx->temp_buf) {
        // Write directly to temporary buffer (for scaling up later)
        for (int y = 0; y < h; y++) {
            int cy = rect->top + y;
            if (cy >= ctx->image_height) break;
            int dest_row = cy * ctx->image_width;

            for (int x = 0; x < w; x++) {
                int cx = rect->left + x;
                if (cx >= ctx->image_width) break;

                uint16_t color;
                #if JD_FORMAT == 1
                color = ((uint16_t *)bitmap)[y * w + x];
                #else
                uint8_t *p = &((uint8_t *)bitmap)[(y * w + x) * 3];
                color = ((p[0] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[2] >> 3);
                #endif

                ctx->temp_buf[dest_row + cx] = color;
            }
        }
    } else {
        // Direct decode to canvas
        int decoded_width = jd->width >> ctx->scale;
        int decoded_height = jd->height >> ctx->scale;
        if (decoded_width <= 0) decoded_width = jd->width;
        if (decoded_height <= 0) decoded_height = jd->height;

        int offset_x = (ctx->canvas_width - decoded_width) / 2;
        int offset_y = (ctx->canvas_height - decoded_height) / 2;
        if (offset_x < 0) offset_x = 0;
        if (offset_y < 0) offset_y = 0;

        if (rect->left >= ctx->canvas_width || rect->top >= ctx->canvas_height) {
            return 1;
        }

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
    }

    ctx->blocks++;
    if ((ctx->blocks & 0x0F) == 0) {
        esp_task_wdt_reset();
        vTaskDelay(1);
    }
    return 1;
}


static volatile bool pending_campus_news = false;
static bool is_campus_news_active = false;
static uint32_t last_banner_change_ms = 0;
static uint8_t current_campus_image_idx = 0;
static constexpr uint8_t MAX_CAMPUS_IMAGES = 20;
static constexpr size_t NEWS_JPEG_MAX = 512 * 1024;
static constexpr size_t LOCAL_JPEG_MAX = 8 * 1024 * 1024;
static String campus_image_paths[MAX_CAMPUS_IMAGES];
static uint8_t campus_image_count = 0;
static uint8_t *news_jpeg_buf = nullptr;

static bool draw_test_banner()
{
    lv_color_t *canvas_buf = ui_chat_get_news_canvas_buffer();
    if (!canvas_buf) {
        if (lvgl_port_lock(200)) {
            ui_chat_set_campus_news_status("ERR: canvas_buf NULL");
            lvgl_port_unlock();
        }
        return false;
    }

    for (int y = 0; y < 480; y++) {
        for (int x = 0; x < 800; x++) {
            uint8_t r = 18 + (x * 70 / 799);
            uint8_t g = 58 + (y * 120 / 479);
            uint8_t b = 120 + ((x + y) * 70 / 1279);
            if (x > 72 && x < 728 && y > 86 && y < 394) {
                r = (r > 221) ? 255 : r + 34;
                g = (g > 225) ? 255 : g + 30;
                b = (b > 239) ? 255 : b + 16;
            }
            if (((x / 24) + (y / 24)) % 9 == 0) {
                r = (r > 231) ? 255 : r + 24;
                g = (g > 239) ? 255 : g + 16;
            }
            canvas_buf[y * 800 + x] = lv_color_make(r, g, b);
        }
    }

    if (lvgl_port_lock(200)) {
        ui_chat_set_campus_news_status("TEST IMAGE: local canvas\nNo server/network needed");
        ui_chat_refresh_news_canvas();
        lvgl_port_unlock();
    }
    return true;
}

static bool download_image(const String& url) {
    if (url.startsWith("test://")) {
        return draw_test_banner();
    }

    if (lvgl_port_lock(200)) {
        String msg = "URL: " + url.substring(0, 50) + "\nConnecting...";
        ui_chat_set_campus_news_status(msg.c_str());
        lvgl_port_unlock();
    }
    
    // Parse URL: support plain HTTP and HTTPS test images.
    String host;
    String path;
    int port = 80;
    bool is_https = false;
    
    if (url.startsWith("http://")) {
        String rest = url.substring(7);
        int slash = rest.indexOf('/');
        if (slash < 0) { host = rest; path = "/"; }
        else { host = rest.substring(0, slash); path = rest.substring(slash); }
        int colon = host.indexOf(':');
        if (colon >= 0) {
            port = host.substring(colon + 1).toInt();
            host = host.substring(0, colon);
        }
    } else if (url.startsWith("https://")) {
        is_https = true;
        port = 443;
        String rest = url.substring(8);
        int slash = rest.indexOf('/');
        if (slash < 0) { host = rest; path = "/"; }
        else { host = rest.substring(0, slash); path = rest.substring(slash); }
        int colon = host.indexOf(':');
        if (colon >= 0) {
            port = host.substring(colon + 1).toInt();
            host = host.substring(0, colon);
        }
    } else {
        if (lvgl_port_lock(200)) {
            ui_chat_set_campus_news_status("ERR: URL must be http/https");
            lvgl_port_unlock();
        }
        return false;
    }
    
    WiFiClient *plain_client = nullptr;
    WiFiClientSecure *secure_client = nullptr;
    Client *client = nullptr;
    if (is_https) {
        secure_client = new WiFiClientSecure();
        if (!secure_client) {
            if (lvgl_port_lock(200)) {
                ui_chat_set_campus_news_status("ERR: WiFiClientSecure alloc");
                lvgl_port_unlock();
            }
            return false;
        }
        secure_client->setInsecure();
        secure_client->setTimeout(10);
        client = secure_client;
    } else {
        plain_client = new WiFiClient();
        if (!plain_client) {
            if (lvgl_port_lock(200)) {
                ui_chat_set_campus_news_status("ERR: WiFiClient alloc");
                lvgl_port_unlock();
            }
            return false;
        }
        plain_client->setTimeout(10);
        client = plain_client;
    }

    auto close_client = [&]() {
        if (client) {
            client->stop();
        }
        delete plain_client;
        delete secure_client;
        plain_client = nullptr;
        secure_client = nullptr;
        client = nullptr;
    };

    if (!client->connect(host.c_str(), port)) {
        if (lvgl_port_lock(200)) {
            String msg = "ERR: Connect failed\nHost: " + host + ":" + String(port);
            ui_chat_set_campus_news_status(msg.c_str());
            lvgl_port_unlock();
        }
        close_client();
        return false;
    }
    
    if (lvgl_port_lock(200)) {
        ui_chat_set_campus_news_status("Connected!\nSending GET...");
        lvgl_port_unlock();
    }
    
    client->printf(
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: UniMate-ESP32S3/1.0\r\n"
        "Accept: image/jpeg,image/*,*/*\r\n"
        "Connection: close\r\n\r\n",
        path.c_str(),
        host.c_str()
    );
    
    // Read HTTP headers
    int http_code = 0;
    String redirect_url = "";
    String content_type = "";
    int content_length = -1;
    bool transfer_chunked = false;
    uint32_t t0 = millis();
    while (!client->available() && client->connected() && (millis() - t0 < 10000)) {
        delay(10);
    }
    while ((client->connected() || client->available()) && (millis() - t0 < 10000)) {
        String line = client->readStringUntil('\n');
        line.trim();
        if (line.length() == 0) break;
        Serial.print("[Campus image HTTP] ");
        Serial.println(line);
        if (http_code == 0 && line.startsWith("HTTP/")) {
            int sp1 = line.indexOf(' ');
            if (sp1 > 0) http_code = line.substring(sp1 + 1, sp1 + 4).toInt();
        }
        if (line.startsWith("Location: ") || line.startsWith("location: ")) {
            redirect_url = line.substring(10);
            redirect_url.trim();
        }
        if (line.startsWith("Content-Type: ") || line.startsWith("content-type: ")) {
            content_type = line.substring(14);
        }
        if (line.startsWith("Content-Length: ") || line.startsWith("content-length: ")) {
            content_length = line.substring(16).toInt();
        }
        if (line.startsWith("Transfer-Encoding: ") || line.startsWith("transfer-encoding: ")) {
            String encoding = line.substring(19);
            encoding.toLowerCase();
            transfer_chunked = encoding.indexOf("chunked") >= 0;
        }
    }
    
    if (lvgl_port_lock(200)) {
        String msg = "HTTP " + String(http_code) + "\nType: " + content_type.substring(0, 30) + "\nLen: " + String(content_length);
        if (transfer_chunked) msg += "\nChunked";
        ui_chat_set_campus_news_status(msg.c_str());
        lvgl_port_unlock();
    }

    if (http_code == 0) {
        if (lvgl_port_lock(200)) {
            String msg = "ERR: no HTTP response\nWiFi=" + String(WiFi.status())
                       + " host=" + host.substring(0, 28)
                       + "\navail=" + String(client->available());
            ui_chat_set_campus_news_status(msg.c_str());
            lvgl_port_unlock();
        }
        close_client();
        return false;
    }
    
    // Handle redirect
    if (http_code >= 300 && http_code < 400 && redirect_url.length() > 0) {
        close_client();
        if (redirect_url.startsWith("/")) {
            redirect_url = String(is_https ? "https://" : "http://") + host + redirect_url;
        }
        return download_image(redirect_url);
    }
    
    if (http_code != 200) {
        close_client();
        return false;
    }
    
    // Download body
    if (!news_jpeg_buf) {
        news_jpeg_buf = (uint8_t *)ps_malloc(NEWS_JPEG_MAX);
        if (!news_jpeg_buf) {
            if (lvgl_port_lock(200)) {
                ui_chat_set_campus_news_status("ERR: ps_malloc JPEG FAILED!\nPSRAM het bo nho");
                lvgl_port_unlock();
            }
            close_client();
            return false;
        }
    }
    
    size_t downloaded = 0;
    bool overflow = false;
    t0 = millis();
    if (transfer_chunked) {
        while ((client->connected() || client->available()) && (millis() - t0 < 15000)) {
            String chunk_line = client->readStringUntil('\n');
            chunk_line.trim();
            if (chunk_line.length() == 0) {
                continue;
            }
            int semi = chunk_line.indexOf(';');
            if (semi >= 0) chunk_line = chunk_line.substring(0, semi);
            size_t chunk_len = strtoul(chunk_line.c_str(), nullptr, 16);
            if (chunk_len == 0) {
                break;
            }
            while (chunk_len > 0 && (millis() - t0 < 15000)) {
                size_t toRead = chunk_len;
                if (toRead > 2048) toRead = 2048;
                if (downloaded + toRead > NEWS_JPEG_MAX) {
                    toRead = NEWS_JPEG_MAX - downloaded;
                    overflow = true;
                }
                if (toRead == 0) break;
                int c = client->readBytes(news_jpeg_buf + downloaded, toRead);
                if (c <= 0) break;
                downloaded += c;
                chunk_len -= c;
                t0 = millis();
            }
            client->readStringUntil('\n');
            if (overflow) break;
        }
    } else {
        while ((client->connected() || client->available()) && (millis() - t0 < 15000) && downloaded < NEWS_JPEG_MAX) {
            size_t avail = client->available();
            if (avail) {
                size_t toRead = avail;
                if (downloaded + toRead > NEWS_JPEG_MAX) {
                    toRead = NEWS_JPEG_MAX - downloaded;
                    overflow = true;
                }
                int c = client->readBytes(news_jpeg_buf + downloaded, toRead);
                downloaded += c;
                t0 = millis();
            }
            delay(1);
        }
    }
    close_client();
    
    if (lvgl_port_lock(200)) {
        String msg = "Downloaded: " + String(downloaded) + " bytes\nDecoding JPEG...";
        if (overflow) msg = "ERR: image >2MB\nTry smaller JPEG";
        ui_chat_set_campus_news_status(msg.c_str());
        lvgl_port_unlock();
    }
    
    if (overflow) return false;
    if (downloaded < 100) return false;
    if (news_jpeg_buf[0] != 0xFF || news_jpeg_buf[1] != 0xD8) {
        if (lvgl_port_lock(200)) {
            String msg = "ERR: not JPEG data\nFirst bytes: "
                       + String(news_jpeg_buf[0], HEX) + " "
                       + String(news_jpeg_buf[1], HEX);
            ui_chat_set_campus_news_status(msg.c_str());
            lvgl_port_unlock();
        }
        return false;
    }
    
    JDEC jd;
    static uint8_t *pool = nullptr;
    if (!pool) pool = (uint8_t *)malloc(4096);
    if (!pool) return false;
    
    lv_color_t *canvas_buf = ui_chat_get_news_canvas_buffer();
    if (!canvas_buf) {
        if (lvgl_port_lock(200)) {
            ui_chat_set_campus_news_status("ERR: canvas_buf NULL!\nVideo canvas chua san sang");
            lvgl_port_unlock();
        }
        return false;
    }
    
    JpgDecContext ctx;
    ctx.data = news_jpeg_buf;
    ctx.file = nullptr;
    ctx.size = downloaded;
    ctx.index = 0;
    ctx.canvas_buf = (uint16_t *)canvas_buf;
    ctx.canvas_width = 800;
    ctx.canvas_height = 480;
    ctx.blocks = 0;
    ctx.scale = 0; // Calculated dynamically after jd_prepare
    
    esp_task_wdt_reset();
    Serial.printf("Campus JPEG prepare: %u bytes\n", (unsigned)downloaded);
    JRESULT res = jd_prepare(&jd, jpg_infunc, pool, 4096, &ctx);
    if (res != JDR_OK) {
        if (lvgl_port_lock(200)) {
            String msg = "ERR: jd_prepare=" + String(res) + "\nFile khong phai JPEG?";
            ui_chat_set_campus_news_status(msg.c_str());
            lvgl_port_unlock();
        }
        return false;
    }
    
    // Determine scale dynamically
    ctx.scale = 0;
    while (ctx.scale < 3 && ((jd.width >> ctx.scale) > 800 || (jd.height >> ctx.scale) > 480)) {
        ctx.scale++;
    }

    int decoded_w = jd.width >> ctx.scale;
    int decoded_h = jd.height >> ctx.scale;
    if (decoded_w <= 0) decoded_w = jd.width;
    if (decoded_h <= 0) decoded_h = jd.height;

    uint16_t *temp_buf = nullptr;
    // If image is smaller than screen, decode to temp buffer first and scale up
    if (decoded_w < 800 || decoded_h < 480) {
        temp_buf = (uint16_t *)ps_malloc(decoded_w * decoded_h * sizeof(uint16_t));
        if (temp_buf) {
            ctx.temp_buf = temp_buf;
            ctx.image_width = decoded_w;
            ctx.image_height = decoded_h;
            Serial.printf("Campus JPEG scale active: %dx%d -> 800x480\n", decoded_w, decoded_h);
        }
    }

    if (!temp_buf) {
        // Clear canvas in chunks to avoid watchdog
        size_t total = 800 * 480 * sizeof(lv_color_t);
        size_t chunk = 64 * 1024;
        for (size_t off = 0; off < total; off += chunk) {
            size_t len = (off + chunk > total) ? total - off : chunk;
            memset((uint8_t*)canvas_buf + off, 0, len);
            esp_task_wdt_reset();
        }
    }

    Serial.printf("Campus JPEG decode start: %ux%u scale=%u\n", jd.width, jd.height, ctx.scale);
    res = jd_decomp(&jd, jpg_outfunc, ctx.scale);
    Serial.printf("Campus JPEG decode result: %d, blocks=%u\n", res, (unsigned)ctx.blocks);
    if (res != JDR_OK) {
        if (temp_buf) {
            free(temp_buf);
        }
        if (lvgl_port_lock(200)) {
            String msg = "ERR: jd_decomp=" + String(res);
            ui_chat_set_campus_news_status(msg.c_str());
            lvgl_port_unlock();
        }
        return false;
    }

    if (temp_buf) {
        // Nearest-neighbor upscale to fill the 800x480 canvas
        for (int cy = 0; cy < 480; cy++) {
            int sy = (cy * decoded_h) / 480;
            int dest_row_offset = cy * 800;
            int src_row_offset = sy * decoded_w;
            for (int cx = 0; cx < 800; cx++) {
                int sx = (cx * decoded_w) / 800;
                ((uint16_t *)canvas_buf)[dest_row_offset + cx] = temp_buf[src_row_offset + sx];
            }
            if ((cy & 0x1F) == 0) {
                esp_task_wdt_reset();
            }
        }
        free(temp_buf);
        Serial.println("Upscaled campus image to fill screen");
    }
    
    if (lvgl_port_lock(200)) {
        ui_chat_set_campus_news_status("");
        ui_chat_refresh_news_canvas();
        lvgl_port_unlock();
    }
    return true;
}

static bool is_jpeg_file_name(const String &name)
{
    String lower = name;
    lower.toLowerCase();
    return lower.endsWith(".jpg") || lower.endsWith(".jpeg");
}

static String make_storage_path(const char *dir_path, const String &name)
{
    if (name.startsWith("/")) {
        return name;
    }
    if (strcmp(dir_path, "/") == 0) {
        return "/" + name;
    }
    return String(dir_path) + "/" + name;
}

static void scan_campus_images_from_dir(const char *dir_path)
{
    if (campus_image_count >= MAX_CAMPUS_IMAGES) {
        return;
    }

    File root = SD.open(dir_path);
    if (!root) {
        Serial.printf("Campus image dir missing: %s\n", dir_path);
        return;
    }
    if (!root.isDirectory()) {
        root.close();
        return;
    }

    File file = root.openNextFile();
    while (file && campus_image_count < MAX_CAMPUS_IMAGES) {
        if (!file.isDirectory()) {
            String name = file.name();
            if (is_jpeg_file_name(name)) {
                String path = make_storage_path(dir_path, name);
                campus_image_paths[campus_image_count++] = path;
                Serial.printf("Campus image[%u]: %s\n", campus_image_count, path.c_str());
            }
        }
        file.close();
        file = root.openNextFile();
    }
    root.close();
}

static bool decode_news_jpeg_file(File &file, size_t jpeg_size, const String &label)
{
    if (jpeg_size < 100) {
        Serial.printf("Campus image too small: %s\n", label.c_str());
        return false;
    }

    file.seek(0);
    uint8_t soi[2] = {0, 0};
    if (file.read(soi, 2) != 2 || soi[0] != 0xFF || soi[1] != 0xD8) {
        Serial.printf("Campus image is not JPEG: %s\n", label.c_str());
        return false;
    }
    file.seek(0);

    JDEC jd;
    static uint8_t *pool = nullptr;
    if (!pool) {
        pool = (uint8_t *)malloc(4096);
    }
    if (!pool) {
        Serial.println("Campus JPEG decode pool allocation failed");
        return false;
    }

    lv_color_t *canvas_buf = ui_chat_get_news_canvas_buffer();
    if (!canvas_buf) {
        Serial.println("Campus canvas buffer is NULL");
        return false;
    }

    JpgDecContext ctx;
    ctx.data = nullptr;
    ctx.file = &file;
    ctx.size = jpeg_size;
    ctx.index = 0;
    ctx.canvas_buf = (uint16_t *)canvas_buf;
    ctx.canvas_width = 800;
    ctx.canvas_height = 480;
    ctx.blocks = 0;
    ctx.scale = 0;

    esp_task_wdt_reset();
    Serial.printf("Campus local JPEG prepare: %u bytes\n", (unsigned)jpeg_size);
    JRESULT res = jd_prepare(&jd, jpg_infunc, pool, 4096, &ctx);
    if (res != JDR_OK) {
        Serial.printf("Campus JPEG prepare failed: %d file=%s\n", res, label.c_str());
        return false;
    }

    while (ctx.scale < 3 && ((jd.width >> ctx.scale) > 800 || (jd.height >> ctx.scale) > 480)) {
        ctx.scale++;
    }

    int decoded_w = jd.width >> ctx.scale;
    int decoded_h = jd.height >> ctx.scale;
    if (decoded_w <= 0) decoded_w = jd.width;
    if (decoded_h <= 0) decoded_h = jd.height;

    uint16_t *temp_buf = nullptr;
    if (decoded_w < 800 || decoded_h < 480) {
        temp_buf = (uint16_t *)ps_malloc(decoded_w * decoded_h * sizeof(uint16_t));
        if (temp_buf) {
            ctx.temp_buf = temp_buf;
            ctx.image_width = decoded_w;
            ctx.image_height = decoded_h;
            Serial.printf("Campus local JPEG scale active: %dx%d -> 800x480\n", decoded_w, decoded_h);
        }
    }

    if (!temp_buf) {
        size_t total = 800 * 480 * sizeof(lv_color_t);
        size_t chunk = 64 * 1024;
        for (size_t off = 0; off < total; off += chunk) {
            size_t len = (off + chunk > total) ? total - off : chunk;
            memset((uint8_t*)canvas_buf + off, 0, len);
            esp_task_wdt_reset();
        }
    }

    Serial.printf("Campus local JPEG decode start: %ux%u scale=%u\n", jd.width, jd.height, ctx.scale);
    res = jd_decomp(&jd, jpg_outfunc, ctx.scale);
    Serial.printf("Campus local JPEG decode result: %d, blocks=%u\n", res, (unsigned)ctx.blocks);
    if (res != JDR_OK) {
        if (temp_buf) free(temp_buf);
        Serial.printf("Campus JPEG decode failed: %d file=%s\n", res, label.c_str());
        return false;
    }

    if (temp_buf) {
        // Nearest-neighbor upscale to fill the 800x480 canvas
        for (int cy = 0; cy < 480; cy++) {
            int sy = (cy * decoded_h) / 480;
            int dest_row_offset = cy * 800;
            int src_row_offset = sy * decoded_w;
            for (int cx = 0; cx < 800; cx++) {
                int sx = (cx * decoded_w) / 800;
                ((uint16_t *)canvas_buf)[dest_row_offset + cx] = temp_buf[src_row_offset + sx];
            }
            if ((cy & 0x1F) == 0) {
                esp_task_wdt_reset();
            }
        }
        free(temp_buf);
        Serial.println("Upscaled campus local image to fill screen");
    }

    if (lvgl_port_lock(200)) {
        ui_chat_set_campus_news_status("");
        ui_chat_refresh_news_canvas();
        lvgl_port_unlock();
    }
    return true;
}

static bool load_local_campus_image(const String &path)
{
    File file = SD.open(path.c_str(), FILE_READ);
    if (!file) {
        Serial.printf("Cannot open campus image: %s\n", path.c_str());
        return false;
    }

    size_t file_size = file.size();
    if (file_size > LOCAL_JPEG_MAX) {
        file.close();
        Serial.printf("Campus image too large: %s size=%u\n", path.c_str(), (unsigned)file_size);
        return false;
    }

    bool ok = decode_news_jpeg_file(file, file_size, path);
    file.close();
    return ok;
}

static void startCampusNews()
{
    is_campus_news_active = true;
    campus_image_count = 0;
    current_campus_image_idx = 0;
    
    if (lvgl_port_lock(200)) {
        ui_chat_set_campus_news_status("Dang tai danh sach anh...");
        lvgl_port_unlock();
    }

    String body = httpGet("/api/v1/campus-images");
    if (body.length() > 0) {
        int pos = 0;
        while (campus_image_count < MAX_CAMPUS_IMAGES) {
            String url = extractJsonStringFrom(body, pos, "url", &pos);
            if (url.length() == 0) {
                break;
            }
            campus_image_paths[campus_image_count++] = url;
            Serial.printf("Campus image list[%u] = %s\n", (unsigned)campus_image_count - 1, url.c_str());
        }
    }

    if (campus_image_count > 0) {
        if (lvgl_port_lock(200)) {
            ui_chat_set_campus_news_status("Dang tai anh tu server...");
            lvgl_port_unlock();
        }
        if (!download_image(campus_image_paths[0])) {
            if (lvgl_port_lock(200)) {
                ui_chat_set_campus_news_status("Loi tai anh. Chuan bi anh khac...");
                lvgl_port_unlock();
            }
        }
        last_banner_change_ms = millis();
    } else {
        String fallback_url = String(SERVER_BASE_URL) + "/api/v1/campus-image";
        if (lvgl_port_lock(200)) {
            ui_chat_set_campus_news_status("Dang tai anh mac dinh...");
            lvgl_port_unlock();
        }
        if (!download_image(fallback_url)) {
            if (lvgl_port_lock(200)) {
                ui_chat_set_campus_news_status("Khong the tai anh hoac server chua co anh");
                lvgl_port_unlock();
            }
        }
    }
}

static void handle_campus_news()
{
    Serial.println("Campus News request");
    ui_chat_set_campus_news_status("Starting...");
    ui_chat_show_campus_news();
    pending_campus_news = true;
}

static volatile int pending_campus_swipe_dir = 0;
static volatile bool pending_campus_swipe = false;

static void handle_campus_news_swipe(int dir)
{
    pending_campus_swipe_dir = dir;
    pending_campus_swipe = true;
}

static void handle_campus_news_close()
{
    Serial.println("Campus News close");
    is_campus_news_active = false;
    ui_chat_hide_campus_news();
}


enum AudioUartParseState { UART_LINE, UART_LEN0, UART_LEN1, UART_PAYLOAD };
static AudioUartParseState audio_uart_state = UART_LINE;
static String audio_uart_line;
static uint16_t audio_frame_len = 0;
static uint16_t audio_frame_pos = 0;
static uint8_t audio_magic_pos = 0;
static const uint8_t audio_magic[] = {'A', 'U', 'D', '0'};

static void pumpAudioSerial(uint32_t duration_ms = 0)
{
    uint32_t start = millis();
    do {
        while (AudioSerial.available()) {
            uint8_t b = AudioSerial.read();
            if (audio_uart_state == UART_LINE) {
                if (b == audio_magic[audio_magic_pos]) {
                    audio_magic_pos++;
                    if (audio_magic_pos == sizeof(audio_magic)) {
                        audio_uart_line = "";
                        audio_frame_len = 0;
                        audio_uart_state = UART_LEN0;
                        audio_magic_pos = 0;
                    }
                } else {
                    audio_magic_pos = 0;
                    if (b == '\n') {
                        audio_uart_line.trim();
                        if (audio_uart_line.length() > 0) {
                            Serial.print("Audio board: ");
                            Serial.println(audio_uart_line);
                            if (audio_uart_line == "OK PLAY_DONE" || audio_uart_line == "OK PLAY_URL_DONE") {
                                ui_status("Online");
                            }
                        }
                        audio_uart_line = "";
                    } else if (b != '\r' && audio_uart_line.length() < 80) {
                        audio_uart_line += (char)b;
                    }
                }
            } else if (audio_uart_state == UART_LEN0) {
                audio_frame_len = b;
                audio_uart_state = UART_LEN1;
            } else if (audio_uart_state == UART_LEN1) {
                audio_frame_len |= ((uint16_t)b << 8);
                audio_frame_pos = 0;
                if (audio_frame_len == 0 || audio_frame_len > 1024) {
                    audio_uart_state = UART_LINE;
                } else {
                    audio_uart_state = UART_PAYLOAD;
                }
            } else if (audio_uart_state == UART_PAYLOAD) {
                if (recording && pcm_len < MAX_PCM_BYTES) {
                    pcm_buffer[pcm_len++] = b;
                }
                audio_frame_pos++;
                if (audio_frame_pos >= audio_frame_len) {
                    audio_uart_state = UART_LINE;
                }
            }
        }
        if (duration_ms == 0) {
            break;
        }
        delay(1);
    } while (millis() - start < duration_ms);
}



void setup()
{
    String title = "LVGL porting example";

    Serial.begin(115200);
    AudioSerial.begin(AUDIO_UART_BAUD, SERIAL_8N1, AUDIO_UART_RX, AUDIO_UART_TX);
    Serial.printf("[HMI UART] begin RX=%d TX=%d baud=%lu\n", AUDIO_UART_RX, AUDIO_UART_TX, (unsigned long)AUDIO_UART_BAUD);
    delay(100);
    Serial.printf("PSRAM size: %u bytes\n", ESP.getPsramSize());
    if (ESP.getPsramSize() == 0) {
        halt_on_error("PSRAM is not enabled. Enable OPI PSRAM in Arduino IDE for ESP32-S3 LCD 7.");
    }
    pcm_buffer = (uint8_t *)ps_malloc(MAX_PCM_BYTES);
    if (!pcm_buffer) {
        halt_on_error("Cannot allocate PSRAM audio buffer");
    }

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
    ui_chat_set_campus_news_callback(handle_campus_news);
    ui_chat_set_campus_news_close_callback(handle_campus_news_close);
    ui_chat_set_campus_news_swipe_callback(handle_campus_news_swipe);

    ui_chat_init();

    // Initialize a default session id at boot; a new one will be created on each "New chat".
    if (session_id.length() == 0) {
        session_id = newSessionId();
    }

    /* Release the mutex */
    lvgl_port_unlock();

    ensureWiFi();
    if (in_ap_mode) {
        if (lvgl_port_lock(200)) {
            ui_chat_show_wifi_config_screen("UniMate-AP", "192.168.4.1");
            lvgl_port_unlock();
        }
    } else {
        syncClockFromNtp();
        updateDashboardClock();
    }
}

void loop()
{
    if (in_ap_mode) {
        if (dns_server) dns_server->processNextRequest();
        if (captive_server) captive_server->handleClient();
    }

    static uint32_t wifi_disconnect_time = 0;
    if (!in_ap_mode) {
        if (WiFi.status() != WL_CONNECTED) {
            if (wifi_disconnect_time == 0) {
                wifi_disconnect_time = millis();
                WiFi.reconnect();
            } else if (millis() - wifi_disconnect_time > 60000) {
                Serial.println("WiFi disconnected > 60s. Fallback to AP mode.");
                ensureWiFi();
                if (in_ap_mode && lvgl_port_lock(200)) {
                    ui_chat_show_wifi_config_screen("UniMate-AP", "192.168.4.1");
                    lvgl_port_unlock();
                }
            }
        } else {
            wifi_disconnect_time = 0;
        }
    }

    if (pending_open_history) {
        pending_open_history = false;
        showHistorySessionsInUi();
    }
    if (pending_history_load) {
        pending_history_load = false;
        loadHistoryData(pending_history_idx);
    }
    if (pending_campus_news) {
        pending_campus_news = false;
        startCampusNews();
    }

    
    if (pending_campus_swipe) {
        pending_campus_swipe = false;
        if (is_campus_news_active && campus_image_count > 1) {
            if (pending_campus_swipe_dir == 1) { // Left swipe -> next
                current_campus_image_idx = (current_campus_image_idx + 1) % campus_image_count;
            } else if (pending_campus_swipe_dir == -1) { // Right swipe -> prev
                if (current_campus_image_idx == 0) {
                    current_campus_image_idx = campus_image_count - 1;
                } else {
                    current_campus_image_idx--;
                }
            }
            Serial.printf("Swipe to campus image index %d: %s\n", (int)current_campus_image_idx, campus_image_paths[current_campus_image_idx].c_str());
            download_image(campus_image_paths[current_campus_image_idx]);
            last_banner_change_ms = millis();
        }
    }

    if (is_campus_news_active && campus_image_count > 1) {
        if (millis() - last_banner_change_ms > 5000) { // Cycle every 5 seconds
            current_campus_image_idx = (current_campus_image_idx + 1) % campus_image_count;
            Serial.printf("Cycling campus image to index %d: %s\n", (int)current_campus_image_idx, campus_image_paths[current_campus_image_idx].c_str());
            download_image(campus_image_paths[current_campus_image_idx]);
            last_banner_change_ms = millis();
        }
    }

    pumpAudioSerial();

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
            AudioSerial.print("REC_STOP\n");
            Serial.println("[HMI UART TX] REC_STOP");
            ui_status("Uploading...");
            pumpAudioSerial(250);
            recording = false;
            Serial.printf("REC_STOP sent, pcm=%u\n", (unsigned)pcm_len);
            if (pcm_len == 0) {
                ui_add("assistant", "Khong thu duoc audio tu mic. Kiem tra firmware devkit audio/UART.");
                ui_status("Error");
            } else {
                handleVoiceChatResponse(postVoiceWav());
            }
        }
    }

    delay(2);
}
