#include <Arduino.h>
#include <WiFi.h>
#include <driver/i2s.h>
#include <driver/adc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_heap_caps.h>
#include <FS.h>
#include <LittleFS.h>
#include <Preferences.h>

// ===================== USER CONFIG =====================
static const char *SERVER_BASE_URL = "http://54.206.118.226:8000";
static const char *TEST_AUDIO_URL = "http://54.206.118.226:8000/api/v1/audio-esp32/reply_d42eb262ffb9492a827f1297fdfeeff2.wav?rate=16000&bits=8";
static const bool AUTO_LOOP_TEST_AUDIO = false;
static const char *TEST_WIFI_SSID = "spad0604";
static const char *TEST_WIFI_PASS = "06042004";
static const char *WIFI_PREF_NAMESPACE = "wifi";
static const char *WIFI_PREF_SSID_KEY = "ssid";
static const char *WIFI_PREF_PASS_KEY = "pass";

// HMI UART
static HardwareSerial HmiSerial(2);
static const int HMI_RX = 16;
static const int HMI_TX = 17;
static const uint32_t HMI_BAUD = 460800;
static const uint32_t USB_DEBUG_BAUD = 115200;

// INMP441 microphone pins
static const int MIC_WS = 15;
static const int MIC_SCK = 14;
static const int MIC_SD = 32;

// MAX98357A speaker pins
static const int SPK_DIN = 25;
static const int SPK_LRC = 26;
static const int SPK_BCLK = 27;

// If your MAX98357A SD/EN pin is connected to ESP32, use another pin, NOT GPIO33.
// GPIO33 is used for the volume potentiometer ADC. Example:
// #define AMP_SD_PIN 13

#define SPEAKER_SAMPLE_RATE 16000
#define MIC_SAMPLE_RATE     16000

// GPIO33 potentiometer volume. Do NOT use GPIO33 as MAX98357A SD/EN.
// 5 stable steps based on ADC raw value:
//   0..999      -> step 0, mute
//   1000..1999  -> step 1
//   2000..2999  -> step 2
//   3000..3999  -> step 3
//   4000+       -> step 4
static const int VOLUME_ADC_PIN = 33;
#define VOLUME_ADC_READ_INTERVAL_MS 250
#define VOLUME_ADC_HYSTERESIS      120
static const uint16_t VOLUME_GAIN_PERMILLE[5] = {0, 500, 1000, 1750, 2500};
static volatile uint16_t currentVolumeGainPermille = 1000;
static uint8_t currentVolumeStep = 4;
static uint32_t lastVolumeReadMs = 0;

// Keep chunks large enough for I2S stability, but not so large that WiFi runs out of heap.
#define PCM_FRAMES_PER_CHUNK 2048
#define MIC_FRAMES_PER_CHUNK 160

// Ring buffer between HTTP download and I2S playback.
// Keep this on heap, not static DRAM, because ESP32 DevKit V1 has limited .dram0.bss.
// Bigger buffers trade RAM for smoother playback. Leave enough heap for WiFi/TCP.
#define AUDIO_RING_BYTES      (64 * 1024)
#define AUDIO_PREBUFFER_BYTES (48 * 1024)
#define AUDIO_LOW_WATER_BYTES (16 * 1024)
#define AUDIO_TASK_STACK      3072

// Keep speaker on I2S0 because your speaker-only test is stable with this.
static const i2s_port_t I2S_SPK_PORT = I2S_NUM_0;
static const i2s_port_t I2S_MIC_PORT = I2S_NUM_1;

static String hmi_line;
static bool mic_installed = false;
static bool recording_enabled = false;
static bool speaker_installed = false;
static volatile bool speaker_playing = false;

// Global buffers to avoid stack pressure.
static int16_t inputBuffer[PCM_FRAMES_PER_CHUNK * 2];
static int16_t outputBuffer[PCM_FRAMES_PER_CHUNK * 2];
static int16_t silenceBuffer[PCM_FRAMES_PER_CHUNK * 2];
static int32_t micRawBuffer[MIC_FRAMES_PER_CHUNK];
static int16_t micPcmBuffer[MIC_FRAMES_PER_CHUNK];

// Audio playback ring buffer. Single producer: HTTP reader. Single consumer: I2S task.
// Do not allocate this statically, otherwise .dram0.bss can overflow on ESP32.
static uint8_t *audioRing = nullptr;
static uint8_t audioTaskBuffer[PCM_FRAMES_PER_CHUNK * sizeof(int16_t)];
static SemaphoreHandle_t audioRingMutex = nullptr;
static TaskHandle_t audioTaskHandle = nullptr;
static size_t audioRingHead = 0;
static size_t audioRingTail = 0;
static size_t audioRingFillBytes = 0;
static volatile bool audioPlaybackActive = false;
static volatile bool audioPlaybackStopping = false;
static volatile bool audioPlaybackFinished = true;
static volatile uint32_t audioUnderruns = 0;
static volatile uint32_t audioPlaybackFrameIndex = 0;
static volatile uint16_t audioPlaybackGainPermille = 1000;

struct WavInfo {
  uint32_t sampleRate = 0;
  uint16_t bitsPerSample = 0;
  uint16_t channels = 0;
  uint32_t dataSize = 0;
};

static void processCommandInputs(bool allowPlaybackCommand);
static void writeToneMs(uint32_t ms, uint32_t frequencyHz);

