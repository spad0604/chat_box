from fastapi import APIRouter, Depends, HTTPException, Query

from app.api.v1.schemas.admin import (
    TopQuestionResponse,
    UnansweredQuestionResponse,
    UsageStatsResponse,
)
from app.api.v1.schemas.chat import ChatMessageResponse, ChatSessionResponse
from app.infrastructure.dependencies import get_admin_store, get_chat_store
from app.infrastructure.storage.admin_store import PostgresAdminStore
from app.ports.chat_store import ChatStorePort

router = APIRouter()


@router.get("/chat/sessions", response_model=list[ChatSessionResponse])
async def admin_chat_sessions(
    limit: int = Query(default=100, ge=1, le=500),
    chat_store: ChatStorePort = Depends(get_chat_store),
) -> list[ChatSessionResponse]:
    sessions = await chat_store.get_sessions(limit=limit)
    return [ChatSessionResponse.model_validate(item) for item in sessions]


@router.get("/chat/history/{session_id}", response_model=list[ChatMessageResponse])
async def admin_chat_history(
    session_id: str,
    limit: int = Query(default=100, ge=1, le=500),
    chat_store: ChatStorePort = Depends(get_chat_store),
) -> list[ChatMessageResponse]:
    messages = await chat_store.get_messages(session_id=session_id, limit=limit)
    if not messages:
        raise HTTPException(status_code=404, detail="Chat session not found")
    return [ChatMessageResponse.model_validate(item) for item in messages]


@router.get("/stats/usage", response_model=UsageStatsResponse)
async def admin_usage_stats(
    period: str = Query(default="day", pattern="^(day|week|month)$"),
    store: PostgresAdminStore = Depends(get_admin_store),
) -> UsageStatsResponse:
    stats = await store.usage_stats(period=period)
    return UsageStatsResponse.model_validate(stats)


@router.get("/stats/top-questions", response_model=list[TopQuestionResponse])
async def admin_top_questions(
    limit: int = Query(default=20, ge=1, le=100),
    store: PostgresAdminStore = Depends(get_admin_store),
) -> list[TopQuestionResponse]:
    questions = await store.top_questions(limit=limit)
    return [TopQuestionResponse.model_validate(item) for item in questions]


@router.get("/stats/unanswered", response_model=list[UnansweredQuestionResponse])
async def admin_unanswered_questions(
    limit: int = Query(default=50, ge=1, le=200),
    store: PostgresAdminStore = Depends(get_admin_store),
) -> list[UnansweredQuestionResponse]:
    questions = await store.unanswered_questions(limit=limit)
    return [UnansweredQuestionResponse.model_validate(item) for item in questions]

