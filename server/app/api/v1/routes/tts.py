from fastapi import APIRouter, Depends

from app.api.v1.schemas.tts import TtsRequest, TtsResponse
from app.infrastructure.config import settings
from app.infrastructure.providers.fpt_tts import FptTtsProvider
from app.infrastructure.dependencies import get_audio_storage
from app.ports.audio_storage import AudioStoragePort

router = APIRouter()


def create_fpt_tts_provider(request: TtsRequest) -> FptTtsProvider:
    return FptTtsProvider(
        api_key=settings.fpt_tts_api_key or settings.fpt_asr_api_key,
        url=settings.fpt_tts_url,
        voice=request.voice or settings.fpt_tts_voice,
        speed=settings.fpt_tts_speed if request.speed is None else request.speed,
        poll_attempts=settings.fpt_tts_poll_attempts,
        poll_interval_seconds=settings.fpt_tts_poll_interval_seconds,
    )


@router.post("/fpt", response_model=TtsResponse)
async def synthesize_with_fpt(
    request: TtsRequest,
    storage: AudioStoragePort = Depends(get_audio_storage),
) -> TtsResponse:
    provider = create_fpt_tts_provider(request)
    audio = await provider.synthesize(request.text)
    if audio is None:
        audio = b""

    filename = storage.save_wav(audio)
    return TtsResponse(
        text=request.text,
        voice=request.voice or settings.fpt_tts_voice,
        speed=settings.fpt_tts_speed if request.speed is None else request.speed,
        audio_url=storage.public_url(filename),
        size=len(audio),
    )