static inline uint16_t readLE16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t readLE32(const uint8_t *p) {
  return (uint32_t)p[0] |
         ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint8_t classifyVolumeAdc(uint16_t adc, uint8_t previousLevel) {
  // Hysteresis prevents volume step flapping near thresholds.
  switch (previousLevel) {
    case 0:
      return (adc > 1000 + VOLUME_ADC_HYSTERESIS) ? 1 : 0;
    case 1:
      if (adc < 1000 - VOLUME_ADC_HYSTERESIS) return 0;
      if (adc > 2000 + VOLUME_ADC_HYSTERESIS) return 2;
      return 1;
    case 2:
      if (adc < 2000 - VOLUME_ADC_HYSTERESIS) return 1;
      if (adc > 3000 + VOLUME_ADC_HYSTERESIS) return 3;
      return 2;
    case 3:
      if (adc < 3000 - VOLUME_ADC_HYSTERESIS) return 2;
      if (adc > 4000 + VOLUME_ADC_HYSTERESIS) return 4;
      return 3;
    default:
      return (adc < 4000 - VOLUME_ADC_HYSTERESIS) ? 3 : 4;
  }
}

static void updateVolumeStep(bool forceLog = false) {
  uint32_t now = millis();
  if (!forceLog && now - lastVolumeReadMs < VOLUME_ADC_READ_INTERVAL_MS) return;
  lastVolumeReadMs = now;

  // Use legacy ADC1 API to avoid Arduino ESP32 ADC driver_ng conflict.
  uint32_t sum = 0;
  for (int i = 0; i < 4; i++) {
    int raw = adc1_get_raw(ADC1_CHANNEL_5); // GPIO33 = ADC1_CH5
    if (raw < 0) raw = 0;
    if (raw > 4095) raw = 4095;
    sum += (uint16_t)raw;
    delayMicroseconds(120);
  }

  uint16_t adc = (uint16_t)(sum / 4);
  uint8_t oldLevel = currentVolumeStep;
  uint8_t newLevel = classifyVolumeAdc(adc, oldLevel);

  if (forceLog || newLevel != oldLevel) {
    currentVolumeStep = newLevel;
    currentVolumeGainPermille = VOLUME_GAIN_PERMILLE[newLevel];
    Serial.printf("Volume step=%u adc=%u gain=%u/1000\n",
                  (unsigned)newLevel,
                  (unsigned)adc,
                  (unsigned)currentVolumeGainPermille);
  }
}

static int16_t amplifySample(int16_t input, uint16_t gainPermille) {
  int32_t value = (int32_t)input;
  value = (value * (int32_t)gainPermille) / 1000;
  if (value > 32767) value = 32767;
  if (value < -32768) value = -32768;
  return (int16_t)value;
}

static int16_t wavFrameToPcm16(const uint8_t *frame, const WavInfo &wav) {
  if (wav.bitsPerSample == 8) {
    if (wav.channels == 1) {
      return (int16_t)(((int32_t)frame[0] - 128) << 8);
    }
    int32_t left = (int32_t)frame[0] - 128;
    int32_t right = (int32_t)frame[1] - 128;
    return (int16_t)(((left + right) / 2) << 8);
  }

  if (wav.channels == 1) {
    return (int16_t)readLE16(frame);
  }

  int16_t left = (int16_t)readLE16(frame);
  int16_t right = (int16_t)readLE16(frame + 2);
  return (int16_t)(((int32_t)left + (int32_t)right) / 2);
}

static int16_t squareToneSample(uint32_t frameIndex, uint32_t frequencyHz, uint16_t gainPermille) {
  uint32_t halfPeriod = SPEAKER_SAMPLE_RATE / (frequencyHz * 2);
  if (halfPeriod == 0) halfPeriod = 1;
  int16_t sample = ((frameIndex / halfPeriod) & 1) ? 12000 : -12000;
  return amplifySample(sample, gainPermille);
}

static int16_t micSampleToPcm16(int32_t sample) {
  // INMP441 gives 24-bit signed audio left-aligned in 32-bit I2S word.
  // This shift is the same style as your earlier working mic-test code.
  int32_t s = sample >> 14;
  s *= 2;
  if (s > 32767) s = 32767;
  if (s < -32768) s = -32768;
  return (int16_t)s;
}

static void micPinsHighZ() {
  pinMode(MIC_WS, INPUT);
  pinMode(MIC_SCK, INPUT);
  pinMode(MIC_SD, INPUT);
}

static String absoluteAudioUrl(const String &audio_url) {
  if (audio_url.startsWith("http://") || audio_url.startsWith("https://")) return audio_url;
  if (audio_url.startsWith("/")) return String(SERVER_BASE_URL) + audio_url;
  return audio_url;
}

static String esp32AudioUrl(const String &audio_url) {
  String url = absoluteAudioUrl(audio_url);
  int audioPath = url.indexOf("/api/v1/audio/");
  if (audioPath >= 0 && url.indexOf("/api/v1/audio-esp32/") < 0) {
    url = url.substring(0, audioPath) + "/api/v1/audio-esp32/" + url.substring(audioPath + 14);
  }

  if (url.indexOf("/api/v1/audio-esp32/") >= 0) {
    if (url.indexOf("rate=") < 0) {
      url += (url.indexOf('?') >= 0) ? "&rate=16000" : "?rate=16000";
    }
    if (url.indexOf("bits=") < 0) {
      url += "&bits=8";
    }
  }

  return url;
}

static int findAudioUrlStart(const String &line);
static String sliceAudioUrl(const String &line, int start);

static String extractAudioUrl(String line) {
  line.trim();
  int playKey = line.indexOf("PLAY_URL ");
  if (playKey >= 0) {
    String tail = line.substring(playKey + 9);
    int urlStart = findAudioUrlStart(tail);
    if (urlStart >= 0) {
      return sliceAudioUrl(tail, urlStart);
    }
    tail.trim();
    return sliceAudioUrl(tail, 0);
  }

  int rawUrlStart = findAudioUrlStart(line);
  if (rawUrlStart >= 0) {
    return sliceAudioUrl(line, rawUrlStart);
  }

  int key = line.indexOf("\"audio_url\"");
  if (key < 0) key = line.indexOf("audio_url");
  if (key < 0) return "";

  int colon = line.indexOf(':', key);
  if (colon < 0) return "";

  int start = colon + 1;
  while (start < line.length() && (line[start] == ' ' || line[start] == '\t' || line[start] == '"')) start++;

  int end = start;
  while (end < line.length() && line[end] != '"' && line[end] != ',' && line[end] != '}' && line[end] != '\r' && line[end] != '\n') end++;

  String url = line.substring(start, end);
  url.trim();
  return url;
}

static int findAudioUrlStart(const String &line) {
  int http = line.indexOf("http://");
  int https = line.indexOf("https://");
  int api = line.indexOf("/api/v1/audio/");

  int start = -1;
  if (http >= 0) start = http;
  if (https >= 0 && (start < 0 || https < start)) start = https;
  if (api >= 0 && (start < 0 || api < start)) start = api;
  return start;
}

static String sliceAudioUrl(const String &line, int start) {
  int end = start;
  while (end < line.length()) {
    char ch = line[end];
    if ((uint8_t)ch < 33 || (uint8_t)ch > 126 ||
        ch == ' ' || ch == '\t' || ch == '"' || ch == '\'' ||
        ch == ',' || ch == '}' || ch == '\r' || ch == '\n') {
      break;
    }
    end++;
  }

  String url = line.substring(start, end);
  url.trim();
  return url;
}

static bool parseHttpUrl(const String &url, String &host, uint16_t &port, String &path) {
  if (!url.startsWith("http://")) {
    Serial.println("Only http:// URL is supported in this build");
    return false;
  }

  int hostStart = 7;
  int pathStart = url.indexOf('/', hostStart);
  if (pathStart < 0) {
    Serial.println("Invalid URL: no path");
    return false;
  }

  String hostPort = url.substring(hostStart, pathStart);
  path = url.substring(pathStart);

  int colon = hostPort.indexOf(':');
  if (colon >= 0) {
    host = hostPort.substring(0, colon);
    port = (uint16_t)hostPort.substring(colon + 1).toInt();
    if (port == 0) port = 80;
  } else {
    host = hostPort;
    port = 80;
  }

  return host.length() > 0 && path.length() > 0;
}

static bool readExact(WiFiClient &client, uint8_t *buffer, size_t len, uint32_t timeoutMs = 5000) {
  size_t total = 0;
  uint32_t lastData = millis();

  while (total < len) {
    int avail = client.available();
    if (avail > 0) {
      size_t want = len - total;
      if (want > (size_t)avail) want = avail;
      int n = client.read(buffer + total, want);
      if (n > 0) {
        total += (size_t)n;
        lastData = millis();
      }
    } else {
      delay(1);
    }

    if (!client.connected() && client.available() <= 0) return false;
    if (millis() - lastData > timeoutMs) return false;
  }
  return true;
}

static bool readHttpLine(WiFiClient &client, char *line, size_t maxLen) {
  size_t idx = 0;
  uint32_t lastData = millis();

  while (idx < maxLen - 1) {
    if (client.available()) {
      char c = (char)client.read();
      if (c == '\n') {
        line[idx] = '\0';
        return true;
      }
      if (c != '\r') line[idx++] = c;
      lastData = millis();
    } else {
      delay(1);
    }

    if (millis() - lastData > 8000) {
      line[idx] = '\0';
      return false;
    }
  }

  line[idx] = '\0';
  return true;
}

static bool skipHttpHeaders(WiFiClient &client) {
  char line[192];

  if (!readHttpLine(client, line, sizeof(line))) {
    Serial.println("Cannot read HTTP status line");
    return false;
  }

  Serial.print("HTTP: ");
  Serial.println(line);

  if (strstr(line, "200") == NULL) {
    Serial.println("HTTP status is not 200");
    return false;
  }

  while (true) {
    if (!readHttpLine(client, line, sizeof(line))) {
      Serial.println("Cannot read HTTP header");
      return false;
    }
    if (strlen(line) == 0) return true;
  }
}

static bool skipBytes(WiFiClient &client, uint32_t count) {
  uint8_t temp[64];
  while (count > 0) {
    uint32_t take = count > sizeof(temp) ? sizeof(temp) : count;
    if (!readExact(client, temp, take)) return false;
    count -= take;
  }
  return true;
}

static bool parseWavHeader(WiFiClient &client, WavInfo &info) {
  uint8_t header[12];
  if (!readExact(client, header, sizeof(header))) {
    Serial.println("Cannot read RIFF header");
    return false;
  }

  if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
    Serial.println("Not WAV RIFF file");
    return false;
  }

  bool foundFmt = false;

  while (true) {
    uint8_t ch[8];
    if (!readExact(client, ch, sizeof(ch))) {
      Serial.println("Cannot read WAV chunk header");
      return false;
    }

    uint32_t chunkSize = readLE32(ch + 4);

    if (memcmp(ch, "fmt ", 4) == 0) {
      if (chunkSize < 16 || chunkSize > 64) {
        Serial.println("Invalid fmt chunk size");
        return false;
      }

      uint8_t fmt[64];
      if (!readExact(client, fmt, chunkSize)) {
        Serial.println("Cannot read fmt chunk");
        return false;
      }

      uint16_t audioFormat = readLE16(fmt + 0);
      info.channels = readLE16(fmt + 2);
      info.sampleRate = readLE32(fmt + 4);
      info.bitsPerSample = readLE16(fmt + 14);

      if (audioFormat != 1 || (info.bitsPerSample != 8 && info.bitsPerSample != 16) ||
          (info.channels != 1 && info.channels != 2)) {
        Serial.printf("Unsupported WAV fmt format=%u ch=%u rate=%lu bits=%u\n",
                      audioFormat, info.channels,
                      (unsigned long)info.sampleRate, info.bitsPerSample);
        return false;
      }

      foundFmt = true;
    } else if (memcmp(ch, "data", 4) == 0) {
      if (!foundFmt) {
        Serial.println("Found data before fmt");
        return false;
      }
      info.dataSize = chunkSize;
      return true;
    } else {
      if (!skipBytes(client, chunkSize)) {
        Serial.println("Cannot skip WAV chunk");
        return false;
      }
    }

    if (chunkSize & 1) {
      uint8_t pad;
      if (!readExact(client, &pad, 1)) return false;
    }
  }
}

