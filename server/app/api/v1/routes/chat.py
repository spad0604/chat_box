from __future__ import annotations

from typing import Optional

from fastapi import APIRouter, Depends, File, Form, HTTPException, UploadFile

from app.api.v1.schemas.chat import (
    ChatMessageResponse,
    ChatResponse,
    ChatSessionResponse,
    ChatTextRequest,
    VoiceChatResponse,
)
from app.application.chat_service import ChatService
from app.infrastructure.dependencies import get_chat_service
from app.infrastructure.providers.openai_llm import OpenAiProviderError

router = APIRouter()


@router.get("/sessions", response_model=list[ChatSessionResponse])
async def list_chat_sessions(
    limit: int = 50,
    service: ChatService = Depends(get_chat_service),
) -> list[ChatSessionResponse]:
    sessions = await service.get_sessions(limit=limit)
    return [ChatSessionResponse.model_validate(item) for item in sessions]


@router.get("/history/{session_id}", response_model=list[ChatMessageResponse])
async def get_chat_history(
    session_id: str,
    limit: int = 50,
    service: ChatService = Depends(get_chat_service),
) -> list[ChatMessageResponse]:
    messages = await service.get_history(session_id=session_id, limit=limit)
    return [ChatMessageResponse.model_validate(item) for item in messages]


@router.post("/text", response_model=ChatResponse)
async def chat_text(
    request: ChatTextRequest,
    service: ChatService = Depends(get_chat_service),
) -> ChatResponse:
    try:
        result = await service.chat_text(
            message=request.message,
            session_id=request.session_id,
        )
    except OpenAiProviderError as exc:
        raise HTTPException(status_code=exc.status_code, detail=exc.message) from exc
    return ChatResponse.model_validate(result)


@router.post("/voice", response_model=VoiceChatResponse)
async def chat_voice(
    file: UploadFile = File(...),
    session_id: Optional[str] = Form(default=None),
    service: ChatService = Depends(get_chat_service),
) -> VoiceChatResponse:
    audio_bytes = await file.read()
    try:
        result = await service.chat_voice(
            audio_bytes=audio_bytes,
            filename=file.filename or "recording.wav",
            content_type=file.content_type or "application/octet-stream",
            session_id=session_id,
        )
    except OpenAiProviderError as exc:
        raise HTTPException(status_code=exc.status_code, detail=exc.message) from exc
    return VoiceChatResponse.model_validate(result)

