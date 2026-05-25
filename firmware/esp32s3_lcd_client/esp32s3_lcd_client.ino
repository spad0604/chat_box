#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>


static const char *WIFI_SSID = "YOUR_WIFI";
static const char *WIFI_PASSWORD = "YOUR_PASSWORD";
static const char *SERVER_BASE_URL = "http://192.168.1.10:8000";

static HardwareSerial AudioSerial(2);
static const int AUDIO_UART_RX = 18;
static const int AUDIO_UART_TX = 17;
static const uint32_t AUDIO_UART_BAUD = 921600;

static void connectWiFi()
{
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        delay(300);
        Serial.print(".");
    }
    Serial.println();
    Serial.print("WiFi IP: ");
    Serial.println(WiFi.localIP());
}

static String postJson(const String &path, const String &json)
{
    HTTPClient http;
    String url = String(SERVER_BASE_URL) + path;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");

    int status = http.POST(json);
    String body = http.getString();
    http.end();

    Serial.printf("POST %s -> %d\n", url.c_str(), status);
    return body;
}

static String chatText(const String &message, const String &sessionId)
{
    String payload = "{\"message\":\"" + message + "\",\"session_id\":\"" + sessionId + "\"}";
    return postJson("/api/v1/chat/text", payload);
}

static void requestVoiceRecord()
{
    AudioSerial.print("REC_START\n");
}

static void playAudioUrl(const String &audioUrl)
{
    AudioSerial.print("PLAY_URL ");
    AudioSerial.print(audioUrl);
    AudioSerial.print("\n");
}

static void readAudioBoardEvents()
{
    while (AudioSerial.available()) {
        String line = AudioSerial.readStringUntil('\n');
        line.trim();
        if (line.length() > 0) {
            Serial.print("Audio board: ");
            Serial.println(line);
        }
    }
}

void setup()
{
    Serial.begin(115200);
    AudioSerial.begin(AUDIO_UART_BAUD, SERIAL_8N1, AUDIO_UART_RX, AUDIO_UART_TX);
    connectWiFi();

    // Sau khi LVGL/Waveshare init xong, gan callback UI:
    // ui_chat_set_send_callback(...)
    // ui_chat_set_mic_callback(...)
}

void loop()
{
    readAudioBoardEvents();
    delay(10);
}