static bool setupSpeaker() {
#ifdef AMP_SD_PIN
  pinMode(AMP_SD_PIN, OUTPUT);
  digitalWrite(AMP_SD_PIN, HIGH); // keep MAX98357A enabled
#endif

  if (speaker_installed) {
    return true;
  }

  i2s_config_t config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SPEAKER_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 12,
    .dma_buf_len = 512,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0,
  };

  i2s_pin_config_t pins = {
    .mck_io_num = I2S_PIN_NO_CHANGE,
    .bck_io_num = SPK_BCLK,
    .ws_io_num = SPK_LRC,
    .data_out_num = SPK_DIN,
    .data_in_num = I2S_PIN_NO_CHANGE,
  };

  esp_err_t err = i2s_driver_install(I2S_SPK_PORT, &config, 0, NULL);
  Serial.printf("Spk i2s_driver_install: %s\n", esp_err_to_name(err));
  if (err == ESP_ERR_INVALID_STATE) {
    // Driver was already installed by a previous setup path.
    speaker_installed = true;
  } else if (err != ESP_OK) {
    speaker_installed = false;
    return false;
  } else {
    speaker_installed = true;
  }

  err = i2s_set_pin(I2S_SPK_PORT, &pins);
  Serial.printf("Spk i2s_set_pin: %s\n", esp_err_to_name(err));
  if (err != ESP_OK) {
    i2s_driver_uninstall(I2S_SPK_PORT);
    speaker_installed = false;
    return false;
  }

  i2s_zero_dma_buffer(I2S_SPK_PORT);
  return true;
}

static bool installMic() {
  if (mic_installed) return true;

  i2s_config_t config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = MIC_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0,
  };

  i2s_pin_config_t pins = {
    .mck_io_num = I2S_PIN_NO_CHANGE,
    .bck_io_num = MIC_SCK,
    .ws_io_num = MIC_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = MIC_SD,
  };

  esp_err_t err = i2s_driver_install(I2S_MIC_PORT, &config, 0, NULL);
  Serial.printf("Mic i2s_driver_install: %s\n", esp_err_to_name(err));
  if (err != ESP_OK) return false;

  err = i2s_set_pin(I2S_MIC_PORT, &pins);
  Serial.printf("Mic i2s_set_pin: %s\n", esp_err_to_name(err));
  if (err != ESP_OK) {
    i2s_driver_uninstall(I2S_MIC_PORT);
    return false;
  }

  i2s_zero_dma_buffer(I2S_MIC_PORT);
  mic_installed = true;
  return true;
}

