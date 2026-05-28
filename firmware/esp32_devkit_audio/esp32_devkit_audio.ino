#include <Arduino.h>
#include <driver/i2s.h>

static HardwareSerial HmiSerial(2);

static const int HMI_RX = 16;
static const int HMI_TX = 17;
static const uint32_t HMI_BAUD = 921600;

// INMP441 microphone
static const int MIC_WS = 15;
static const int MIC_SCK = 14;
static const int MIC_SD = 32;

// MAX98357A speaker
static const int SPK_LRC = 25;
static const int SPK_BCLK = 26;
static const int SPK_DIN = 27;

static const i2s_port_t I2S_MIC_PORT = I2S_NUM_0;
static const i2s_port_t I2S_SPK_PORT = I2S_NUM_1;

static bool mic_test_enabled = false;
static bool monitor_enabled = false;
static bool recording_enabled = false;
static uint32_t last_mic_test_ms = 0;
static String hmi_line;
static uint32_t dropped_uart_bytes = 0;

static bool isPrintableCommandByte(uint8_t b)
{
    return b == '\n' || b == '\r' || b == ' ' || (b >= '0' && b <= '9') ||
           (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') ||
           b == '_' || b == ':' || b == '/' || b == '.' || b == '-';
}

static bool isKnownCommandPrefix(const String &line)
{
    return line == "PING" ||
           line == "REC_START" ||
           line == "REC_STOP" ||
           line == "MIC_TEST" ||
           line == "MIC_TEST_ON" ||
           line == "MIC_TEST_OFF" ||
           line == "MONITOR_ON" ||
           line == "MONITOR_OFF" ||
           line.startsWith("PLAY_URL ");
}

static void setupMic()
{
    i2s_config_t config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = 16000,
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
        .bck_io_num = MIC_SCK,
        .ws_io_num = MIC_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = MIC_SD,
    };

    esp_err_t err = i2s_driver_install(I2S_MIC_PORT, &config, 0, NULL);
    Serial.printf("Mic i2s_driver_install: %s\n", esp_err_to_name(err));
    err = i2s_set_pin(I2S_MIC_PORT, &pins);
    Serial.printf("Mic i2s_set_pin: %s\n", esp_err_to_name(err));
    i2s_zero_dma_buffer(I2S_MIC_PORT);
}

static void setupSpeaker()
{
    i2s_config_t config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = 16000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0,
    };

    i2s_pin_config_t pins = {
        .bck_io_num = SPK_BCLK,
        .ws_io_num = SPK_LRC,
        .data_out_num = SPK_DIN,
        .data_in_num = I2S_PIN_NO_CHANGE,
    };

    i2s_driver_install(I2S_SPK_PORT, &config, 0, NULL);
    i2s_set_pin(I2S_SPK_PORT, &pins);
}

static void printMicLevel()
{
    int32_t samples[256];
    size_t bytes_read = 0;
    esp_err_t err = i2s_read(I2S_MIC_PORT, samples, sizeof(samples), &bytes_read, pdMS_TO_TICKS(50));
    if (err != ESP_OK || bytes_read == 0) {
        Serial.printf("MIC_READ_ERR %s bytes=%u\n", esp_err_to_name(err), (unsigned)bytes_read);
        return;
    }

    size_t count = bytes_read / sizeof(samples[0]);
    int64_t abs_sum = 0;
    int32_t peak = 0;
    int64_t dc_sum = 0;

    for (size_t i = 0; i < count; i++) {
        // INMP441 gives 24-bit signed audio left-aligned in a 32-bit I2S word.
        int32_t s = samples[i] >> 14;
        dc_sum += s;
        int32_t a = abs(s);
        abs_sum += a;
        if (a > peak) {
            peak = a;
        }
    }

    int32_t avg = abs_sum / count;
    int32_t dc = dc_sum / (int64_t)count;
    Serial.printf("MIC avg=%ld peak=%ld dc=%ld samples=%u\n", (long)avg, (long)peak, (long)dc, (unsigned)count);
}

static int16_t micSampleToPcm16(int32_t sample)
{
    // INMP441 gives 24-bit signed audio left-aligned in a 32-bit I2S word.
    int32_t s = sample >> 14;
    s *= 2;

    if (s > 32767) {
        s = 32767;
    } else if (s < -32768) {
        s = -32768;
    }

    return (int16_t)s;
}

static void monitorMicToSpeaker()
{
    int32_t mic_samples[128];
    int16_t speaker_samples[128];
    size_t bytes_read = 0;

    esp_err_t err = i2s_read(I2S_MIC_PORT, mic_samples, sizeof(mic_samples), &bytes_read, pdMS_TO_TICKS(20));
    if (err != ESP_OK || bytes_read == 0) {
        return;
    }

    size_t count = bytes_read / sizeof(mic_samples[0]);
    for (size_t i = 0; i < count; i++) {
        speaker_samples[i] = micSampleToPcm16(mic_samples[i]);
    }

    size_t bytes_written = 0;
    i2s_write(I2S_SPK_PORT, speaker_samples, count * sizeof(speaker_samples[0]), &bytes_written, pdMS_TO_TICKS(20));
}

