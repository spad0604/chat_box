from typing import Protocol


class ChatStorePort(Protocol):
    async def append_message(self, session_id: str, role: str, content: str) -> None:
        raise NotImplementedError

    async def get_messages(self, session_id: str, limit: int = 20) -> list[dict]:
        raise NotImplementedError

    async def get_sessions(self, limit: int = 50) -> list[dict]:
        raise NotImplementedError