static void uninstallMic() {
  if (!mic_installed) {
    micPinsHighZ();
    return;
  }

  recording_enabled = false;
  i2s_zero_dma_buffer(I2S_MIC_PORT);
  i2s_driver_uninstall(I2S_MIC_PORT);
  mic_installed = false;
  micPinsHighZ();
  Serial.println("Mic uninstalled and pins high-Z");
}


static bool audioRingInit() {
  if (audioRing == nullptr) {
    audioRing = (uint8_t *)heap_caps_malloc(AUDIO_RING_BYTES, MALLOC_CAP_8BIT);
    if (audioRing == nullptr) {
      Serial.printf("Audio ring malloc failed: %u bytes, free heap=%u\n",
                    (unsigned)AUDIO_RING_BYTES,
                    (unsigned)ESP.getFreeHeap());
      return false;
    }
    Serial.printf("Audio ring allocated: %u bytes, free heap=%u\n",
                  (unsigned)AUDIO_RING_BYTES,
                  (unsigned)ESP.getFreeHeap());
  }

  if (audioRingMutex == nullptr) {
    audioRingMutex = xSemaphoreCreateMutex();
    if (audioRingMutex == nullptr) {
      Serial.println("Audio ring mutex create failed");
      return false;
    }
  }

  return true;
}

static size_t audioRingFill() {
  size_t fill = 0;
  if (!audioRingInit()) return fill;
  if (xSemaphoreTake(audioRingMutex, portMAX_DELAY) == pdTRUE) {
    fill = audioRingFillBytes;
    xSemaphoreGive(audioRingMutex);
  }
  return fill;
}

static void audioRingClear() {
  if (!audioRingInit()) return;
  if (xSemaphoreTake(audioRingMutex, portMAX_DELAY) == pdTRUE) {
    audioRingHead = 0;
    audioRingTail = 0;
    audioRingFillBytes = 0;
    xSemaphoreGive(audioRingMutex);
  }
}

static size_t audioRingWriteSome(const uint8_t *data, size_t len) {
  size_t written = 0;
  if (!audioRingInit()) return 0;

  if (xSemaphoreTake(audioRingMutex, portMAX_DELAY) != pdTRUE) return 0;

  size_t freeBytes = AUDIO_RING_BYTES - audioRingFillBytes;
  size_t toWrite = len < freeBytes ? len : freeBytes;

  while (written < toWrite) {
    size_t contiguous = AUDIO_RING_BYTES - audioRingHead;
    size_t n = toWrite - written;
    if (n > contiguous) n = contiguous;

    memcpy(audioRing + audioRingHead, data + written, n);
    audioRingHead = (audioRingHead + n) % AUDIO_RING_BYTES;
    audioRingFillBytes += n;
    written += n;
  }

  xSemaphoreGive(audioRingMutex);
  return written;
}

static bool audioRingWriteBlocking(const uint8_t *data, size_t len, uint32_t timeoutMs) {
  size_t total = 0;
  uint32_t start = millis();

  while (total < len) {
    size_t n = audioRingWriteSome(data + total, len - total);
    if (n > 0) {
      total += n;
      continue;
    }

    if (timeoutMs > 0 && millis() - start > timeoutMs) {
      return false;
    }

    delay(1);
  }

  return true;
}

static size_t audioRingReadSome(uint8_t *data, size_t maxLen) {
  size_t readBytes = 0;
  if (!audioRingInit()) return 0;

  if (xSemaphoreTake(audioRingMutex, portMAX_DELAY) != pdTRUE) return 0;

  size_t toRead = maxLen < audioRingFillBytes ? maxLen : audioRingFillBytes;

  while (readBytes < toRead) {
    size_t contiguous = AUDIO_RING_BYTES - audioRingTail;
    size_t n = toRead - readBytes;
    if (n > contiguous) n = contiguous;

    memcpy(data + readBytes, audioRing + audioRingTail, n);
    audioRingTail = (audioRingTail + n) % AUDIO_RING_BYTES;
    audioRingFillBytes -= n;
    readBytes += n;
  }

  xSemaphoreGive(audioRingMutex);
  return readBytes;
}

static void audioPlaybackTask(void *param) {
  (void)param;

  for (;;) {
    if (!audioPlaybackActive) {
      delay(2);
      continue;
    }

    size_t n = audioRingReadSome(audioTaskBuffer, sizeof(audioTaskBuffer));

    if (n == 0) {
      if (audioPlaybackStopping) {
        audioPlaybackActive = false;
        audioPlaybackFinished = true;
        continue;
      }

      // Network reader has not produced data in time. Write a short silence block
      // instead of letting I2S starve hard/pop. Count it for debugging.
      memset(audioTaskBuffer, 0, sizeof(audioTaskBuffer));
      n = sizeof(audioTaskBuffer);
      audioUnderruns++;
    }

    // I2S mono 16-bit requires a multiple of 2 bytes.
    n = (n / 2) * 2;
    if (n == 0) {
      delay(1);
      continue;
    }

    int16_t *samples = (int16_t *)audioTaskBuffer;
    size_t sampleCount = n / sizeof(int16_t);
    uint16_t gainPermille = audioPlaybackGainPermille;
    for (size_t i = 0; i < sampleCount; i++) {
      samples[i] = amplifySample(samples[i], gainPermille);
    }

    size_t bytesWritten = 0;
    esp_err_t err = i2s_write(I2S_SPK_PORT,
                              audioTaskBuffer,
                              n,
                              &bytesWritten,
                              pdMS_TO_TICKS(1000));
    if (err != ESP_OK || bytesWritten == 0) {
      Serial.printf("audio task i2s_write failed: %s written=%u\n",
                    esp_err_to_name(err),
                    (unsigned)bytesWritten);
      audioPlaybackActive = false;
      audioPlaybackFinished = true;
    }

    delay(0);
  }
}

static bool ensureAudioPlaybackTask() {
  if (audioTaskHandle != nullptr) return true;

  BaseType_t ok = xTaskCreatePinnedToCore(audioPlaybackTask,
                                          "audio_play",
                                          AUDIO_TASK_STACK,
                                          nullptr,
                                          5,
                                          &audioTaskHandle,
                                          1);
  if (ok != pdPASS) {
    Serial.println("Cannot create audio playback task");
    audioTaskHandle = nullptr;
    return false;
  }

  return true;
}

static void startBufferedPlayback() {
  if (!audioPlaybackActive) {
    audioPlaybackFrameIndex = 0;
    audioPlaybackGainPermille = currentVolumeGainPermille;
    audioPlaybackFinished = false;
    audioPlaybackStopping = false;
    audioPlaybackActive = true;
  }
}

