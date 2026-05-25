from fastapi import APIRouter, Depends
from fastapi.responses import FileResponse

from app.ports.audio_storage import AudioStoragePort
from app.infrastructure.dependencies import get_audio_storage

router = APIRouter()


@router.get("/{filename}")
async def get_audio_file(
    filename: str,
    storage: AudioStoragePort = Depends(get_audio_storage),
) -> FileResponse:
    path = storage.resolve(filename)
    return FileResponse(path, media_type="audio/wav", filename=filename)

