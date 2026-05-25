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
ESP32 DevKit V1 RX2 GPIO16  <- HMI_TX tu ESP32-S3
ESP32 DevKit V1 TX2 GPIO17  -> HMI_RX tu ESP32-S3
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

## Voice Path De Xuat

Ban co 2 cach:

### Cach A: DevKit cung co WiFi

DevKit ghi am va upload truc tiep len server. Cach nay tot hon cho audio dai.

### Cach B: DevKit chi la audio peripheral

DevKit gui audio PCM ve ESP32-S3 qua UART, ESP32-S3 upload len server. Cach nay dung dung vai tro "ESP32-S3 connect WiFi, giao tiep server", nhung chi nen ghi am ngan.

Skeleton hien tai dat khung cho Cach B, phan frame PCM can hoan thien theo buffer size va baudrate that te.