static void stopBufferedPlaybackAndWait(uint32_t timeoutMs) {
  audioPlaybackStopping = true;
  uint32_t start = millis();
  while (!audioPlaybackFinished && millis() - start < timeoutMs) {
    delay(5);
  }
  audioPlaybackActive = false;
  audioPlaybackStopping = false;
}

static void writeSilenceMs(uint32_t ms) {
  if (!setupSpeaker()) {
    Serial.println("Cannot write silence: speaker init failed");
    return;
  }

  memset(silenceBuffer, 0, sizeof(silenceBuffer));
  uint32_t start = millis();
  while (millis() - start < ms) {
    size_t written = 0;
    esp_err_t err = i2s_write(I2S_SPK_PORT, silenceBuffer, sizeof(silenceBuffer), &written, pdMS_TO_TICKS(200));
    if (err != ESP_OK) {
      Serial.printf("silence i2s_write failed: %s written=%u\n", esp_err_to_name(err), (unsigned)written);
      return;
    }
    delay(0);
  }
}

static int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

static bool percentDecode(const String &input, String &output) {
  output = "";
  output.reserve(input.length());
  for (int i = 0; i < (int)input.length(); i++) {
    char c = input[i];
    if (c == '%') {
      if (i + 2 >= (int)input.length()) return false;
      int hi = hexValue(input[i + 1]);
      int lo = hexValue(input[i + 2]);
      if (hi < 0 || lo < 0) return false;
      output += (char)((hi << 4) | lo);
      i += 2;
    } else if (c == '+') {
      output += ' ';
    } else {
      output += c;
    }
  }
  return true;
}

static bool loadStoredWiFiCredentials(String &ssid, String &pass) {
  Preferences prefs;
  if (!prefs.begin(WIFI_PREF_NAMESPACE, true)) {
    Serial.println("WiFi prefs open failed; using defaults");
    ssid = TEST_WIFI_SSID;
    pass = TEST_WIFI_PASS;
    return false;
  }
  ssid = prefs.getString(WIFI_PREF_SSID_KEY, "");
  pass = prefs.getString(WIFI_PREF_PASS_KEY, "");
  prefs.end();

  if (ssid.length() == 0 && strlen(TEST_WIFI_SSID) > 0) {
    ssid = TEST_WIFI_SSID;
    pass = TEST_WIFI_PASS;
  }
  return true;
}

static bool saveStoredWiFiCredentials(const String &ssid, const String &pass) {
  Preferences prefs;
  if (!prefs.begin(WIFI_PREF_NAMESPACE, false)) {
    Serial.println("WiFi prefs write open failed");
    return false;
  }
  bool ok = prefs.putString(WIFI_PREF_SSID_KEY, ssid) == ssid.length();
  ok = prefs.putString(WIFI_PREF_PASS_KEY, pass) == pass.length() && ok;
  prefs.end();
  return ok;
}

static bool parseWifiCredsV1(const String &line, String &ssid, String &pass) {
  const String prefix = "WIFI_CREDS_V1 ";
  if (!line.startsWith(prefix)) return false;

  String payload = line.substring(prefix.length());
  int sep = payload.indexOf(' ');
  if (sep <= 0) return false;

  String encodedSsid = payload.substring(0, sep);
  String encodedPass = payload.substring(sep + 1);
  if (!percentDecode(encodedSsid, ssid)) {
    return false;
  }
  if (encodedPass == "%00") {
    pass = "";
  } else if (!percentDecode(encodedPass, pass)) {
    return false;
  }
  return ssid.length() > 0 && ssid.length() <= 32 && pass.length() <= 64;
}

static bool parseLegacyWifiCreds(const String &line, String &ssid, String &pass) {
  const String prefix = "WIFI_CREDS ";
  if (!line.startsWith(prefix)) return false;

  String payload = line.substring(prefix.length());
  int sep = payload.indexOf(' ');
  if (sep <= 0) return false;
  ssid = payload.substring(0, sep);
  pass = payload.substring(sep + 1);
  ssid.trim();
  pass.trim();
  return ssid.length() > 0 && ssid.length() <= 32 && pass.length() <= 64;
}

static void handleWifiCredsCommand(const String &line) {
  if (recording_enabled || mic_installed || speaker_playing || audioPlaybackActive) {
    Serial.println("WIFI_CREDS busy; retry after mic/playback stops");
    HmiSerial.print("ERR WIFI_CREDS_BUSY\n");
    return;
  }

  String newSsid;
  String newPass;
  bool parsed = parseWifiCredsV1(line, newSsid, newPass) || parseLegacyWifiCreds(line, newSsid, newPass);
  if (!parsed) {
    Serial.println("Invalid WIFI_CREDS command");
    HmiSerial.print("ERR WIFI_CREDS_BAD\n");
    return;
  }

  String oldSsid;
  String oldPass;
  loadStoredWiFiCredentials(oldSsid, oldPass);
  if (oldSsid == newSsid && oldPass == newPass) {
    Serial.print("WiFi credentials already in sync: ");
    Serial.println(newSsid);
    HmiSerial.print("OK WIFI_CREDS_SAME\n");
    return;
  }

  Serial.print("WiFi credentials updated from S3. New SSID: ");
  Serial.println(newSsid);
  if (!saveStoredWiFiCredentials(newSsid, newPass)) {
    HmiSerial.print("ERR WIFI_CREDS_SAVE_FAILED\n");
    return;
  }

  HmiSerial.print("OK WIFI_CREDS_SAVED_REBOOTING\n");
  HmiSerial.flush();
  Serial.println("Rebooting to apply WiFi credentials...");
  delay(500);
  ESP.restart();
}

static void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  String ssid;
  String pass;
  loadStoredWiFiCredentials(ssid, pass);
  if (ssid.length() == 0) {
    Serial.println("No WiFi credentials available");
    return;
  }

  Serial.print("Connecting WiFi: ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false); // Test build: do not write WiFi config to flash.
  WiFi.disconnect(false, false);
  delay(250);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.begin(ssid.c_str(), pass.c_str());

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("Free heap after WiFi: ");
    Serial.println(ESP.getFreeHeap());
  } else {
    Serial.println("WiFi connect failed");
  }
}


