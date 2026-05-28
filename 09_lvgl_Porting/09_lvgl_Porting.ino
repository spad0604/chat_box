#include <Arduino.h>
#include <esp_display_panel.hpp>
#include <WiFi.h>
#include <WiFiClient.h>

#include <lvgl.h>
#include "lvgl_v8_port.h"
#include "ui_chat.h"

using namespace esp_panel::drivers;
using namespace esp_panel::board;

static const char *WIFI_SSID = "spad0604";
static const char *WIFI_PASSWORD = "06042004";
static const char *SERVER_HOST = "54.206.118.226";
static const uint16_t SERVER_PORT = 8000;
static const char *SERVER_BASE_URL = "http://54.206.118.226:8000";

static HardwareSerial AudioSerial(2);
static const int AUDIO_UART_RX = 18;
static const int AUDIO_UART_TX = 17;
static const uint32_t AUDIO_UART_BAUD = 921600;

static const uint32_t SAMPLE_RATE = 16000;
static const uint16_t BITS_PER_SAMPLE = 16;
static const uint16_t CHANNELS = 1;
static const size_t MAX_PCM_BYTES = SAMPLE_RATE * 2 * 8;

static String session_id = "esp32-session";
static String pending_text;
static volatile bool pending_text_send = false;
static volatile bool pending_voice_toggle = false;
static bool recording = false;
static uint8_t *pcm_buffer = nullptr;
static size_t pcm_len = 0;

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

static void playAudioUrl(const String &audio_url)
{
    if (audio_url.length() == 0) {
        return;
    }
    AudioSerial.print("PLAY_URL ");
    AudioSerial.print(absoluteAudioUrl(audio_url));
    AudioSerial.print("\n");
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
}

static void handle_history(uint8_t index)
{
    Serial.print("Open history index: ");
    Serial.println(index);
}

/**
 * To use the built-in examples and demos of LVGL uncomment the includes below respectively.
 */
 // #include <demos/lv_demos.h>
 // #include <examples/lv_examples.h>

void setup()
{
    String title = "LVGL porting example";

    Serial.begin(115200);
    AudioSerial.begin(AUDIO_UART_BAUD, SERIAL_8N1, AUDIO_UART_RX, AUDIO_UART_TX);
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
    ui_chat_set_history_callback(handle_history);
    ui_chat_init();

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

    if (pending_text_send) {
        pending_text_send = false;
        sendTextToServer(pending_text);
    }

    if (pending_voice_toggle) {
        pending_voice_toggle = false;
        bool should_record = ui_chat_is_mic_recording();
        if (should_record && !recording) {
            pcm_len = 0;
            recording = true;
            AudioSerial.print("REC_START\n");
            ui_status("Recording...");
            Serial.println("REC_START sent");
        } else if (!should_record && recording) {
            recording = false;
            AudioSerial.print("REC_STOP\n");
            ui_status("Uploading...");
            Serial.printf("REC_STOP sent, pcm=%u\n", (unsigned)pcm_len);
            handleChatResponse(postVoiceWav());
        }
    }

    delay(2);
}
