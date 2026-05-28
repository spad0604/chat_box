from __future__ import annotations

from typing import Optional

from pydantic import BaseModel, Field


class ChatTextRequest(BaseModel):
    message: str = Field(min_length=1, max_length=4096)
    session_id: Optional[str] = None


class ChatResponse(BaseModel):
    session_id: str
    input_text: str
    reply_text: str
    audio_url: Optional[str] = None


class VoiceChatResponse(ChatResponse):
    transcript: str


class ChatMessageResponse(BaseModel):
    role: str
    content: str
    created_at: str


class ChatSessionResponse(BaseModel):
    session_id: str
    updated_at: str
    last_message: str

