#include <Arduino.h>
#include <WiFi.h>
#include <driver/i2s.h>
#include <Preferences.h>

// ===================== USER CONFIG =====================
static const char *SERVER_BASE_URL = "http://54.206.118.226:8000";

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

// If your MAX98357A SD/EN pin is connected to ESP32, uncomment and set the pin.
// #define AMP_SD_PIN 33

#define SPEAKER_SAMPLE_RATE 16000
#define MIC_SAMPLE_RATE     16000

// Speaker volume. Increase later if stable: 2.0f, 2.5f, 3.0f.
#define VOLUME_GAIN 1.5f

// Smaller chunks reduce instantaneous current.
#define PCM_FRAMES_PER_CHUNK 128
#define MIC_FRAMES_PER_CHUNK 160

// Keep speaker on I2S0 because your speaker-only test is stable with this.
static const i2s_port_t I2S_SPK_PORT = I2S_NUM_0;
static const i2s_port_t I2S_MIC_PORT = I2S_NUM_1;

static String hmi_line;
static bool mic_installed = false;
static bool recording_enabled = false;
static bool speaker_installed = false;

// Global buffers to avoid stack pressure.
static int16_t inputBuffer[PCM_FRAMES_PER_CHUNK * 2];
static int16_t outputBuffer[PCM_FRAMES_PER_CHUNK * 2];
static int16_t silenceBuffer[PCM_FRAMES_PER_CHUNK * 2];
static int32_t micRawBuffer[MIC_FRAMES_PER_CHUNK];
static int16_t micPcmBuffer[MIC_FRAMES_PER_CHUNK];

struct WavInfo {
  uint32_t sampleRate = 0;
  uint16_t bitsPerSample = 0;
  uint16_t channels = 0;
  uint32_t dataSize = 0;
};

