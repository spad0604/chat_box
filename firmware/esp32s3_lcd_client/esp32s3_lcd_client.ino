#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>


static const char *WIFI_SSID = "YOUR_WIFI";
static const char *WIFI_PASSWORD = "YOUR_PASSWORD";
static const char *SERVER_BASE_URL = "http://192.168.1.10:8000";

// Audio streaming format expected by the ESP32 audio board:
// - PCM 16-bit little-endian
// - mono, 16000 Hz
// - framed over UART as: 'S' 'P' 'K' '0' + uint16_le(len) + payload
static const uint32_t AUDIO_SAMPLE_RATE = 16000;
static const uint16_t AUDIO_FRAME_MAX_BYTES = 1024; // must match audio board buffer

static HardwareSerial AudioSerial(1);
// GPIO17/18 are used by the RGB LCD data bus on Waveshare ESP32-S3 LCD-7.
// Use different GPIOs for the UART link to the audio board.
static const int AUDIO_UART_RX = 15;
static const int AUDIO_UART_TX = 16;
static const uint32_t AUDIO_UART_BAUD = 460800;

static uint32_t audio_frames_sent = 0;
static uint32_t audio_bytes_sent = 0;
static uint32_t last_wifi_creds_sync_ms = 0;
static const uint32_t WIFI_CREDS_SYNC_INTERVAL_MS = 30000;

static inline uint16_t readLE16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t readLE32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void sendAudioBoardCommand(const char *cmd)
{
    AudioSerial.print(cmd);
    AudioSerial.print("\n");
}

static bool isUnreservedUrlChar(char c)
{
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == '~';
}

static String percentEncode(const String &input)
{
    static const char hex[] = "0123456789ABCDEF";
    String out;
    out.reserve(input.length() + 8);
    for (size_t i = 0; i < input.length(); i++) {
        uint8_t c = (uint8_t)input[i];
        if (isUnreservedUrlChar((char)c)) {
            out += (char)c;
        } else {
            out += '%';
            out += hex[(c >> 4) & 0x0f];
            out += hex[c & 0x0f];
        }
    }
    return out;
}

static String encodeWiFiToken(const String &input)
{
    if (input.length() == 0) {
        return "%00";
    }
    return percentEncode(input);
}

static void syncAudioBoardWiFiCredentials(bool force = false)
{
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }

    uint32_t now = millis();
    if (!force && now - last_wifi_creds_sync_ms < WIFI_CREDS_SYNC_INTERVAL_MS) {
        return;
    }
    last_wifi_creds_sync_ms = now;

    AudioSerial.print("WIFI_CREDS_V1 ");
    AudioSerial.print(percentEncode(WIFI_SSID));
    AudioSerial.print(" ");
    AudioSerial.print(encodeWiFiToken(WIFI_PASSWORD));
    AudioSerial.print("\n");

    Serial.printf("[UART->AUDIO] WIFI_CREDS_V1 ssid=%s pass_len=%u\n",
                  WIFI_SSID,
                  (unsigned)strlen(WIFI_PASSWORD));
}

static void sendSpkFrame(const uint8_t *data, uint16_t len)
{
    static const uint8_t header[4] = {'S', 'P', 'K', '0'};
    uint8_t len_bytes[2] = {(uint8_t)(len & 0xff), (uint8_t)(len >> 8)};
    AudioSerial.write(header, sizeof(header));
    AudioSerial.write(len_bytes, sizeof(len_bytes));
    AudioSerial.write(data, len);
    audio_frames_sent++;
    audio_bytes_sent += len;
}

static void sendPcmSilenceMs(uint32_t duration_ms)
{
    const uint32_t total_samples = (AUDIO_SAMPLE_RATE * duration_ms) / 1000;
    const uint32_t total_bytes = total_samples * 2;

    static uint8_t zeros[AUDIO_FRAME_MAX_BYTES];
    memset(zeros, 0, sizeof(zeros));

    uint32_t remaining = total_bytes;
    while (remaining > 0) {
        uint16_t chunk = (remaining > AUDIO_FRAME_MAX_BYTES) ? AUDIO_FRAME_MAX_BYTES : (uint16_t)remaining;
        chunk &= (uint16_t)~1; // keep 16-bit aligned
        if (chunk == 0) {
            break;
        }
        sendSpkFrame(zeros, chunk);
        remaining -= chunk;
        delay(1);
    }
}

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
    syncAudioBoardWiFiCredentials(true);
}

