from __future__ import annotations

from typing import Optional, Protocol


class LlmPort(Protocol):
    async def generate_reply(self, message: str, session_id: str, history: Optional[list[dict]] = None) -> str:
        raise NotImplementedError

