from __future__ import annotations

from typing import Optional, Protocol


class TextToSpeechPort(Protocol):
    async def synthesize(self, text: str) -> Optional[bytes]:
        raise NotImplementedError