static bool saveWavPcmToLittleFS(WiFiClient &client, uint32_t dataSize, const char *path) {
  if (!LittleFS.begin(false)) {
    Serial.println("LittleFS mount failed");
    return false;
  }

  if (LittleFS.exists(path)) {
    LittleFS.remove(path);
  }

  File f = LittleFS.open(path, "w");
  if (!f) {
    Serial.println("Cannot open audio cache file for write");
    return false;
  }

  uint8_t temp[2048];
  uint32_t remaining = dataSize;
  uint32_t writtenTotal = 0;
  uint32_t lastLog = millis();

  Serial.println("Downloading WAV PCM data to LittleFS cache...");

  while (remaining > 0) {
    size_t want = remaining > sizeof(temp) ? sizeof(temp) : remaining;
    int n = client.readBytes(temp, want);

    if (n <= 0) {
      if (!client.connected()) break;
      delay(1);
      continue;
    }

    size_t w = f.write(temp, (size_t)n);
    if (w != (size_t)n) {
      Serial.printf("LittleFS write failed: wanted=%u wrote=%u\n", (unsigned)n, (unsigned)w);
      f.close();
      return false;
    }

    remaining -= (uint32_t)n;
    writtenTotal += (uint32_t)n;

    if (millis() - lastLog > 1000) {
      lastLog = millis();
      Serial.printf("downloading... remaining=%lu written=%lu heap=%u\n",
                    (unsigned long)remaining,
                    (unsigned long)writtenTotal,
                    (unsigned)ESP.getFreeHeap());
    }

    delay(0);
  }

  f.close();

  if (remaining != 0) {
    Serial.printf("Download incomplete: written=%lu remaining=%lu\n",
                  (unsigned long)writtenTotal,
                  (unsigned long)remaining);
    return false;
  }

  Serial.printf("Download done: %lu bytes cached\n", (unsigned long)writtenTotal);
  return true;
}

static bool playCachedPcmFromLittleFS(const char *path, const WavInfo &wav) {
  File f = LittleFS.open(path, "r");
  if (!f) {
    Serial.println("Cannot open audio cache file for read");
    return false;
  }

  if (!setupSpeaker()) {
    Serial.println("Speaker init failed");
    f.close();
    return false;
  }

#ifdef AMP_SD_PIN
  digitalWrite(AMP_SD_PIN, HIGH);
  delay(50);
#endif

  i2s_zero_dma_buffer(I2S_SPK_PORT);
  writeSilenceMs(80);

  const size_t bytesPerInputFrame = wav.channels * (wav.bitsPerSample / 8);
  uint32_t remaining = wav.dataSize;
  uint32_t frameIndex = 0;
  uint32_t totalWritten = 0;
  uint32_t lastLog = millis();

  Serial.println("Start LittleFS local playback...");
  updateVolumeStep(true);

  while (remaining > 0) {
    updateVolumeStep(false);
    size_t want = PCM_FRAMES_PER_CHUNK * bytesPerInputFrame;
    if (remaining < want) want = remaining;
    want = (want / bytesPerInputFrame) * bytesPerInputFrame;
    if (want == 0) break;

    int bytesRead = f.read((uint8_t *)inputBuffer, want);
    if (bytesRead <= 0) {
      Serial.println("Audio cache read ended early");
      break;
    }

    remaining -= (uint32_t)bytesRead;
    int frames = bytesRead / (int)bytesPerInputFrame;
    int outIndex = 0;

    const uint8_t *inputBytes = (const uint8_t *)inputBuffer;
    for (int i = 0; i < frames; i++) {
      outputBuffer[outIndex++] = amplifySample(wavFrameToPcm16(inputBytes + (i * bytesPerInputFrame), wav),
                                               currentVolumeGainPermille);
    }

    size_t bytesWritten = 0;
    esp_err_t err = i2s_write(I2S_SPK_PORT,
                              outputBuffer,
                              outIndex * sizeof(int16_t),
                              &bytesWritten,
                              pdMS_TO_TICKS(1000));

    if (err != ESP_OK || bytesWritten == 0) {
      Serial.printf("i2s_write failed: %s written=%u remaining=%lu\n",
                    esp_err_to_name(err),
                    (unsigned)bytesWritten,
                    (unsigned long)remaining);
      f.close();
      return false;
    }

    totalWritten += (uint32_t)bytesWritten;

    if (millis() - lastLog > 2000) {
      lastLog = millis();
      Serial.printf("playing local... remaining=%lu written=%lu heap=%u\n",
                    (unsigned long)remaining,
                    (unsigned long)totalWritten,
                    (unsigned)ESP.getFreeHeap());
    }

    delay(0);
  }

  f.close();

  writeSilenceMs(300);
  i2s_zero_dma_buffer(I2S_SPK_PORT);

#ifdef AMP_SD_PIN
  digitalWrite(AMP_SD_PIN, HIGH);
#endif

  Serial.printf("LittleFS playback done, totalWritten=%lu remaining=%lu\n",
                (unsigned long)totalWritten,
                (unsigned long)remaining);

  return remaining == 0;
}

