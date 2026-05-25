from typing import Protocol


class LlmPort(Protocol):
    async def generate_reply(self, message: str, session_id: str) -> str:
        raise NotImplementedError

