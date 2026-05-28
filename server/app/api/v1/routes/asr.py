from fastapi import APIRouter, Depends, File, UploadFile

from app.api.v1.schemas.asr import AsrResponse
from app.infrastructure.providers.fpt_asr import FptAsrProvider, extract_transcript
from app.infrastructure.config import settings

router = APIRouter()


def get_fpt_asr_provider() -> FptAsrProvider:
    return FptAsrProvider(
        api_key=settings.fpt_asr_api_key,
        url=settings.fpt_asr_url,
    )


@router.post("/fpt", response_model=AsrResponse)
async def transcribe_with_fpt(
    file: UploadFile = File(...),
    provider: FptAsrProvider = Depends(get_fpt_asr_provider),
) -> AsrResponse:
    audio_bytes = await file.read()
    content_type = file.content_type or "application/octet-stream"
    raw = await provider.transcribe_raw(
        audio_bytes=audio_bytes,
        filename=file.filename or "recording.wav",
        content_type=content_type,
    )
    return AsrResponse(
        filename=file.filename or "recording.wav",
        content_type=content_type,
        size=len(audio_bytes),
        transcript=extract_transcript(raw),
        raw=raw,
    )