static bool playWavFromUrl(const String &audioUrl) {
  if (speaker_playing) {
    Serial.println("Playback already active");
    return false;
  }
  speaker_playing = true;
  bool ok = false;
  String url = esp32AudioUrl(audioUrl);
  Serial.print("Resolved playback URL: ");
  Serial.println(url);
  String host, path;
  uint16_t port = 80;

  if (!parseHttpUrl(url, host, port, path)) {
    speaker_playing = false;
    return false;
  }

  // Stop mic while playing to avoid I2S/power contention.
  if (recording_enabled || mic_installed) {
    Serial.println("Stopping mic before playback...");
    uninstallMic();
  }

  connectWiFi();
  if (WiFi.status() != WL_CONNECTED) {
    speaker_playing = false;
    return false;
  }

  if (!setupSpeaker() || !ensureAudioPlaybackTask()) {
    Serial.println("Speaker/playback task init failed");
    speaker_playing = false;
    return false;
  }

  Serial.print("Connecting audio server: ");
  Serial.print(host);
  Serial.print(":");
  Serial.println(port);

  WiFiClient client;
  client.setTimeout(8000);

  if (!client.connect(host.c_str(), port)) {
    Serial.println("TCP connect failed");
    speaker_playing = false;
    return false;
  }

  client.print("GET ");
  client.print(path);
  client.println(" HTTP/1.1");
  client.print("Host: ");
  client.print(host);
  client.print(":");
  client.println(port);
  client.println("Connection: close");
  client.println();

  if (!skipHttpHeaders(client)) {
    client.stop();
    speaker_playing = false;
    return false;
  }

  WavInfo wav;
  if (!parseWavHeader(client, wav)) {
    client.stop();
    speaker_playing = false;
    return false;
  }

  Serial.printf("Play WAV: rate=%lu ch=%u bits=%u size=%lu\n",
                (unsigned long)wav.sampleRate,
                wav.channels,
                wav.bitsPerSample,
                (unsigned long)wav.dataSize);

  if (wav.sampleRate != SPEAKER_SAMPLE_RATE) {
    Serial.printf("Unsupported sample rate. Expected %u Hz.\n", SPEAKER_SAMPLE_RATE);
    client.stop();
    speaker_playing = false;
    return false;
  }

  const size_t bytesPerInputFrame = wav.channels * (wav.bitsPerSample / 8);
  uint32_t remaining = wav.dataSize;
  uint32_t totalIn = 0;
  uint32_t totalOut = 0;
  uint32_t lastLog = millis();
  bool playbackStarted = false;

  audioRingClear();
  audioUnderruns = 0;
  i2s_zero_dma_buffer(I2S_SPK_PORT);
  writeSilenceMs(60);
  updateVolumeStep(true);
  audioPlaybackGainPermille = currentVolumeGainPermille;

  Serial.printf("Streaming playback prebuffer=%u ring=%u lowWater=%u\n",
                (unsigned)AUDIO_PREBUFFER_BYTES,
                (unsigned)AUDIO_RING_BYTES,
                (unsigned)AUDIO_LOW_WATER_BYTES);

  while (remaining > 0) {
    updateVolumeStep(false);
    audioPlaybackGainPermille = currentVolumeGainPermille;

    size_t want = PCM_FRAMES_PER_CHUNK * bytesPerInputFrame;
    if (remaining < want) want = remaining;
    want = (want / bytesPerInputFrame) * bytesPerInputFrame;
    if (want == 0) break;

    if (!readExact(client, (uint8_t *)inputBuffer, want, 8000)) {
      Serial.println("HTTP stream read failed");
      break;
    }

    size_t bytesRead = want;
    remaining -= (uint32_t)bytesRead;
    totalIn += (uint32_t)bytesRead;

    int frames = bytesRead / (int)bytesPerInputFrame;
    int outIndex = 0;

    const uint8_t *inputBytes = (const uint8_t *)inputBuffer;
    for (int i = 0; i < frames; i++) {
      outputBuffer[outIndex++] = wavFrameToPcm16(inputBytes + (i * bytesPerInputFrame), wav);
    }

    size_t outBytes = outIndex * sizeof(int16_t);
    if (!audioRingWriteBlocking((const uint8_t *)outputBuffer, outBytes, 10000)) {
      Serial.println("Audio ring write timeout");
      break;
    }
    totalOut += (uint32_t)outBytes;

    size_t fill = audioRingFill();
    if (!playbackStarted && (fill >= AUDIO_PREBUFFER_BYTES || remaining == 0)) {
      Serial.printf("Start streaming playback, buffered=%u bytes\n", (unsigned)fill);
      startBufferedPlayback();
      playbackStarted = true;
    }

    if (millis() - lastLog > 1000) {
      lastLog = millis();
      Serial.printf("streaming... remaining=%lu ring=%u in=%lu out=%lu underruns=%lu heap=%u\n",
                    (unsigned long)remaining,
                    (unsigned)fill,
                    (unsigned long)totalIn,
                    (unsigned long)totalOut,
                    (unsigned long)audioUnderruns,
                    (unsigned)ESP.getFreeHeap());
    }

    processCommandInputs(false);
    delay(0);
  }

  client.stop();

  if (!playbackStarted && audioRingFill() > 0) {
    startBufferedPlayback();
    playbackStarted = true;
  }

  if (remaining == 0 && playbackStarted) {
    stopBufferedPlaybackAndWait(20000);
    ok = audioRingFill() == 0;
  } else {
    audioPlaybackActive = false;
    audioPlaybackStopping = false;
    audioPlaybackFinished = true;
  }

  writeSilenceMs(120);
  i2s_zero_dma_buffer(I2S_SPK_PORT);

  if (!ok) {
    Serial.printf("Streaming playback failed: remaining=%lu ring=%u underruns=%lu\n",
                  (unsigned long)remaining,
                  (unsigned)audioRingFill(),
                  (unsigned long)audioUnderruns);
    speaker_playing = false;
    return false;
  }

  Serial.printf("Streaming playback done: in=%lu out=%lu underruns=%lu\n",
                (unsigned long)totalIn,
                (unsigned long)totalOut,
                (unsigned long)audioUnderruns);

  HmiSerial.print("OK PLAY_DONE\n");
  speaker_playing = false;
  return true;
}

static void sendAudioFrame(const uint8_t *data, uint16_t len) {
  static const uint8_t header[] = {'A', 'U', 'D', '0'};
  uint8_t lenBytes[2] = {
    (uint8_t)(len & 0xff),
    (uint8_t)(len >> 8),
  };
  HmiSerial.write(header, sizeof(header));
  HmiSerial.write(lenBytes, sizeof(lenBytes));
  HmiSerial.write(data, len);
}

static void startRecording() {
  Serial.println("REC_START requested");
  if (speaker_playing) {
    Serial.println("Reject REC_START: speaker is playing");
    HmiSerial.print("ERR SPEAKER_BUSY\n");
    return;
  }

  if (!installMic()) {
    Serial.println("Mic init failed");
    HmiSerial.print("ERR MIC_INIT_FAILED\n");
    return;
  }

  i2s_zero_dma_buffer(I2S_MIC_PORT);
  recording_enabled = true;
  HmiSerial.print("OK RECORDING\n");
  Serial.println("OK RECORDING");
}

static void stopRecording() {
  if (!recording_enabled && !mic_installed) {
    HmiSerial.print("OK STOPPED\n");
    return;
  }

  recording_enabled = false;
  uninstallMic();
  HmiSerial.print("OK STOPPED\n");
  Serial.println("OK STOPPED");
}

static void streamMicToHmi() {
  if (!recording_enabled || !mic_installed) return;

  size_t bytesRead = 0;
  esp_err_t err = i2s_read(I2S_MIC_PORT,
                           micRawBuffer,
                           sizeof(micRawBuffer),
                           &bytesRead,
                           pdMS_TO_TICKS(10));

  if (err != ESP_OK || bytesRead == 0) {
    return;
  }

  size_t count = bytesRead / sizeof(micRawBuffer[0]);
  for (size_t i = 0; i < count; i++) {
    micPcmBuffer[i] = micSampleToPcm16(micRawBuffer[i]);
  }

  sendAudioFrame((const uint8_t *)micPcmBuffer, (uint16_t)(count * sizeof(micPcmBuffer[0])));
}

