# Firmware Architecture

Thu muc nay chua skeleton code cho 2 board:

```text
firmware/
  esp32s3_lcd_client/      Board Waveshare ESP32-S3 LCD7, WiFi + UI + HTTP server client
  esp32_devkit_audio/      ESP32 DevKit V1, INMP441 record + MAX98357A playback
```

## UART Protocol

ESP32-S3 la controller. ESP32 DevKit V1 la audio peripheral.

Pin theo so do cua ban:

```text
ESP32 DevKit V1 GPIO16 (RX) <- HMI_TX tu ESP32-S3
ESP32 DevKit V1 GPIO17 (TX) -> HMI_RX tu ESP32-S3
GND noi chung
```

Protocol dang line-based de debug de hon:

```text
REC_START\n
REC_STOP\n
PLAY_URL http://server:8000/api/v1/audio/reply_x.wav\n
PING\n
```

Response:

```text
OK READY\n
OK RECORDING\n
OK STOPPED\n
OK PLAYING\n
ERR message\n
```

Khi nhan `REC_START`, board audio se stream PCM16 mono 16 kHz lien tuc ve ESP32-S3 theo frame nhi phan:

```text
AUD0 + uint16_length_little_endian + pcm_payload
```

ESP32-S3 gom cac frame nay den khi UI bam mic lan nua, gui `REC_STOP`, dong goi thanh WAV va upload:

```text
POST http://54.206.118.226:8000/api/v1/chat/voice
```

Text chat tren HMI goi:

```text
POST http://54.206.118.226:8000/api/v1/chat/text
```

Pin UART dang dat trong sketch LCD (UART1, remap GPIO):

```text
ESP32-S3 LCD GPIO16 TX -> ESP32 DevKit GPIO16 RX
ESP32-S3 LCD GPIO15 RX <- ESP32 DevKit GPIO17 TX
GND chung
```

GPIO17/GPIO18 tren board LCD-7 bi trung voi bus data RGB LCD (chay lien tuc), khong dung duoc cho UART.
Neu ban muon doi sang cap GPIO khac, sua `AUDIO_UART_RX` va `AUDIO_UART_TX` trong `09_lvgl_Porting/09_lvgl_Porting.ino`.

