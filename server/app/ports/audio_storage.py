from pathlib import Path
from typing import Protocol


class AudioStoragePort(Protocol):
    def save_wav(self, audio_bytes: bytes) -> str:
        raise NotImplementedError

    def resolve(self, filename: str) -> Path:
        raise NotImplementedError

    def public_url(self, filename: str) -> str:
        raise NotImplementedError