static void sendAudioFrame(const uint8_t *data, uint16_t len)
{
    static const uint8_t header[] = {'A', 'U', 'D', '0'};
    uint8_t len_bytes[2] = {
        (uint8_t)(len & 0xff),
        (uint8_t)(len >> 8),
    };
    HmiSerial.write(header, sizeof(header));
    HmiSerial.write(len_bytes, sizeof(len_bytes));
    HmiSerial.write(data, len);
}

static void streamMicToHmi()
{
    int32_t mic_samples[256];
    int16_t pcm_samples[256];
    size_t bytes_read = 0;

    esp_err_t err = i2s_read(I2S_MIC_PORT, mic_samples, sizeof(mic_samples), &bytes_read, pdMS_TO_TICKS(10));
    if (err != ESP_OK || bytes_read == 0) {
        return;
    }

    size_t count = bytes_read / sizeof(mic_samples[0]);
    for (size_t i = 0; i < count; i++) {
        pcm_samples[i] = micSampleToPcm16(mic_samples[i]);
    }

    sendAudioFrame((const uint8_t *)pcm_samples, (uint16_t)(count * sizeof(pcm_samples[0])));
}

static void startRecording()
{
    HmiSerial.print("OK RECORDING\n");
    recording_enabled = true;
    mic_test_enabled = false;
    monitor_enabled = false;
    i2s_zero_dma_buffer(I2S_MIC_PORT);
}

static void stopRecording()
{
    recording_enabled = false;
    HmiSerial.print("OK STOPPED\n");
    mic_test_enabled = false;
    monitor_enabled = false;
}

static void playUrl(const String &url)
{
    HmiSerial.print("OK PLAYING\n");

    // TODO: Neu DevKit co WiFi thi download URL va stream WAV ra MAX98357A.
    // Neu khong co WiFi, ESP32-S3 can stream audio bytes qua UART cho board nay.
    Serial.print("Play URL requested: ");
    Serial.println(url);
}

static void handleCommand(String line)
{
    line.trim();
    if (line == "PING") {
        HmiSerial.print("OK READY\n");
    } else if (line == "REC_START") {
        startRecording();
    } else if (line == "REC_STOP") {
        stopRecording();
    } else if (line == "MIC_TEST" || line == "MIC_TEST_ON") {
        recording_enabled = false;
        mic_test_enabled = true;
        monitor_enabled = false;
        Serial.println("OK MIC_TEST_ON");
        HmiSerial.print("OK MIC_TEST_ON\n");
    } else if (line == "MIC_TEST_OFF") {
        mic_test_enabled = false;
        Serial.println("OK MIC_TEST_OFF");
        HmiSerial.print("OK MIC_TEST_OFF\n");
    } else if (line == "MONITOR_ON") {
        recording_enabled = false;
        mic_test_enabled = false;
        monitor_enabled = true;
        Serial.println("OK MONITOR_ON");
        Serial.println("Keep the microphone away from the speakers to avoid feedback.");
        HmiSerial.print("OK MONITOR_ON\n");
    } else if (line == "MONITOR_OFF") {
        monitor_enabled = false;
        Serial.println("OK MONITOR_OFF");
        HmiSerial.print("OK MONITOR_OFF\n");
    } else if (line.startsWith("PLAY_URL ")) {
        playUrl(line.substring(9));
    } else if (line.length() > 0) {
        HmiSerial.print("ERR unknown command\n");
    }
}

void setup()
{
    Serial.begin(115200);
    Serial.setTimeout(20);
    HmiSerial.begin(HMI_BAUD, SERIAL_8N1, HMI_RX, HMI_TX);
    HmiSerial.setTimeout(20);
    setupMic();
    setupSpeaker();
    Serial.println("Audio board ready");
    Serial.println("Commands: MIC_TEST_ON, MIC_TEST_OFF, MONITOR_ON, MONITOR_OFF, REC_START, REC_STOP");
    HmiSerial.print("OK READY\n");
}

void loop()
{
    if (Serial.available()) {
        String line = Serial.readStringUntil('\n');
        handleCommand(line);
    }

    while (HmiSerial.available()) {
        uint8_t b = HmiSerial.read();
        if (!isPrintableCommandByte(b)) {
            dropped_uart_bytes++;
            hmi_line = "";
            continue;
        }

        if (b == '\n') {
            hmi_line.trim();
            if (hmi_line.length() > 0) {
                if (isKnownCommandPrefix(hmi_line)) {
                    Serial.print("[UART1 CMD] ");
                    Serial.println(hmi_line);
                    handleCommand(hmi_line);
                } else {
                    Serial.print("[UART1 IGNORE] ");
                    Serial.println(hmi_line);
                }
            }
            hmi_line = "";
        } else if (b != '\r') {
            if (hmi_line.length() < 160) {
                hmi_line += (char)b;
            } else {
                hmi_line = "";
            }
        }
    }

    if (dropped_uart_bytes >= 128) {
        Serial.printf("[UART1 DROP] %lu non-command bytes. Check wiring/baud/loopback.\n", (unsigned long)dropped_uart_bytes);
        dropped_uart_bytes = 0;
    }

    if (mic_test_enabled && millis() - last_mic_test_ms >= 100) {
        last_mic_test_ms = millis();
        printMicLevel();
    }

    if (recording_enabled) {
        streamMicToHmi();
    }

    if (monitor_enabled) {
        monitorMicToSpeaker();
    }

    delay(5);
}

