from collections import defaultdict
from datetime import datetime, timezone

from app.ports.chat_store import ChatStorePort


class MemoryChatStore(ChatStorePort):
    def __init__(self) -> None:
        self._messages: dict[str, list[dict]] = defaultdict(list)

    async def append_message(self, session_id: str, role: str, content: str) -> None:
        self._messages[session_id].append(
            {
                "session_id": session_id,
                "role": role,
                "content": content,
                "created_at": datetime.now(timezone.utc).isoformat(),
            }
        )

    async def get_messages(self, session_id: str, limit: int = 20) -> list[dict]:
        return self._messages.get(session_id, [])[-limit:]

    async def get_sessions(self, limit: int = 50) -> list[dict]:
        sessions = []
        for session_id, messages in self._messages.items():
            if not messages:
                continue
            sessions.append(
                {
                    "session_id": session_id,
                    "updated_at": messages[-1]["created_at"],
                    "last_message": messages[-1]["content"],
                }
            )
        sessions.sort(key=lambda item: item["updated_at"], reverse=True)
        return sessions[:limit]
