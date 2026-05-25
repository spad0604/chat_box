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

    i2s_driver_install(I2S_MIC_PORT, &config, 0, NULL);
    i2s_set_pin(I2S_MIC_PORT, &pins);
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

static void startRecording()
{
    HmiSerial.print("OK RECORDING\n");

    // TODO: Doc I2S tu INMP441, convert 32-bit sample sang 16-bit PCM,
    // dong goi frame va gui ve ESP32-S3 qua UART.
}

static void stopRecording()
{
    HmiSerial.print("OK STOPPED\n");
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
    } else if (line.startsWith("PLAY_URL ")) {
        playUrl(line.substring(9));
    } else if (line.length() > 0) {
        HmiSerial.print("ERR unknown command\n");
    }
}

void setup()
{
    Serial.begin(115200);
    HmiSerial.begin(HMI_BAUD, SERIAL_8N1, HMI_RX, HMI_TX);
    setupMic();
    setupSpeaker();
    HmiSerial.print("OK READY\n");
}

void loop()
{
    if (HmiSerial.available()) {
        String line = HmiSerial.readStringUntil('\n');
        handleCommand(line);
    }
    delay(5);
}

