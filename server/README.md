# ChatBox Server Architecture

- ESP32-S3 LCD7: hien thi UI, connect WiFi, goi HTTP API toi server, dieu khien ESP32 DevKit V1 qua UART.
- ESP32 DevKit V1: thu am tu INMP441 I2S va phat audio ra MAX98357A.
- Server: nhan text/audio, goi STT/LLM/TTS provider, tra ve text va audio URL.

## Data Flow

### Text chat

1. ESP32-S3 nhan text tu UI.
2. ESP32-S3 `POST /api/v1/chat/text`.
3. Server goi LLM provider.
4. Server goi TTS provider de tao audio reply.
5. Server tra ve `reply_text` va `audio_url`.
6. ESP32-S3 gui lenh UART cho ESP32 DevKit V1 phat `audio_url`.

### Voice chat

1. ESP32-S3 nhan nut Mic.
2. ESP32-S3 gui UART `REC_START` cho ESP32 DevKit V1.
3. ESP32 DevKit V1 thu PCM tu INMP441 va gui frame audio ve ESP32-S3 qua UART.
4. ESP32-S3 upload file audio len `POST /api/v1/chat/voice`.
5. Server goi STT provider de chuyen speech thanh text.
6. Server goi LLM provider.
7. Server goi TTS provider.
8. Server tra ve `transcript`, `reply_text`, `audio_url`.
9. ESP32-S3 hien thi text va gui UART `PLAY_URL` cho ESP32 DevKit V1 phat loa.


## Layer Architecture

```text
server/app
  api/                  HTTP layer: routes, request/response schemas
  application/          Use cases: chat orchestration
  domain/               Entity/model thuan business
  ports/                Interface cho LLM/STT/TTS/storage
  infrastructure/       Provider implementations, file storage, config
```

Dependency direction:

```text
api -> application -> domain
application -> ports
infrastructure -> ports
```

`application` khong phu thuoc FastAPI hay provider cu the. Muon doi OpenAI, Google, Azure, local model, chi thay implementation trong `infrastructure/providers`.

## Run

```powershell
cd server
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
copy .env.example .env
uvicorn app.main:app --host 0.0.0.0 --port 8000 --reload
```

Test:

```powershell
curl.exe -X POST http://localhost:8000/api/v1/chat/text `
  -H "Content-Type: application/json" `
  -d "{\"message\":\"Xin chao\",\"session_id\":\"demo\"}"
```

Mo docs:

```text
http://localhost:8000/docs
```

## Provider Configuration

Mac dinh server dung mock provider de chay duoc ngay:

```env
LLM_PROVIDER=mock
STT_PROVIDER=mock
TTS_PROVIDER=mock
```

Khi co API that, tao class moi trong:

```text
app/infrastructure/providers/
```

roi cap nhat factory trong:

```text
app/infrastructure/dependencies.py
```

## API Summary

### `POST /api/v1/chat/text`

Request:

```json
{
  "message": "Xin chao",
  "session_id": "demo"
}
```

Response:

```json
{
  "session_id": "demo",
  "input_text": "Xin chao",
  "reply_text": "...",
  "audio_url": "/api/v1/audio/reply_x.wav"
}
```

### `POST /api/v1/chat/voice`

Multipart form:

- `file`: wav/pcm audio
- `session_id`: optional

Response co them `transcript`.

### `GET /api/v1/audio/{filename}`

Tra ve file audio TTS de ESP32 DevKit V1 download va phat.

