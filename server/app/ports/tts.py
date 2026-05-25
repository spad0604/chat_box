from typing import Protocol


class TextToSpeechPort(Protocol):
    async def synthesize(self, text: str) -> bytes | None:
        raise NotImplementedError

