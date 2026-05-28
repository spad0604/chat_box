from pathlib import Path
from uuid import uuid4

from fastapi import HTTPException

from app.ports.audio_storage import AudioStoragePort


class LocalAudioStorage(AudioStoragePort):
    def __init__(self, storage_dir: str, public_base_url: str) -> None:
        self._storage_dir = Path(storage_dir)
        self._storage_dir.mkdir(parents=True, exist_ok=True)
        self._public_base_url = public_base_url.rstrip("/")

    def save_wav(self, audio_bytes: bytes) -> str:
        filename = f"reply_{uuid4().hex}{detect_audio_extension(audio_bytes)}"
        path = self._storage_dir / filename
        path.write_bytes(audio_bytes)
        return filename

    def resolve(self, filename: str) -> Path:
        safe_name = Path(filename).name
        path = self._storage_dir / safe_name
        if not path.exists() or not path.is_file():
            raise HTTPException(status_code=404, detail="Audio file not found")
        return path

    def public_url(self, filename: str) -> str:
        return f"{self._public_base_url}/api/v1/audio/{filename}"


def detect_audio_extension(audio_bytes: bytes) -> str:
    if audio_bytes.startswith(b"RIFF") and audio_bytes[8:12] == b"WAVE":
        return ".wav"
    if audio_bytes.startswith(b"ID3") or audio_bytes[:2] in (b"\xff\xfb", b"\xff\xf3", b"\xff\xf2"):
        return ".mp3"
    return ".bin"