static inline uint16_t readLE16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t readLE32(const uint8_t *p) {
  return (uint32_t)p[0] |
         ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static int16_t amplifySampleRamp(int16_t input, uint32_t frameIndex) {
  const uint32_t rampFrames = SPEAKER_SAMPLE_RATE / 4; // 0.25s
  float ramp = 1.0f;
  if (frameIndex < rampFrames) {
    ramp = (float)frameIndex / (float)rampFrames;
  }

  int32_t value = (int32_t)((float)input * VOLUME_GAIN * ramp);
  if (value > 32767) value = 32767;
  if (value < -32768) value = -32768;
  return (int16_t)value;
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

    if (millis() - lastData > 5000) {
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

      if (audioFormat != 1 || info.bitsPerSample != 16 ||
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
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
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

static void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Preferences prefs;
  prefs.begin("wifi", true);
  String ssid = prefs.getString("ssid", "spad0604");
  String pass = prefs.getString("pass", "06042004");
  prefs.end();

  Serial.print("Connecting WiFi: ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
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

static bool playWavFromUrl(const String &audioUrl) {
  String url = absoluteAudioUrl(audioUrl);
  String host, path;
  uint16_t port = 80;

  if (!parseHttpUrl(url, host, port, path)) return false;

  // Stop mic while playing to avoid I2S/power contention.
  if (recording_enabled || mic_installed) {
    Serial.println("Stopping mic before playback...");
    uninstallMic();
  }

  connectWiFi();
  if (WiFi.status() != WL_CONNECTED) return false;

  Serial.print("Connecting audio server: ");
  Serial.print(host);
  Serial.print(":");
  Serial.println(port);

  WiFiClient client;
  client.setTimeout(5000);

  if (!client.connect(host.c_str(), port)) {
    Serial.println("TCP connect failed");
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
    return false;
  }

  WavInfo wav;
  if (!parseWavHeader(client, wav)) {
    client.stop();
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
    return false;
  }

  // Keep speaker I2S installed across plays. Reinstalling/uninstalling I2S0 every
  // playback can leave MAX98357A with stale/missing clocks on the next play.
  if (!setupSpeaker()) {
    Serial.println("Speaker init failed");
    client.stop();
    return false;
  }

#ifdef AMP_SD_PIN
  digitalWrite(AMP_SD_PIN, HIGH);
  delay(50);
#endif

  i2s_zero_dma_buffer(I2S_SPK_PORT);
  writeSilenceMs(80);

  Serial.println("Start playing audio data...");

  const size_t bytesPerInputFrame = wav.channels * sizeof(int16_t);
  uint32_t remaining = wav.dataSize;
  uint32_t frameIndex = 0;
  uint32_t totalWritten = 0;
  uint32_t lastLog = millis();

  while (remaining > 0) {
    size_t want = sizeof(inputBuffer);
    if (remaining < want) want = remaining;
    want = (want / bytesPerInputFrame) * bytesPerInputFrame;
    if (want == 0) break;

    int bytesRead = client.readBytes((uint8_t *)inputBuffer, want);
    if (bytesRead <= 0) {
      if (!client.connected()) break;
      delay(1);
      continue;
    }

    remaining -= (uint32_t)bytesRead;
    int frames = bytesRead / (int)bytesPerInputFrame;
    int outIndex = 0;

    if (wav.channels == 1) {
      for (int i = 0; i < frames; i++) {
        int16_t s = amplifySampleRamp(inputBuffer[i], frameIndex++);
        outputBuffer[outIndex++] = s;
        outputBuffer[outIndex++] = s;
      }
    } else {
      for (int i = 0; i < frames; i++) {
        int16_t l = amplifySampleRamp(inputBuffer[i * 2], frameIndex);
        int16_t r = amplifySampleRamp(inputBuffer[i * 2 + 1], frameIndex);
        frameIndex++;
        outputBuffer[outIndex++] = l;
        outputBuffer[outIndex++] = r;
      }
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
      client.stop();
      return false;
    }

    totalWritten += (uint32_t)bytesWritten;

    if (millis() - lastLog > 500) {
      lastLog = millis();
      Serial.printf("playing... remaining=%lu written=%lu heap=%u\n",
                    (unsigned long)remaining,
                    (unsigned long)totalWritten,
                    (unsigned)ESP.getFreeHeap());
    }

    delay(0);
  }

  writeSilenceMs(300);
  i2s_zero_dma_buffer(I2S_SPK_PORT);

#ifdef AMP_SD_PIN
  // Keep amp enabled. Pulling SD/EN low after playback can make the next playback silent
  // if the pin or module enable state is not restored cleanly.
  digitalWrite(AMP_SD_PIN, HIGH);
#endif

  client.stop();

  Serial.printf("URL playback done, totalWritten=%lu remaining=%lu\n",
                (unsigned long)totalWritten,
                (unsigned long)remaining);
  HmiSerial.print("OK PLAY_DONE\n");
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

static void playUrl(const String &url) {
  Serial.print("Play URL requested: ");
  Serial.println(url);

  HmiSerial.print("OK PLAYING_URL\n");
  bool ok = playWavFromUrl(url);
  if (!ok) {
    HmiSerial.print("ERR PLAY_URL_FAILED\n");
    Serial.println("PLAY_URL failed");
  }
}

static bool isKnownCommandPrefix(const String &line) {
  return line == "PING" ||
         line == "REC_START" ||
         line == "REC_STOP" ||
         line == "MIC_TEST" ||
         line.startsWith("WIFI_CREDS ") ||
         line.startsWith("PLAY_URL ");
}

static void handleCommand(String line) {
  line.trim();
  if (line == "PING") {
    Serial.println("OK READY");
    HmiSerial.print("OK READY\n");
  } else if (line == "REC_START") {
    startRecording();
  } else if (line == "REC_STOP") {
    stopRecording();
  } else if (line == "MIC_TEST") {
    printMicLevelOnce();
  } else if (line.startsWith("PLAY_URL ")) {
    playUrl(line.substring(9));
  } else if (line.startsWith("WIFI_CREDS ")) {
    String payload = line.substring(11);
    int spaceIdx = payload.indexOf(' ');
    if (spaceIdx > 0) {
      String ssid = payload.substring(0, spaceIdx);
      String pass = payload.substring(spaceIdx + 1);
      Preferences prefs;
      prefs.begin("wifi", false);
      prefs.putString("ssid", ssid);
      prefs.putString("pass", pass);
      prefs.end();
      Serial.println("Saved new WiFi credentials. Reconnecting...");
      WiFi.disconnect();
      connectWiFi();
    }
  } else if (line.length() > 0) {
    Serial.print("Unknown command: ");
    Serial.println(line);
  }
}

void setup() {
  Serial.begin(USB_DEBUG_BAUD);
  delay(700);
  Serial.setTimeout(20);

  Serial.println();
  Serial.println("Audio board stable build: speaker + lazy mic");
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

  micPinsHighZ();
  Serial.println("Mic lazy mode: pins high-Z until REC_START");

  connectWiFi();
  if (!setupSpeaker()) {
    Serial.println("Speaker init failed at boot");
  }

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
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    handleCommand(line);
  }

  while (HmiSerial.available()) {
    char c = (char)HmiSerial.read();
    if (c == '\n') {
      hmi_line.trim();
      if (hmi_line.length() > 0) {
        if (isKnownCommandPrefix(hmi_line)) {
          Serial.print("[UART CMD] ");
          Serial.println(hmi_line);
          handleCommand(hmi_line);
        } else {
          Serial.print("[UART IGNORE] ");
          Serial.println(hmi_line);
        }
      }
      hmi_line = "";
    } else if (c != '\r') {
      if (hmi_line.length() < 240) {
        hmi_line += c;
      } else {
        hmi_line = "";
      }
    }
  }

  streamMicToHmi();
  delay(1);
}
