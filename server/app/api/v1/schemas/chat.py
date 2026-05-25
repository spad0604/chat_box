from pydantic import BaseModel, Field


class ChatTextRequest(BaseModel):
    message: str = Field(min_length=1, max_length=4096)
    session_id: str | None = None


class ChatResponse(BaseModel):
    session_id: str
    input_text: str
    reply_text: str
    audio_url: str | None = None


class VoiceChatResponse(ChatResponse):
    transcript: str

