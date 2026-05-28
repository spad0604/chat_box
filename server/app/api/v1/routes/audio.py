from fastapi import APIRouter, Depends
from fastapi.responses import FileResponse

from app.ports.audio_storage import AudioStoragePort
from app.infrastructure.dependencies import get_audio_storage

router = APIRouter()


MEDIA_TYPES = {
    ".mp3": "audio/mpeg",
    ".wav": "audio/wav",
}


@router.get("/{filename}")
async def get_audio_file(
    filename: str,
    storage: AudioStoragePort = Depends(get_audio_storage),
) -> FileResponse:
    path = storage.resolve(filename)
    media_type = MEDIA_TYPES.get(path.suffix.lower(), "application/octet-stream")
    return FileResponse(path, media_type=media_type, filename=filename)

