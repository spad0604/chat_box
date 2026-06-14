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
WIFI_CREDS_V1 <percent_encoded_ssid> <percent_encoded_password>\n
```

Response:

```text
OK READY\n
OK RECORDING\n
OK STOPPED\n
OK PLAYING\n
ERR message\n
```

`WIFI_CREDS_V1` duoc ESP32-S3 gui sau khi bat duoc WiFi va lap lai moi 30 giay khi khong record.
ESP32 DevKit V1 so sanh voi credential dang luu trong NVS `Preferences("wifi")`.
Neu SSID/password khac, DevKit luu lai va tra:

```text
OK WIFI_CREDS_SAVED_REBOOTING\n
```

sau do tu reboot de connect lai bang WiFi giong ESP32-S3. Neu da giong nhau, DevKit tra:

```text
OK WIFI_CREDS_SAME\n
```

Password rong duoc encode thanh `%00`; cac ky tu dac biet/space trong SSID/password duoc percent-encode.

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

Pin UART dang dat trong sketch LCD:

```text
ESP32-S3 LCD UART TX GPIO43 -> ESP32 DevKit GPIO16 RX2
ESP32-S3 LCD UART RX GPIO44 <- ESP32 DevKit GPIO17 TX2
GND chung
```

GPIO17/GPIO18 tren board LCD-7 bi trung voi bus data RGB LCD (chay lien tuc), khong dung duoc cho UART.
Neu ban muon doi sang cap GPIO khac, sua `AUDIO_UART_RX` va `AUDIO_UART_TX` trong `09_lvgl_Porting/09_lvgl_Porting.ino`.