static bool parseWavDataOffset(const uint8_t *buf, size_t len, size_t &dataOffset, uint32_t &dataSize)
{
    // Minimal RIFF/WAVE parser to find the "data" chunk.
    // Returns true if a data chunk is found within [buf, len).
    if (len < 12) {
        return false;
    }
    if (!(buf[0] == 'R' && buf[1] == 'I' && buf[2] == 'F' && buf[3] == 'F')) {
        return false;
    }
    if (!(buf[8] == 'W' && buf[9] == 'A' && buf[10] == 'V' && buf[11] == 'E')) {
        return false;
    }

    size_t pos = 12;
    while (pos + 8 <= len) {
        const uint8_t *chunk = buf + pos;
        uint32_t chunkSize = readLE32(chunk + 4);
        size_t payloadPos = pos + 8;

        if (chunk[0] == 'd' && chunk[1] == 'a' && chunk[2] == 't' && chunk[3] == 'a') {
            dataOffset = payloadPos;
            dataSize = chunkSize;
            return dataOffset <= len;
        }

        // Chunks are word-aligned; pad to even.
        size_t advance = 8 + (size_t)chunkSize;
        if (advance & 1) {
            advance++;
        }
        pos += advance;
    }
    return false;
}

static bool streamUrlToAudioBoard(const String &audioUrl)
{
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi not connected");
        return false;
    }

    Serial.print("Streaming to audio board: ");
    Serial.println(audioUrl);

    HTTPClient http;
    http.setReuse(false);
    http.setTimeout(15000);
    if (!http.begin(audioUrl)) {
        Serial.println("HTTP begin failed");
        return false;
    }

    int status = http.GET();
    if (status != HTTP_CODE_OK) {
        Serial.printf("HTTP GET failed: %d\n", status);
        http.end();
        return false;
    }

    WiFiClient *stream = http.getStreamPtr();
    if (!stream) {
        Serial.println("HTTP stream unavailable");
        http.end();
        return false;
    }

    audio_frames_sent = 0;
    audio_bytes_sent = 0;

    sendAudioBoardCommand("PLAY_PCM_START");

    // Read initial header bytes for WAV detection.
    uint8_t headerBuf[256];
    size_t headerLen = 0;
    uint32_t wavDataSize = 0;
    size_t wavDataOffset = 0;
    bool isWav = false;
    bool dataOffsetKnown = false;

    uint32_t t0 = millis();
    while (headerLen < sizeof(headerBuf) && (millis() - t0) < 2000) {
        int avail = stream->available();
        if (avail <= 0) {
            delay(5);
            continue;
        }
        size_t want = sizeof(headerBuf) - headerLen;
        if ((size_t)avail < want) {
            want = (size_t)avail;
        }
        int r = stream->readBytes(headerBuf + headerLen, want);
        if (r <= 0) {
            break;
        }
        headerLen += (size_t)r;
        if (headerLen >= 12) {
            size_t off = 0;
            uint32_t size = 0;
            if (parseWavDataOffset(headerBuf, headerLen, off, size)) {
                isWav = true;
                wavDataOffset = off;
                wavDataSize = size;
                dataOffsetKnown = true;
                break;
            }
            // Not a WAV header, or header not complete enough to find data yet.
            if (!(headerBuf[0] == 'R' && headerBuf[1] == 'I' && headerBuf[2] == 'F' && headerBuf[3] == 'F')) {
                isWav = false;
                break;
            }
        }
    }

    uint8_t frameBuf[AUDIO_FRAME_MAX_BYTES];
    uint16_t framePos = 0;
    uint8_t tail[2];
    uint8_t tailLen = 0;
    uint32_t bytesRemaining = 0;
    bool limitToDataSize = false;

    // If WAV detected and data offset known, skip header and start at data chunk.
    // If the server returns raw PCM, just play from the beginning.
    size_t startPos = 0;
    if (dataOffsetKnown) {
        startPos = wavDataOffset;
        bytesRemaining = wavDataSize;
        limitToDataSize = true;
        Serial.printf("WAV detected: dataOffset=%u dataSize=%lu\n", (unsigned)wavDataOffset, (unsigned long)wavDataSize);
    } else {
        startPos = 0;
        Serial.println("WAV header not detected; treating response as raw PCM16 mono");
    }

    auto pushAudioBytes = [&](const uint8_t *data, size_t len) {
        // Ensure 16-bit alignment by carrying 0/1 leftover byte.
        if (len == 0) {
            return;
        }
        size_t idx = 0;
        if (tailLen == 1) {
            // Need one more byte to complete a sample.
            tail[1] = data[0];
            if (framePos + 2 <= AUDIO_FRAME_MAX_BYTES) {
                frameBuf[framePos++] = tail[0];
                frameBuf[framePos++] = tail[1];
            }
            tailLen = 0;
            idx = 1;
        }

        // Copy even number of bytes.
        size_t evenLen = (len - idx) & ~((size_t)1);
        while (evenLen > 0) {
            uint16_t can = (uint16_t)(AUDIO_FRAME_MAX_BYTES - framePos);
            if (can == 0) {
                sendSpkFrame(frameBuf, framePos);
                framePos = 0;
                can = AUDIO_FRAME_MAX_BYTES;
            }
            size_t take = evenLen;
            if (take > can) {
                take = can;
            }
            memcpy(frameBuf + framePos, data + idx, take);
            framePos += (uint16_t)take;
            idx += take;
            evenLen -= take;
        }

        // Save leftover odd byte.
        if (idx < len) {
            tail[0] = data[idx];
            tailLen = 1;
        }
    };

    // Feed any initial bytes already read.
    if (startPos < headerLen) {
        size_t initialLen = headerLen - startPos;
        if (limitToDataSize && initialLen > bytesRemaining) {
            initialLen = bytesRemaining;
        }
        pushAudioBytes(headerBuf + startPos, initialLen);
        if (limitToDataSize) {
            bytesRemaining -= (uint32_t)initialLen;
        }
    }

    // Stream remaining bytes.
    uint8_t netBuf[512];
    uint32_t last_data_ms = millis();
    while (stream->connected() || stream->available()) {
        if (limitToDataSize && bytesRemaining == 0) {
            break;
        }

        int avail = stream->available();
        if (avail <= 0) {
            // Avoid a busy loop. If the server stalls too long, abort.
            if (millis() - last_data_ms > 3000) {
                Serial.println("HTTP stream stalled; aborting");
                break;
            }
            delay(2);
            continue;
        }
        last_data_ms = millis();

        size_t want = sizeof(netBuf);
        if ((size_t)avail < want) {
            want = (size_t)avail;
        }
        if (limitToDataSize && want > bytesRemaining) {
            want = bytesRemaining;
        }

        int r = stream->readBytes(netBuf, want);
        if (r <= 0) {
            break;
        }
        pushAudioBytes(netBuf, (size_t)r);
        if (limitToDataSize) {
            bytesRemaining -= (uint32_t)r;
        }
        delay(1);
    }

    if (framePos > 0) {
        sendSpkFrame(frameBuf, framePos);
    }

    // Tail byte can't be played; drop it.
    tailLen = 0;

    // A bit of silence helps avoid pops at stop.
    sendPcmSilenceMs(250);
    sendAudioBoardCommand("PLAY_PCM_STOP");

    http.end();

    Serial.printf("Audio stream done: frames=%lu bytes=%lu\n", (unsigned long)audio_frames_sent, (unsigned long)audio_bytes_sent);
    return true;
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
    // ESP32-S3 downloads the audio and streams PCM frames to the audio board.
    // This avoids needing WiFi/HTTP on the audio-only MCU.
    streamUrlToAudioBoard(audioUrl);
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
    // Simple USB serial test:
    // - Type: PLAY_URL http://<server>/path/to.wav
    // - Or: REC_START
    if (Serial.available()) {
        String line = Serial.readStringUntil('\n');
        line.trim();
        if (line == "REC_START") {
            requestVoiceRecord();
        } else if (line.startsWith("PLAY_URL ")) {
            playAudioUrl(line.substring(9));
        } else if (line.length() > 0) {
            Serial.print("Unknown cmd: ");
            Serial.println(line);
        }
    }
    readAudioBoardEvents();
    syncAudioBoardWiFiCredentials();
    delay(10);
}

