from typing import Protocol


class SpeechToTextPort(Protocol):
    async def transcribe(self, audio_bytes: bytes, filename: str, content_type: str) -> str:
        raise NotImplementedError

