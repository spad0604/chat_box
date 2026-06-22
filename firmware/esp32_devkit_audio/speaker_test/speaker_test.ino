/*
 * ESP32 DevKit + MAX98357A continuous speaker test.
 *
 * Wiring:
 *   GPIO25 -> MAX98357A DIN
 *   GPIO26 -> MAX98357A LRC / WS
 *   GPIO27 -> MAX98357A BCLK
 *   GND    -> GND
 */
#include <Arduino.h>
#include <WiFi.h>
#include <driver/i2s.h>

// ===================== CHANGE THESE IF NEEDED =====================
static const char *WIFI_SSID = "spad0604";
static const char *WIFI_PASS = "06042004";
static const char *AUDIO_URL =
    "http://54.206.118.226:8000/api/v1/audio-esp32/"
    "reply_d42eb262ffb9492a827f1297fdfeeff2.wav?rate=16000&bits=8";

static const int SPK_DIN = 25;
static const int SPK_LRC = 26;
static const int SPK_BCLK = 27;
static const uint32_t SPEAKER_SAMPLE_RATE = 16000;
static const i2s_port_t I2S_SPK_PORT = I2S_NUM_0;

struct WavInfo {
  uint16_t channels = 0;
  uint16_t bitsPerSample = 0;
  uint32_t sampleRate = 0;
  uint32_t dataSize = 0;
};

static uint8_t networkBuffer[2048];
static int16_t outputBuffer[1024];

static uint16_t readLE16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t readLE32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool readExact(WiFiClient &client, uint8_t *buffer, size_t length) {
  size_t received = 0;
  uint32_t lastDataMs = millis();
  while (received < length) {
    int available = client.available();
    if (available > 0) {
      int count = client.read(buffer + received, min((size_t)available, length - received));
      if (count > 0) {
        received += (size_t)count;
        lastDataMs = millis();
      }
    } else if (!client.connected() || millis() - lastDataMs > 8000) {
      return false;
    } else {
      delay(1);
    }
  }
  return true;
}

static bool skipHttpHeaders(WiFiClient &client) {
  String status = client.readStringUntil('\n');
  status.trim();
  if (!status.startsWith("HTTP/") || status.indexOf(" 200 ") < 0) {
    Serial.printf("HTTP error: %s\n", status.c_str());
    return false;
  }

  while (true) {
    String line = client.readStringUntil('\n');
    if (line == "\r" || line.length() == 0) return true;
  }
}

static bool parseWavHeader(WiFiClient &client, WavInfo &info) {
  uint8_t header[12];
  if (!readExact(client, header, sizeof(header)) || memcmp(header, "RIFF", 4) != 0 ||
      memcmp(header + 8, "WAVE", 4) != 0) {
    Serial.println("Not a RIFF/WAVE file");
    return false;
  }

  bool hasFormat = false;
  while (client.connected()) {
    uint8_t chunkHeader[8];
    if (!readExact(client, chunkHeader, sizeof(chunkHeader))) return false;
    uint32_t chunkSize = readLE32(chunkHeader + 4);
    const bool hasPadding = (chunkSize & 1) != 0;

    if (memcmp(chunkHeader, "fmt ", 4) == 0) {
      if (chunkSize < 16 || chunkSize > sizeof(networkBuffer) ||
          !readExact(client, networkBuffer, chunkSize)) return false;
      uint16_t format = readLE16(networkBuffer);
      info.channels = readLE16(networkBuffer + 2);
      info.sampleRate = readLE32(networkBuffer + 4);
      info.bitsPerSample = readLE16(networkBuffer + 14);
      if (format != 1 || (info.channels != 1 && info.channels != 2) ||
          (info.bitsPerSample != 8 && info.bitsPerSample != 16)) {
        Serial.println("Only PCM WAV, mono/stereo, 8/16-bit is supported");
        return false;
      }
      hasFormat = true;
    } else if (memcmp(chunkHeader, "data", 4) == 0) {
      if (!hasFormat) return false;
      info.dataSize = chunkSize;
      return true;
    } else {
      while (chunkSize > 0) {
        size_t bytes = min((uint32_t)sizeof(networkBuffer), chunkSize);
        if (!readExact(client, networkBuffer, bytes)) return false;
        chunkSize -= bytes;
      }
    }

    // RIFF chunks are word-aligned.
    if (hasPadding) {
      uint8_t padding;
      if (!readExact(client, &padding, 1)) return false;
    }
  }
  return false;
}