static void printMicLevelOnce() {
  if (!installMic()) return;

  size_t bytesRead = 0;
  esp_err_t err = i2s_read(I2S_MIC_PORT,
                           micRawBuffer,
                           sizeof(micRawBuffer),
                           &bytesRead,
                           pdMS_TO_TICKS(50));
  if (err != ESP_OK || bytesRead == 0) {
    Serial.printf("MIC_READ_ERR %s bytes=%u\n", esp_err_to_name(err), (unsigned)bytesRead);
    return;
  }

  size_t count = bytesRead / sizeof(micRawBuffer[0]);
  int64_t absSum = 0;
  int32_t peak = 0;
  for (size_t i = 0; i < count; i++) {
    int16_t s = micSampleToPcm16(micRawBuffer[i]);
    int32_t a = abs((int)s);
    absSum += a;
    if (a > peak) peak = a;
  }

  Serial.printf("MIC avg=%ld peak=%ld samples=%u\n",
                (long)(absSum / (int64_t)count),
                (long)peak,
                (unsigned)count);
}

static bool playUrl(const String &url) {
  Serial.print("Play URL requested: ");
  Serial.println(url);

  HmiSerial.print("OK PLAYING_URL\n");
  bool ok = playWavFromUrl(url);
  speaker_playing = false;
  if (!ok) {
    HmiSerial.print("ERR PLAY_URL_FAILED\n");
    Serial.println("PLAY_URL failed");
  }
  return ok;
}

static bool isKnownCommandPrefix(const String &line) {
  return line == "PING" ||
         line == "REC_START" ||
         line == "REC_STOP" ||
         line == "MIC_TEST" ||
         line.startsWith("WIFI_CREDS_V1 ") ||
         line.startsWith("WIFI_CREDS ") ||
         line.indexOf("PLAY_URL ") >= 0 ||
         findAudioUrlStart(line) >= 0 ||
         line.indexOf("audio_url") >= 0;
}

static void handleCommand(String line) {
  line.trim();
  String audioUrl = extractAudioUrl(line);

  if (line == "PING") {
    Serial.println("OK READY");
    HmiSerial.print("OK READY\n");
  } else if (line == "REC_START") {
    startRecording();
  } else if (line == "REC_STOP") {
    stopRecording();
  } else if (line == "MIC_TEST") {
    printMicLevelOnce();
  } else if (line.startsWith("WIFI_CREDS_V1 ") || line.startsWith("WIFI_CREDS ")) {
    handleWifiCredsCommand(line);
  } else if (audioUrl.length() > 0) {
    playUrl(audioUrl);
  } else if (line.length() > 0) {
    Serial.print("Unknown command: ");
    Serial.println(line);
  }
}

static void writeToneMs(uint32_t ms, uint32_t frequencyHz) {
  if (!setupSpeaker()) {
    Serial.println("Cannot write tone: speaker init failed");
    return;
  }

  updateVolumeStep(true);
  uint32_t totalFrames = (SPEAKER_SAMPLE_RATE * ms) / 1000;
  uint32_t frameIndex = 0;

  while (frameIndex < totalFrames) {
    size_t frames = totalFrames - frameIndex;
    if (frames > PCM_FRAMES_PER_CHUNK) frames = PCM_FRAMES_PER_CHUNK;

    for (size_t i = 0; i < frames; i++) {
      outputBuffer[i] = squareToneSample(frameIndex + i, frequencyHz, currentVolumeGainPermille);
    }

    size_t written = 0;
    esp_err_t err = i2s_write(I2S_SPK_PORT,
                              outputBuffer,
                              frames * sizeof(int16_t),
                              &written,
                              pdMS_TO_TICKS(500));
    if (err != ESP_OK || written == 0) {
      Serial.printf("tone i2s_write failed: %s written=%u\n",
                    esp_err_to_name(err),
                    (unsigned)written);
      return;
    }

    frameIndex += frames;
    delay(0);
  }
}

static void processCommandInputs(bool allowPlaybackCommand) {
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (!allowPlaybackCommand && extractAudioUrl(line).length() > 0) {
      Serial.println("Reject audio URL: playback is busy");
    } else {
      handleCommand(line);
    }
  }

  while (HmiSerial.available()) {
    char c = (char)HmiSerial.read();
    if (c == '\n') {
      hmi_line.trim();
      if (hmi_line.length() > 0) {
        if (isKnownCommandPrefix(hmi_line)) {
          Serial.print("[UART CMD] ");
          Serial.println(hmi_line);
          if (!allowPlaybackCommand && extractAudioUrl(hmi_line).length() > 0) {
            HmiSerial.print("ERR SPEAKER_BUSY\n");
          } else {
            handleCommand(hmi_line);
          }
        } else {
          Serial.print("[UART IGNORE] ");
          Serial.println(hmi_line);
        }
      }
      hmi_line = "";
    } else if (c != '\r') {
      if (hmi_line.length() < 1024) {
        hmi_line += c;
      } else {
        hmi_line = "";
      }
    }
  }
}

void setup() {
  Serial.begin(USB_DEBUG_BAUD);
  delay(700);
  Serial.setTimeout(20);

  Serial.println();
  Serial.println("Audio board mono buffered build: speaker + lazy mic");
  Serial.print("Reset reason: ");
  Serial.println((int)esp_reset_reason());
  Serial.print("Free heap at boot: ");
  Serial.println(ESP.getFreeHeap());

#ifdef AMP_SD_PIN
  pinMode(AMP_SD_PIN, OUTPUT);
  digitalWrite(AMP_SD_PIN, HIGH);
#endif

  HmiSerial.begin(HMI_BAUD, SERIAL_8N1, HMI_RX, HMI_TX);
  HmiSerial.setTimeout(20);

  pinMode(VOLUME_ADC_PIN, INPUT);
  // Legacy ADC1 setup. Avoid analogRead()/analogSetPinAttenuation() to prevent driver_ng conflict.
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(ADC1_CHANNEL_5, ADC_ATTEN_DB_11); // GPIO33
  updateVolumeStep(true);

  micPinsHighZ();
  Serial.println("Mic lazy mode: pins high-Z until REC_START");

  Serial.println("LittleFS skipped in streaming test build; flash is untouched");

  connectWiFi();
  if (!setupSpeaker()) {
    Serial.println("Speaker init failed at boot");
  }
  ensureAudioPlaybackTask();

  Serial.println("Audio board ready");
  Serial.printf("USB debug baud=%lu, HMI UART RX=%d TX=%d baud=%lu\n",
                (unsigned long)USB_DEBUG_BAUD,
                HMI_RX,
                HMI_TX,
                (unsigned long)HMI_BAUD);
  Serial.println("Commands: PING, MIC_TEST, REC_START, REC_STOP, PLAY_URL <url>");
  HmiSerial.print("OK READY\n");
}

void loop() {
  updateVolumeStep(false);

  processCommandInputs(true);

  streamMicToHmi();

  if (AUTO_LOOP_TEST_AUDIO && !speaker_playing && !recording_enabled) {
    bool ok = playUrl(TEST_AUDIO_URL);
    delay(ok ? 500 : 3000);
  }

  delay(1);
}
