from dataclasses import dataclass, field
from datetime import datetime, timezone
from uuid import uuid4


@dataclass(frozen=True)
class ChatMessage:
    role: str
    content: str
    created_at: datetime = field(default_factory=lambda: datetime.now(timezone.utc))


@dataclass
class ChatSession:
    id: str = field(default_factory=lambda: str(uuid4()))
    messages: list[ChatMessage] = field(default_factory=list)

    def add_user_message(self, content: str) -> None:
        self.messages.append(ChatMessage(role="user", content=content))

    def add_assistant_message(self, content: str) -> None:
        self.messages.append(ChatMessage(role="assistant", content=content))