static bool parseHttpUrl(const char *url, String &host, uint16_t &port, String &path) {
  String value(url);
  if (!value.startsWith("http://")) return false;
  value.remove(0, 7);
  int slash = value.indexOf('/');
  String authority = slash < 0 ? value : value.substring(0, slash);
  path = slash < 0 ? "/" : value.substring(slash);
  int colon = authority.indexOf(':');
  host = colon < 0 ? authority : authority.substring(0, colon);
  port = colon < 0 ? 80 : (uint16_t)authority.substring(colon + 1).toInt();
  return host.length() > 0 && port > 0;
}

static bool setupSpeaker() {
  i2s_config_t config = {};
  config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  config.sample_rate = SPEAKER_SAMPLE_RATE;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  // MAX98357A uses the standard Philips I2S timing, not left-justified/MSB.
  // Keep this identical to the working audio sketch.
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 12;
  config.dma_buf_len = 512;
  config.use_apll = false;
  config.tx_desc_auto_clear = true;

  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = SPK_BCLK;
  pins.ws_io_num = SPK_LRC;
  pins.data_out_num = SPK_DIN;
  pins.data_in_num = I2S_PIN_NO_CHANGE;

  return i2s_driver_install(I2S_SPK_PORT, &config, 0, nullptr) == ESP_OK &&
         i2s_set_pin(I2S_SPK_PORT, &pins) == ESP_OK;
}

static bool playOnce() {
  String host, path;
  uint16_t port;
  if (!parseHttpUrl(AUDIO_URL, host, port, path)) {
    Serial.println("AUDIO_URL must start with http://");
    return false;
  }

  WiFiClient client;
  client.setTimeout(8000);
  if (!client.connect(host.c_str(), port)) {
    Serial.println("Audio-server connection failed");
    return false;
  }
  client.printf("GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                path.c_str(), host.c_str());
  if (!skipHttpHeaders(client)) return false;

  WavInfo wav;
  if (!parseWavHeader(client, wav) || wav.sampleRate != SPEAKER_SAMPLE_RATE) {
    Serial.printf("Expected a %lu Hz WAV\n", (unsigned long)SPEAKER_SAMPLE_RATE);
    return false;
  }
  Serial.printf("Playing: %lu Hz, %u channel, %u-bit, %lu bytes\n",
                (unsigned long)wav.sampleRate, wav.channels, wav.bitsPerSample,
                (unsigned long)wav.dataSize);

  const size_t inputFrameBytes = wav.channels * (wav.bitsPerSample / 8);
  uint32_t remaining = wav.dataSize;
  while (remaining >= inputFrameBytes) {
    size_t wanted = min((uint32_t)sizeof(networkBuffer), remaining);
    wanted -= wanted % inputFrameBytes;
    if (!readExact(client, networkBuffer, wanted)) return false;
    remaining -= wanted;

    size_t frames = wanted / inputFrameBytes;
    size_t frameOffset = 0;
    while (frameOffset < frames) {
      size_t count = min((size_t)1024, frames - frameOffset);
      for (size_t i = 0; i < count; ++i) {
        const uint8_t *frame = networkBuffer + (frameOffset + i) * inputFrameBytes;
        int32_t sample = wav.bitsPerSample == 8 ? ((int32_t)frame[0] - 128) << 8
                                                 : (int16_t)readLE16(frame);
        if (wav.channels == 2) {
          int32_t right = wav.bitsPerSample == 8 ? ((int32_t)frame[wav.bitsPerSample / 8] - 128) << 8
                                                   : (int16_t)readLE16(frame + 2);
          sample = (sample + right) / 2;
        }
        outputBuffer[i] = (int16_t)sample;
      }
      size_t written = 0;
      if (i2s_write(I2S_SPK_PORT, outputBuffer, count * sizeof(int16_t), &written,
                    pdMS_TO_TICKS(1000)) != ESP_OK || written != count * sizeof(int16_t)) {
        Serial.println("I2S write failed");
        return false;
      }
      frameOffset += count;
    }
  }
  client.stop();
  return remaining == 0;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("Connecting Wi-Fi: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
  }
  Serial.printf("\nWi-Fi connected: %s\n", WiFi.localIP().toString().c_str());

  if (!setupSpeaker()) {
    Serial.println("MAX98357A I2S setup failed");
    while (true) delay(1000);
  }
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    delay(1000);
    return;
  }

  bool ok = playOnce();
  Serial.println(ok ? "Playback complete; replaying..." : "Playback failed; retrying...");
  delay(ok ? 500 : 3000);
}
