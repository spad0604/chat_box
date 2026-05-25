from fastapi import APIRouter, Depends, File, Form, UploadFile

from app.api.v1.schemas.chat import ChatTextRequest, ChatResponse, VoiceChatResponse
from app.application.chat_service import ChatService
from app.infrastructure.dependencies import get_chat_service

router = APIRouter()


@router.post("/text", response_model=ChatResponse)
async def chat_text(
    request: ChatTextRequest,
    service: ChatService = Depends(get_chat_service),
) -> ChatResponse:
    result = await service.chat_text(
        message=request.message,
        session_id=request.session_id,
    )
    return ChatResponse.model_validate(result)


@router.post("/voice", response_model=VoiceChatResponse)
async def chat_voice(
    file: UploadFile = File(...),
    session_id: str | None = Form(default=None),
    service: ChatService = Depends(get_chat_service),
) -> VoiceChatResponse:
    audio_bytes = await file.read()
    result = await service.chat_voice(
        audio_bytes=audio_bytes,
        filename=file.filename or "recording.wav",
        content_type=file.content_type or "application/octet-stream",
        session_id=session_id,
    )
    return VoiceChatResponse.model_validate(result)

